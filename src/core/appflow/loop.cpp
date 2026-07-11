#include "loop.hpp"

#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../communication/rpcserver.hpp"
#include "../os/inputstate.hpp"
#include "../os/window.hpp"
#include "../playback/vatplayer.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../startup.hpp"
#include "../userpublic/userinput.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../vkcore/rendertiming.hpp"
#include "enginetime.hpp"
#include "framephase.hpp"
#include "framerate.hpp"

#include <filesystem>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {

namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>{end - start}.count();
}

struct RenderOutPattern {
    bool has_frame_token = false;
    size_t token_pos = std::string::npos;
    size_t token_len = 0;
    int width = 0;
};

RenderOutPattern parseRenderOutPattern(const std::filesystem::path &path) {
    const auto pattern = path.string();
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            continue;
        }

        size_t pos = i + 1;
        bool zero_pad = false;
        if (pos < pattern.size() && pattern[pos] == '0') {
            zero_pad = true;
            ++pos;
        }

        int width = 0;
        while (pos < pattern.size() && pattern[pos] >= '0' && pattern[pos] <= '9') {
            width = width * 10 + (pattern[pos] - '0');
            ++pos;
        }

        if (pos < pattern.size() && pattern[pos] == 'd') {
            return RenderOutPattern{
                .has_frame_token = true,
                .token_pos = i,
                .token_len = pos - i + 1,
                .width = zero_pad ? width : 0,
            };
        }
    }
    return {};
}

std::filesystem::path formatRenderOutPath(const std::filesystem::path &path, const RenderOutPattern &pattern,
                                          uint32_t frame_number) {
    if (!pattern.has_frame_token) {
        return path;
    }

    std::ostringstream frame_stream;
    if (pattern.width > 0) {
        frame_stream << std::setw(pattern.width) << std::setfill('0');
    }
    frame_stream << frame_number;

    auto path_string = path.string();
    path_string.replace(pattern.token_pos, pattern.token_len, frame_stream.str());
    return std::filesystem::path{path_string};
}

void logInputSnapshotIfRequested(const InputSnapshot &snapshot) {
    if (!UserInput::isKeyPushed(KeyCode::F1)) {
        return;
    }

    LOG_INFO(logger, "input snapshot: down={} pushed={} released={} mouse=({}, {}) delta=({}, {})",
             snapshot.downCount(), snapshot.pushedCount(), snapshot.releasedCount(), snapshot.mouse_x,
             snapshot.mouse_y, snapshot.mouse_delta_x, snapshot.mouse_delta_y);
}

void dumpFramePlanIfRequested(const EngineLaunchConfig &launch_config, const Renderer &renderer) {
    if (!launch_config.dump_frame_plan) {
        return;
    }
    std::cerr << renderer.currentFramePlanJson().dump(2) << std::endl;
}

} // namespace

Loop::Loop() {}

void Loop::run() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    auto &renderer = GET_MODULE(Renderer);
    auto &engine_time = GET_MODULE(EngineTime);
    auto &vat_player = GET_MODULE(VatPlayer);
    auto &input_state = GET_MODULE(InputState);
    (void)vat_player;
    RenderTiming *render_timing =
        GET_MODULE(RenderingPassContainer).isFeatureEnabled("gpu_timing") ? &GET_MODULE(RenderTiming) : nullptr;

    const auto time_mode =
        launch_config.headless ? EngineTime::Mode::fixed_step : EngineTime::Mode::realtime;
    engine_time.setup(time_mode, 1.0 / launch_config.fps);
    dumpFramePlanIfRequested(launch_config, renderer);
    GET_MODULE(StartupMetrics).finishAndLog();

    LOG_INFO(logger, "starting main loop");

    if (launch_config.headless) {
        if (launch_config.rpc) {
            runEngineRpcServer(std::cin, std::cout);
            GET_MODULE(VulkanManageCore).waitIdle();
            GET_MODULE(DeletionQueue).flushAll();
            return;
        }

        const auto render_out_pattern =
            launch_config.render_out ? parseRenderOutPattern(*launch_config.render_out) : RenderOutPattern{};
        for (uint32_t frame = 0; launch_config.headless_frames == 0 || frame < launch_config.headless_frames;
             ++frame) {
            input_state.clear();
            const auto update_start = Clock::now();
            engine_time.advance();
            updateFrameState();
            const auto update_end = Clock::now();

            const auto render_start = Clock::now();
            renderer.render();
            const auto render_end = Clock::now();
            if (render_timing != nullptr) {
                render_timing->recordCpuFrame(CpuFrameDurations{
                    elapsedMs(update_start, update_end),
                    elapsedMs(render_start, render_end),
                    0.0,
                });
            }
            if (launch_config.render_out && render_out_pattern.has_frame_token) {
                GET_MODULE(RenderTarget)
                    .captureLastFrameToPng(formatRenderOutPath(*launch_config.render_out, render_out_pattern,
                                                               frame + 1));
            }
        }
        if (launch_config.render_out && !render_out_pattern.has_frame_token && launch_config.headless_frames > 0) {
            GET_MODULE(RenderTarget).captureLastFrameToPng(*launch_config.render_out);
        }
        GET_MODULE(VulkanManageCore).waitIdle();
        if (render_timing != nullptr) {
            render_timing->flush();
        }
        GET_MODULE(DeletionQueue).flushAll();
        return;
    }

    auto &window = GET_MODULE(Window);
    auto &framerate_adjuster = GET_MODULE(FramerateAdjust);

    while (true) {
        if (!window.process())
            break;
        input_state.queueEvents(window.drainInputEvents());
        const auto update_start = Clock::now();
        engine_time.advance();
        updateFrameState();
        logInputSnapshotIfRequested(input_state.currentSnapshot());
        const auto update_end = Clock::now();

        const auto render_start = Clock::now();
        renderer.render();
        const auto render_end = Clock::now();

        const auto wait_start = Clock::now();
        framerate_adjuster.wait();
        const auto wait_end = Clock::now();
        if (render_timing != nullptr) {
            render_timing->recordCpuFrame(CpuFrameDurations{
                elapsedMs(update_start, update_end),
                elapsedMs(render_start, render_end),
                elapsedMs(wait_start, wait_end),
            });
        }
    }

    GET_MODULE(VulkanManageCore).waitIdle();
    if (render_timing != nullptr) {
        render_timing->flush();
    }
    GET_MODULE(DeletionQueue).flushAll();
}

} // namespace Pelican
