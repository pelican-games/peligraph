#include "loop.hpp"

#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../communication/rpcserver.hpp"
#include "../build_features.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "../os/inputstate.hpp"
#include "../os/inputsequence.hpp"
#include "../os/window.hpp"
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrcompositiontarget.hpp"
#include "../openxr/openxrmirrorsink.hpp"
#include "../openxr/openxrsession.hpp"
#include "../openxr/openxrviewspace.hpp"
#endif
#include "../playback/camerabake.hpp"
#include "../playback/vatplayer.hpp"
#include "../persistence/persistence.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
#include "../renderer/debugtext.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/modaltransform.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderdoc/renderdoccapture.hpp"
#include "../startup.hpp"
#include "../ui/module.hpp"
#include "../userpublic/userinput.hpp"
#include "../userpublic/deterministicrng.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/renderer.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../vkcore/rendertiming.hpp"
#include "../watch/reloadservice.hpp"
#include "enginetime.hpp"
#include "framephase.hpp"
#include "framerate.hpp"

#include <filesystem>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
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

struct LoopModules {
    const EngineLaunchConfig &launch_config;
    RenderDocCapture &renderdoc_capture;
    Renderer &renderer;
    VulkanManageCore &vulkan;
    EngineTime &engine_time;
    VatPlayer &vat_player;
    InputState &input_state;
    InputSequenceRuntime &input_sequence;
    watch::ReloadService &reload_service;
    RenderTiming *render_timing;
    CameraBakeRecorder *camera_bake;
    StartupMetrics &startup_metrics;
};

LoopModules resolveLoopModules() {
    const auto &launch_config = GET_MODULE(EngineLaunchConfig);
    // Resolve the passive API before Renderer can create the Vulkan instance.
    // This never loads RenderDoc; it only observes an already injected module.
    auto &renderdoc_capture = GET_MODULE(RenderDocCapture);
    auto &renderer = GET_MODULE(Renderer);
    auto &rendering_passes = GET_MODULE(RenderingPassContainer);
    // UI input routing precedes rendering, so create the purgeable CPU runtime
    // during loop setup when (and only when) the UI feature is enabled. Lazy
    // creation from the first render would drop first-frame pointer events.
    if (rendering_passes.isFeatureEnabled("ui")) (void)GET_MODULE(ui::UiModule);

    auto &engine_time = GET_MODULE(EngineTime);
    auto &vat_player = GET_MODULE(VatPlayer);
    auto &input_state = GET_MODULE(InputState);
    auto &input_sequence = GET_MODULE(InputSequenceRuntime);
    auto *render_timing = rendering_passes.isFeatureEnabled("gpu_timing")
                              ? &GET_MODULE(RenderTiming)
                              : nullptr;
    auto *camera_bake = FastModuleContainer::tryGet<CameraBakeRecorder>();
    if (launch_config.camera_bake_output && camera_bake == nullptr)
        camera_bake = &GET_MODULE(CameraBakeRecorder);

    return {
        launch_config,
        renderdoc_capture,
        renderer,
        GET_MODULE(VulkanManageCore),
        engine_time,
        vat_player,
        input_state,
        input_sequence,
        GET_MODULE(watch::ReloadService),
        render_timing,
        camera_bake,
        GET_MODULE(StartupMetrics),
    };
}

struct InteractiveLoopModules {
    Window &window;
    FramerateAdjust &framerate_adjuster;
};

InteractiveLoopModules resolveInteractiveLoopModules() {
    return {GET_MODULE(Window), GET_MODULE(FramerateAdjust)};
}

struct GpuDrainModules {
    VulkanManageCore &vulkan;
    DeletionQueue &deletion_queue;
};

GpuDrainModules resolveGpuDrainModules() {
    return {GET_MODULE(VulkanManageCore), GET_MODULE(DeletionQueue)};
}

RenderTarget &resolveOutputRenderTarget() {
    return GET_MODULE(RenderTarget);
}

#if PELICAN_WITH_OPENXR
OpenXr::XrSessionDependencies resolveXrSessionDependencies() {
    auto &discovery = GET_MODULE(OpenXr::DiscoveryRuntime);
    auto &vulkan = GET_MODULE(VulkanManageCore);
    return {
        .get_instance_proc_addr = discovery.getInstanceProcAddr(),
        .instance = discovery.getInstance(),
        .system_id = discovery.getSystemId(),
        .vulkan_instance = static_cast<VkInstance>(vulkan.getInstance()),
        .vulkan_physical_device = static_cast<VkPhysicalDevice>(vulkan.getPhysDevice()),
        .vulkan_device = static_cast<VkDevice>(vulkan.getDevice()),
        .graphics_queue_family_index = vulkan.getGraphicsQueueFamilyIndex(),
        .graphics_queue_index = 0,
        .input_actions = internal::inputActionMap(),
        .win32_time_conversion_enabled = discovery.win32TimeConversionEnabled(),
    };
}

OpenXr::XrCompositionDependencies resolveXrCompositionDependencies(
    OpenXr::SessionRuntime &session) {
    auto &discovery = GET_MODULE(OpenXr::DiscoveryRuntime);
    return {
        .get_instance_proc_addr = discovery.getInstanceProcAddr(),
        .session_runtime = &session,
        .vulkan = &GET_MODULE(VulkanManageCore),
        .renderer_color_format = GET_MODULE(RenderTarget).getSwapchainFormat(),
        .renderer_depth_format =
            GET_MODULE(Renderer)
                .xrCompositionDepthFormat()
                .value_or(vk::Format::eUndefined),
        .composition_layer_depth_enabled =
            discovery.compositionLayerDepthEnabled(),
    };
}
#endif

void prepareRuntimeModuleGraph(LoopModules &modules) {
    // The composition root owns all first construction. Runtime-facing
    // facades may keep using GET_MODULE, but only as reads after this point.
    prepareFrameStateModules();
    internal::prepareInputActionsRuntime();
    modules.renderer.prepareRuntimeModules();

    // Public GameContext services are legal at any point in game code, so
    // their modules must exist before the graph is frozen even when the
    // initial scene does not happen to exercise them.
#if PELICAN_WITH_PHYSICS
    (void)GET_MODULE(PhysWorld);
#endif
    (void)GET_MODULE(DeterministicRng);
    (void)GET_MODULE(DebugText);
    // The built-in modal transform system reads this on every frame. Keep its
    // state in the engine graph and construct it before creation is frozen.
    (void)GET_MODULE(ModalTransformState);
    (void)GET_MODULE(Persistence);
#if PELICAN_WITH_AUDIO
    (void)GET_MODULE(Audio);
#endif

    if (!modules.launch_config.headless) (void)resolveInteractiveLoopModules();
#if PELICAN_WITH_OPENXR
    if (modules.launch_config.xr_active) {
        OpenXr::setSessionDependencyProvider(&resolveXrSessionDependencies);
        (void)GET_MODULE(OpenXr::SessionRuntime);
    }
#endif
    (void)resolveGpuDrainModules();
}

void finishLoopResources(InputSequenceRuntime &input_sequence,
                         CameraBakeRecorder *camera_bake,
                         RenderTiming *render_timing,
                         RenderDocCapture &renderdoc_capture,
                         bool flush_timing) {
    renderdoc_capture.beginShutdown();
    auto gpu = resolveGpuDrainModules();
    gpu.vulkan.waitIdle();
    if (flush_timing && render_timing != nullptr) render_timing->flush();
    gpu.deletion_queue.flushAll();
    if (input_sequence.isRecording()) input_sequence.stopRecording();
    if (camera_bake == nullptr)
        camera_bake = FastModuleContainer::tryGet<CameraBakeRecorder>();
    if (camera_bake != nullptr && camera_bake->isActive()) camera_bake->finish();
}

void requestF11CaptureIfNeeded(RenderDocCapture &capture, bool xr_active) {
    if (!UserInput::isKeyPushed(KeyCode::F11) || !capture.available()) return;
    try {
        capture.request(RenderDocCaptureSource::f11, xr_active);
        LOG_INFO(logger, "RenderDoc F11 capture armed");
    } catch (const RenderDocCaptureError &error) {
        LOG_ERROR(logger, "RenderDoc F11 capture rejected: {}", error.what());
    }
}

void renderFlatFrameWithOptionalCapture(LoopModules &modules, Window &window) {
    auto &capture = modules.renderdoc_capture;
    if (capture.state() != RenderDocCaptureState::armed) {
        modules.renderer.render();
        return;
    }

    bool rendered = false;
    try {
        const auto raw_instance = reinterpret_cast<void *>(
            static_cast<VkInstance>(modules.vulkan.getInstance()));
        const auto result = capture.captureArmedFrame(
            modules.engine_time.frameIndex(),
            renderDocDevicePointerFromVulkanInstance(raw_instance),
            window.renderDocCaptureHandle(), [&] {
                rendered = true;
                modules.renderer.render();
            });
        LOG_INFO(logger, "RenderDoc F11 capture complete: index={} frame={} path={}",
                 result.capture_index, result.frame_index, result.path.string());
    } catch (const RenderDocCaptureError &error) {
        LOG_ERROR(logger, "RenderDoc F11 capture failed: {}", error.what());
        // A failed capture must not suppress the logical frame. Do not render a
        // second time if EndFrameCapture failed after rendering completed.
        if (!rendered) modules.renderer.render();
    }
}

} // namespace

Loop::Loop() {}

void Loop::run() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    auto modules = resolveLoopModules();
    const auto &launch_config = modules.launch_config;
    auto &renderer = modules.renderer;
    auto &engine_time = modules.engine_time;
    auto &input_state = modules.input_state;
    auto &input_sequence = modules.input_sequence;
    auto *render_timing = modules.render_timing;
    (void)modules.vat_player;

    const auto pose_action_names = internal::poseInputActionNames();
    if (launch_config.input_replay_path) {
        input_sequence.startReplay(*launch_config.input_replay_path, pose_action_names);
    } else if (launch_config.input_record) {
        input_sequence.startRecording(*launch_config.input_record, launch_config.fps,
                                      pose_action_names);
    }
    const auto time_mode = launch_config.headless || input_sequence.isReplaying()
                               ? EngineTime::Mode::fixed_step
                               : EngineTime::Mode::realtime;
    const auto timeline_fps = input_sequence.isReplaying() ? input_sequence.replayFps() : launch_config.fps;
    engine_time.setup(time_mode, 1.0 / timeline_fps);
    if (launch_config.camera_bake_output) {
        modules.camera_bake->start(*launch_config.camera_bake_output, input_sequence.replayFps());
    }
    prepareRuntimeModuleGraph(modules);
#if PELICAN_WITH_OPENXR
    std::unique_ptr<OpenXr::XrCompositionTarget> xr_composition_target;
    std::unique_ptr<OpenXr::XrMirrorSink> xr_mirror_sink;
    if (launch_config.xr_active) {
        auto &session = GET_MODULE(OpenXr::SessionRuntime);
        xr_composition_target = std::make_unique<OpenXr::XrCompositionTarget>(
            resolveXrCompositionDependencies(session));
        xr_mirror_sink = std::make_unique<OpenXr::XrMirrorSink>();
    }
#endif
    dumpFramePlanIfRequested(launch_config, renderer);
    modules.startup_metrics.finishAndLog();
    FastModuleContainer::freezeCreation();
    const auto module_graph = FastModuleContainer::graphSnapshot();
    LOG_INFO(logger, "runtime module graph frozen: modules={} dependencies={}",
             module_graph.initialized_modules.size(), module_graph.dependencies.size());

    LOG_INFO(logger, "starting main loop");

    if (launch_config.headless) {
        if (launch_config.rpc) {
            runEngineRpcServer(std::cin, std::cout);
            finishLoopResources(input_sequence, modules.camera_bake, render_timing,
                                modules.renderdoc_capture, false);
            return;
        }

        const auto render_out_pattern =
            launch_config.render_out ? parseRenderOutPattern(*launch_config.render_out) : RenderOutPattern{};
        const auto replay_frames = input_sequence.isReplaying() ? input_sequence.replayFrameCount() : 0;
        const auto frame_limit = input_sequence.isReplaying() && !launch_config.headless_frames_explicit
                                     ? static_cast<uint32_t>(replay_frames)
                                     : launch_config.headless_frames;
        for (uint32_t frame = 0; frame_limit == 0 || frame < frame_limit;
             ++frame) {
            if (!input_sequence.isReplaying()) {
                input_state.clear();
            }
            const auto update_start = Clock::now();
            engine_time.advance();
            modules.vulkan.setCurrentFrameIndex(engine_time.frameIndex());
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
                resolveOutputRenderTarget()
                    .captureLastFrameToPng(formatRenderOutPath(*launch_config.render_out, render_out_pattern,
                                                               frame + 1));
            }
        }
        if (launch_config.render_out && !render_out_pattern.has_frame_token && launch_config.headless_frames > 0) {
            resolveOutputRenderTarget().captureLastFrameToPng(*launch_config.render_out);
        }
        finishLoopResources(input_sequence, modules.camera_bake, render_timing,
                            modules.renderdoc_capture, true);
        return;
    }

#if PELICAN_WITH_RPC
    std::unique_ptr<EngineRpcEndpoint> windowed_rpc_endpoint;
    std::unique_ptr<WindowedRpcHost> windowed_rpc_host;
    if (launch_config.rpc) {
        windowed_rpc_endpoint = std::make_unique<EngineRpcEndpoint>(std::cin, std::cout);
        windowed_rpc_host = std::make_unique<WindowedRpcHost>(
            std::cin, std::cout,
            [&windowed_rpc_endpoint](std::string_view line) {
                return windowed_rpc_endpoint->processLine(line);
            },
            defaultWindowedRpcQueueCapacity);
    }
#endif

    auto interactive = resolveInteractiveLoopModules();
    auto &window = interactive.window;
    auto &framerate_adjuster = interactive.framerate_adjuster;
#if PELICAN_WITH_OPENXR
    auto *xr_session = launch_config.xr_active
                           ? FastModuleContainer::tryGet<OpenXr::SessionRuntime>()
                           : nullptr;
    auto *xr_target = xr_composition_target.get();
    auto *xr_mirror = xr_mirror_sink.get();
#endif

    const auto update_interactive_state = [&](bool xr_frame) {
        updateFrameState();
#if PELICAN_WITH_RPC
        if (windowed_rpc_host) windowed_rpc_host->processFrameBoundary();
#endif
        if (UserInput::isKeyPushed(KeyCode::F5)) {
            (void)modules.reload_service.requestRuntimeReload(
                watch::gameLogicReloadParticipantName);
        }
        logInputSnapshotIfRequested(input_state.currentSnapshot());
        requestF11CaptureIfNeeded(modules.renderdoc_capture, xr_frame);
    };

    std::uint32_t interactive_frame_count = 0;
    const auto windowFrameLimitReached = [&] {
        if (!launch_config.headless_frames_explicit ||
            launch_config.headless_frames == 0) {
            return false;
        }
        ++interactive_frame_count;
        return interactive_frame_count >=
               launch_config.headless_frames;
    };

    while (true) {
        if (!window.process())
            break;
        auto window_events = window.drainInputEvents();
        if (!input_sequence.isReplaying()) {
            input_state.queueEvents(window_events);
        }
#if PELICAN_WITH_OPENXR
        if (xr_session != nullptr) {
            xr_session->pollEvents();
            if (xr_session->hasTerminalPath()) break;
            if (xr_session->isSessionRunning()) {
                if (xr_target == nullptr || xr_target->generationTeardownRequired()) {
                    LOG_ERROR(logger, "OpenXR composition generation requires teardown");
                    break;
                }
                const auto display_timing = xr_session->waitFrame();
                engine_time.advance();
                modules.vulkan.setCurrentFrameIndex(engine_time.frameIndex());
                const auto begin_result = xr_session->beginFrame();
                OpenXr::XrLocatedViews located_views;
                const bool xr_frame_renderable =
                    begin_result == OpenXr::XrBeginFrameResult::ready &&
                    display_timing.shouldRender();
                if (xr_frame_renderable) {
                    located_views = xr_session->locateViews(display_timing);
                }

                // WP131's adapter anchors both eye poses to the active camera
                // captured at this logical frame boundary.
                const auto active_camera_view = GET_MODULE(Camera).getViewMatrix();
                const auto camera_projection = GET_MODULE(Camera).getProjectionSpec();
                const auto update_start = Clock::now();
                auto xr_input = xr_session->syncActions(Actions::actionSetStack());
                internal::setInputActionBackendFrame(std::move(xr_input.actions));
                input_state.queuePoseSamples(std::move(xr_input.pose_samples));
                update_interactive_state(true);
                const auto update_end = Clock::now();

                renderer.selectGraphVariant(RenderGraphVariant::xr);
                const auto render_start = Clock::now();
                if (xr_frame_renderable) {
                    xr_target->prepareFrame(
                        display_timing, located_views,
                        camera_projection.znear,
                        camera_projection.zfar);
                    const auto view_family = OpenXr::buildMainRenderViewFamily(
                        active_camera_view, located_views.views,
                        camera_projection.znear, camera_projection.zfar);
                    renderer.renderLogicalFrame(*xr_target, view_family);
                    if (xr_mirror != nullptr) {
                        xr_mirror->tryPresent();
                        const auto &mirror = xr_mirror->statistics();
                        xr_session->recordMirrorStatistics(
                            mirror.presented, mirror.dropped, mirror.failures);
                    }
                } else {
                    xr_target->endFrameWithoutLayers(display_timing);
                }
                const auto render_end = Clock::now();
                if (render_timing != nullptr) {
                    render_timing->recordCpuFrame(CpuFrameDurations{
                        elapsedMs(update_start, update_end),
                        elapsedMs(render_start, render_end),
                        0.0,
                    });
                }
                if (windowFrameLimitReached()) {
                    break;
                }
                continue;
            }
            auto xr_input = xr_session->syncActions(Actions::actionSetStack());
            internal::setInputActionBackendFrame(std::move(xr_input.actions));
            input_state.queuePoseSamples(std::move(xr_input.pose_samples));
        }
#endif
        const auto update_start = Clock::now();
        engine_time.advance();
        modules.vulkan.setCurrentFrameIndex(engine_time.frameIndex());
        update_interactive_state(false);
        const auto update_end = Clock::now();

        const auto render_start = Clock::now();
        renderFlatFrameWithOptionalCapture(modules, window);
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
        if (windowFrameLimitReached()) {
            break;
        }
    }

    finishLoopResources(input_sequence, modules.camera_bake, render_timing,
                        modules.renderdoc_capture, true);
}

} // namespace Pelican
