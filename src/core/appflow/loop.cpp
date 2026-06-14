#include "loop.hpp"

#include "../ecs/core.hpp"
#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../os/window.hpp"
#include "../playback/seqplayer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/rendertarget.hpp"
#include "enginetime.hpp"
#include "framerate.hpp"

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {

namespace {

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

} // namespace

Loop::Loop() {}

void Loop::run() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    auto &renderer = GET_MODULE(Renderer);
    auto &ecs = GET_MODULE(ECSCore);
    auto &engine_time = GET_MODULE(EngineTime);
    auto &seq_player = GET_MODULE(SeqPlayer);

    const auto time_mode =
        launch_config.headless ? EngineTime::Mode::fixed_step : EngineTime::Mode::realtime;
    engine_time.setup(time_mode, 1.0 / launch_config.fps);

    LOG_INFO(logger, "starting main loop");

    if (launch_config.headless) {
        const auto render_out_pattern =
            launch_config.render_out ? parseRenderOutPattern(*launch_config.render_out) : RenderOutPattern{};
        for (uint32_t frame = 0; launch_config.headless_frames == 0 || frame < launch_config.headless_frames;
             ++frame) {
            engine_time.advance();
            ecs.update();
            seq_player.update(engine_time.now());
            renderer.render();
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
        GET_MODULE(DeletionQueue).flushAll();
        return;
    }

    auto &window = GET_MODULE(Window);
    auto &framerate_adjuster = GET_MODULE(FramerateAdjust);

    while (true) {
        if (!window.process())
            break;
        engine_time.advance();
        ecs.update();
        seq_player.update(engine_time.now());
        renderer.render();
        framerate_adjuster.wait();
    }

    GET_MODULE(VulkanManageCore).waitIdle();
    GET_MODULE(DeletionQueue).flushAll();
}

} // namespace Pelican
