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
#include "../playback/launchcameraoverride.hpp"
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
#include <cstdint>
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
    // `--camera` used to be applied by SeqPlayer's constructor (always) and
    // VatPlayer's (when --play-vat was given), so with both players active it
    // ran twice. It runs once here instead, at the earlier of those two
    // points: VatPlayer is constructed on the next line, SeqPlayer later in
    // prepareRuntimeModuleGraph, and nothing between the two reads Camera.
    applyLaunchCameraOverride();
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

// Whether an exit path flushes the GPU timing ring on the way out. `flush()`
// blocks on `collectGpuResults(true)` and then emits the frame-metrics log
// line; the headless RPC exit is the one caller that skips it. This used to be
// an unnamed trailing `bool`: the behaviour is preserved exactly and only the
// name is new. The reason for the skip is not documented anywhere in the tree.
enum class TimingFlush {
    skip,
    flush,
};

void finishLoopResources(InputSequenceRuntime &input_sequence,
                         CameraBakeRecorder *camera_bake,
                         RenderTiming *render_timing,
                         RenderDocCapture &renderdoc_capture,
                         TimingFlush timing_flush) {
    renderdoc_capture.beginShutdown();
    auto gpu = resolveGpuDrainModules();
    gpu.vulkan.waitIdle();
    if (timing_flush == TimingFlush::flush && render_timing != nullptr)
        render_timing->flush();
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
            modules.engine_time.evaluationIndex(),
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

// ---------------------------------------------------------------------------
// One frame, spelled once.
//
// The engine runs frames in three places -- offline (`--headless`), in a
// window, and in a window driving an OpenXR session -- and all three run the
// same four steps in the same order: advance the timeline, update the CPU-side
// world, render, record the CPU frame timings. `FrameStep` is that order
// written down once. The three drivers below (`runHeadlessFrames`,
// `runInteractive`, `runXrFrame`) differ only in what they hand to these four
// calls, so adding or reordering a phase is one edit rather than three.
//
// The three places where the paths genuinely differ are the three enums below.
// Each is passed explicitly at every call site -- including the sites that want
// the do-nothing option -- so a difference between the paths is something a
// reader sees named rather than something they have to notice is missing.
// ---------------------------------------------------------------------------

// Where the update clock opens relative to the timeline advance.
enum class AdvanceTiming {
    // Offline and flat frames: the clock opens before the advance, so
    // EngineTime::advance() is counted as update time.
    measured,
    // XR frames: the advance sits between xrWaitFrame and xrBeginFrame, and
    // the protocol calls that follow it (begin frame, locate views) are not
    // the engine's update work, so the clock opens after them. Preserved from
    // the pre-refactor code; reason not documented.
    unmeasured,
};

// Who picks the render graph variant for this frame.
enum class GraphVariantSelection {
    // Offline and flat frames: Renderer::render() forwards to
    // Renderer::render(view_families), whose first statement is
    // selectGraphVariant(RenderGraphVariant::flat) (renderer.cpp). Selecting
    // it here would be a second, redundant call -- and selectGraphVariant
    // resets temporal history on a real transition, so the redundancy would
    // not be free. Intentional.
    renderer_selects,
    // XR frames call Renderer::renderLogicalFrame() directly, which does not
    // select a variant, so the driver has to.
    xr,
};

// Whether this frame is paced by FramerateAdjust.
enum class FramerateWait {
    // Offline frames run a fixed-step timeline as fast as the machine allows,
    // and XR frames are paced by xrWaitFrame -- named as the frame-progression
    // owner of the windowed+XR path in
    // docs/source-code-guide/02_runtime_lifecycle.md 2.4, which also states
    // that FramerateAdjust is windowed-only. Sleeping again would push the
    // frame past the runtime's predicted display time. Both intentional.
    none,
    // The flat windowed frame is the only one with nothing else pacing it.
    adjuster,
};

class FrameStep {
  public:
    FrameStep(Renderer &renderer, EngineTime &engine_time,
              VulkanManageCore &vulkan, RenderTiming *render_timing)
        : renderer_{renderer}, engine_time_{engine_time}, vulkan_{vulkan},
          render_timing_{render_timing} {}

    // Step 1: move the timeline on by one frame and republish the in-flight
    // frame index that the per-frame GPU resources are indexed by.
    void advance(AdvanceTiming timing) {
        advance_is_measured_ = timing == AdvanceTiming::measured;
        if (advance_is_measured_) update_start_ = Clock::now();
        engine_time_.advance();
        vulkan_.setCurrentFrameIndex(engine_time_.evaluationIndex());
    }

    // Step 2: everything between the advance and the first render call.
    template <class UpdateFn> void update(UpdateFn &&update_world) {
        if (!advance_is_measured_) update_start_ = Clock::now();
        update_world();
        update_end_ = Clock::now();
    }

    // Step 3: graph variant selection, then the path's own render call.
    template <class RenderFn>
    void render(GraphVariantSelection variant, RenderFn &&render_frame) {
        if (variant == GraphVariantSelection::xr)
            renderer_.selectGraphVariant(RenderGraphVariant::xr);
        render_start_ = Clock::now();
        render_frame();
        render_end_ = Clock::now();
    }

    // Step 4: the optional pacing wait, then the one recordCpuFrame for this
    // frame. The wait lives here because its duration is the third field of
    // that record, so the two cannot be separated without re-timing it.
    void record(FramerateWait wait, FramerateAdjust *framerate_adjuster) {
        double wait_ms = 0.0;
        if (wait == FramerateWait::adjuster) {
            const auto wait_start = Clock::now();
            framerate_adjuster->wait();
            const auto wait_end = Clock::now();
            wait_ms = elapsedMs(wait_start, wait_end);
        }
        if (render_timing_ == nullptr) return;
        render_timing_->recordCpuFrame(CpuFrameDurations{
            elapsedMs(update_start_, update_end_),
            elapsedMs(render_start_, render_end_),
            wait_ms,
        });
    }

  private:
    Renderer &renderer_;
    EngineTime &engine_time_;
    VulkanManageCore &vulkan_;
    RenderTiming *render_timing_;
    bool advance_is_measured_ = true;
    Clock::time_point update_start_{};
    Clock::time_point update_end_{};
    Clock::time_point render_start_{};
    Clock::time_point render_end_{};
};

// The windowed JSON-RPC layer: one object built before the loop, one call per
// frame boundary. Both halves live behind this single feature region so that
// the frame drivers below carry no preprocessor conditional of their own.
#if PELICAN_WITH_RPC
class WindowedRpcPump {
  public:
    void start(std::istream &input, std::ostream &output) {
        endpoint_ = std::make_unique<EngineRpcEndpoint>(input, output);
        host_ = std::make_unique<WindowedRpcHost>(
            input, output,
            [this](std::string_view line) { return endpoint_->processLine(line); },
            defaultWindowedRpcQueueCapacity);
    }

    void processFrameBoundary() {
        if (host_) host_->processFrameBoundary();
    }

  private:
    std::unique_ptr<EngineRpcEndpoint> endpoint_;
    std::unique_ptr<WindowedRpcHost> host_;
};
#else
// --rpc is refused while the launch config is parsed when RPC is compiled out
// (src/player/main.cpp), so neither method is reached in this build.
class WindowedRpcPump {
  public:
    void start(std::istream &, std::ostream &) {}
    void processFrameBoundary() {}
};
#endif

// What one turn of the XR half of the interactive loop decided.
enum class XrFrameOutcome {
    // No XR session on this run (compiled out, or --xr not active): the flat
    // frame runs.
    not_xr,
    // XR is up but the session is not running yet: actions were synced and the
    // flat frame runs.
    session_idle,
    // A full XR frame ran; the interactive loop continues to its next turn.
    frame_done,
    // Terminal session path, or the composition generation needs teardown:
    // leave the interactive loop.
    stop,
};

// Everything the XR path owns. Built before the module graph is frozen because
// its constructors pull modules.
#if PELICAN_WITH_OPENXR
struct XrRuntime {
    std::unique_ptr<OpenXr::XrCompositionTarget> composition_target;
    std::unique_ptr<OpenXr::XrMirrorSink> mirror_sink;
    OpenXr::SessionRuntime *session = nullptr;
};

XrRuntime createXrRuntime(const EngineLaunchConfig &launch_config) {
    XrRuntime runtime;
    if (!launch_config.xr_active) return runtime;
    auto &session = GET_MODULE(OpenXr::SessionRuntime);
    runtime.composition_target = std::make_unique<OpenXr::XrCompositionTarget>(
        resolveXrCompositionDependencies(session));
    runtime.mirror_sink = std::make_unique<OpenXr::XrMirrorSink>();
    runtime.session = &session;
    return runtime;
}

// One XR frame: the same four FrameStep calls a flat frame makes, with the
// OpenXR frame protocol (wait / begin / locate views / composition layer)
// wrapped around them.
template <class UpdateFn, class LimitFn>
XrFrameOutcome runXrFrame(XrRuntime &xr, LoopModules &modules,
                          const UpdateFn &update_interactive_state,
                          const LimitFn &frame_limit_reached) {
    auto *xr_session = xr.session;
    if (xr_session == nullptr) return XrFrameOutcome::not_xr;

    xr_session->pollEvents();
    if (xr_session->hasTerminalPath()) return XrFrameOutcome::stop;
    if (!xr_session->isSessionRunning()) {
        auto xr_input = xr_session->syncActions(Actions::actionSetStack());
        internal::setInputActionBackendFrame(std::move(xr_input.actions));
        modules.input_state.queuePoseSamples(std::move(xr_input.pose_samples));
        return XrFrameOutcome::session_idle;
    }

    auto *xr_target = xr.composition_target.get();
    auto *xr_mirror = xr.mirror_sink.get();
    if (xr_target == nullptr || xr_target->generationTeardownRequired()) {
        LOG_ERROR(logger, "OpenXR composition generation requires teardown");
        return XrFrameOutcome::stop;
    }

    const auto display_timing = xr_session->waitFrame();
    FrameStep step{modules.renderer, modules.engine_time, modules.vulkan,
                   modules.render_timing};
    step.advance(AdvanceTiming::unmeasured);

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

    step.update([&] {
        auto xr_input = xr_session->syncActions(Actions::actionSetStack());
        internal::setInputActionBackendFrame(std::move(xr_input.actions));
        modules.input_state.queuePoseSamples(std::move(xr_input.pose_samples));
        update_interactive_state(true);
    });
    step.render(GraphVariantSelection::xr, [&] {
        if (xr_frame_renderable) {
            xr_target->prepareFrame(
                display_timing, located_views,
                camera_projection.znear,
                camera_projection.zfar);
            const auto view_family = OpenXr::buildMainRenderViewFamily(
                active_camera_view, located_views.views,
                camera_projection.znear, camera_projection.zfar);
            modules.renderer.renderLogicalFrame(*xr_target, view_family);
            if (xr_mirror != nullptr) {
                xr_mirror->tryPresent();
                const auto &mirror = xr_mirror->statistics();
                xr_session->recordMirrorStatistics(
                    mirror.presented, mirror.dropped, mirror.failures);
            }
        } else {
            xr_target->endFrameWithoutLayers(display_timing);
        }
    });
    step.record(FramerateWait::none, nullptr);

    if (frame_limit_reached()) return XrFrameOutcome::stop;
    return XrFrameOutcome::frame_done;
}
#else
struct XrRuntime {};

inline XrRuntime createXrRuntime(const EngineLaunchConfig &) { return {}; }

// XR is compiled out: the interactive loop always runs its flat frame.
template <class UpdateFn, class LimitFn>
XrFrameOutcome runXrFrame(XrRuntime &, LoopModules &, const UpdateFn &,
                          const LimitFn &) {
    return XrFrameOutcome::not_xr;
}
#endif

// Offline frames: --headless without --rpc.
void runHeadlessFrames(LoopModules &modules, double timeline_fps) {
    const auto &launch_config = modules.launch_config;
    auto &input_sequence = modules.input_sequence;

    const auto render_out_pattern =
        launch_config.render_out ? parseRenderOutPattern(*launch_config.render_out) : RenderOutPattern{};
    // One headless frame, spelled once -- and, now that the frame is one
    // FrameStep, spelled the same way the windowed and XR frames are. Offline
    // output replays the same frame the frame-count loop runs -- advance,
    // update, render -- because a warm-up that skipped any of the three would
    // leave the clocks, the game systems and the temporal history somewhere
    // the real run never is.
    const auto headlessFrame = [&](bool capture, uint32_t frame_number) {
        if (!input_sequence.isReplaying()) {
            modules.input_state.clear();
        }
        FrameStep step{modules.renderer, modules.engine_time, modules.vulkan,
                       modules.render_timing};
        step.advance(AdvanceTiming::measured);
        step.update([] { updateFrameState(); });
        step.render(GraphVariantSelection::renderer_selects,
                    [&] { modules.renderer.render(); });
        step.record(FramerateWait::none, nullptr);
        if (capture) {
            resolveOutputRenderTarget()
                .captureLastFrameToPng(formatRenderOutPath(*launch_config.render_out, render_out_pattern,
                                                           frame_number));
        }
    };
    // `--start-time`: the run's first captured frame is the fixed-step frame
    // that lands on that time, so the frames before it are replayed rather
    // than skipped. There is no jump: the state that decides what a frame
    // looks like is not reachable from the clock alone -- graph clocks only
    // advance inside render(), project game systems integrate their own dt,
    // and an animation component's evaluator is not consulted at all -- so
    // the only honest way to be at a time is to have been at every step
    // before it. The replayed frames run the same three calls the captured
    // ones do; only the capture is left out.
    if (launch_config.start_frames > 0) {
        const auto replay = launch_config.start_frames - 1;
        std::cerr << "offline: start_time="
                  << static_cast<double>(launch_config.start_frames) / timeline_fps
                  << " start_frame=" << launch_config.start_frames
                  << " replay=" << replay << " frames=" << launch_config.headless_frames << std::endl;
        for (std::uint64_t step = 0; step < replay; ++step) headlessFrame(false, 0u);
    }
    const auto replay_frames = input_sequence.isReplaying() ? input_sequence.replayFrameCount() : 0;
    const auto frame_limit = input_sequence.isReplaying() && !launch_config.headless_frames_explicit
                                 ? static_cast<uint32_t>(replay_frames)
                                 : launch_config.headless_frames;
    for (uint32_t frame = 0; frame_limit == 0 || frame < frame_limit;
         ++frame) {
        headlessFrame(launch_config.render_out && render_out_pattern.has_frame_token, frame + 1);
    }
    if (launch_config.render_out && !render_out_pattern.has_frame_token && launch_config.headless_frames > 0) {
        resolveOutputRenderTarget().captureLastFrameToPng(*launch_config.render_out);
    }
}

// Windowed frames, with the XR half layered onto the same loop while a session
// is running. --rpc layers onto this path too, as one call inside the update.
void runInteractive(LoopModules &modules, XrRuntime &xr,
                    WindowedRpcPump &rpc_pump) {
    const auto &launch_config = modules.launch_config;
    auto &input_sequence = modules.input_sequence;
    auto &input_state = modules.input_state;

    if (launch_config.rpc) rpc_pump.start(std::cin, std::cout);

    auto interactive = resolveInteractiveLoopModules();
    auto &window = interactive.window;
    auto &framerate_adjuster = interactive.framerate_adjuster;

    // The single point that means "this frame's state update is finished".
    // Both the flat and the XR frame call it, which is why it is a lambda
    // rather than inline in either.
    const auto update_interactive_state = [&](bool xr_frame) {
        updateFrameState();
        rpc_pump.processFrameBoundary();
        if (UserInput::isKeyPushed(KeyCode::F5)) {
            (void)modules.reload_service.requestRuntimeReload(
                watch::gameLogicReloadParticipantName);
        }
        logInputSnapshotIfRequested(input_state.currentSnapshot());
        requestF11CaptureIfNeeded(modules.renderdoc_capture, xr_frame);
    };

    std::uint32_t interactive_frame_count = 0;
    // Counts flat and XR frames alike, so --frames means the same number on
    // both. It increments as a side effect, so it is called exactly once per
    // frame on each path.
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
        // GP-4: the cursor mode game code asked for in the frame before.
        const auto cursor = window.applyCursorMode(
            input_state.cursorMode().requested, launch_config.rpc);
        input_state.setAppliedCursorMode(cursor.effective, cursor.embedded);
        auto window_events = window.drainInputEvents();
        if (!input_sequence.isReplaying()) {
            input_state.queueEvents(window_events);
        }

        const auto xr_outcome = runXrFrame(xr, modules, update_interactive_state,
                                           windowFrameLimitReached);
        if (xr_outcome == XrFrameOutcome::stop) break;
        if (xr_outcome == XrFrameOutcome::frame_done) continue;

        // The flat frame: reached when XR is compiled out, is not active, or
        // its session is not running.
        FrameStep step{modules.renderer, modules.engine_time, modules.vulkan,
                       modules.render_timing};
        step.advance(AdvanceTiming::measured);
        step.update([&] { update_interactive_state(false); });
        step.render(GraphVariantSelection::renderer_selects,
                    [&] { renderFlatFrameWithOptionalCapture(modules, window); });
        step.record(FramerateWait::adjuster, &framerate_adjuster);

        if (windowFrameLimitReached()) {
            break;
        }
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
    auto &input_sequence = modules.input_sequence;
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
    modules.engine_time.setup(time_mode, 1.0 / timeline_fps);
    if (launch_config.camera_bake_output) {
        modules.camera_bake->start(*launch_config.camera_bake_output, input_sequence.replayFps());
    }
    prepareRuntimeModuleGraph(modules);
    // Built here rather than inside runInteractive(): the XR objects pull
    // modules, and creation is frozen a few lines below.
    auto xr = createXrRuntime(launch_config);
    dumpFramePlanIfRequested(launch_config, modules.renderer);
    modules.startup_metrics.finishAndLog();
    FastModuleContainer::freezeCreation();
    const auto module_graph = FastModuleContainer::graphSnapshot();
    LOG_INFO(logger, "runtime module graph frozen: modules={} dependencies={}",
             module_graph.initialized_modules.size(), module_graph.dependencies.size());

    LOG_INFO(logger, "starting main loop");

    if (launch_config.headless) {
        if (launch_config.rpc) {
            // Frame progression belongs to the RPC client here: the server
            // pumps its own frames, so there is no FrameStep on this path.
            runEngineRpcServer(std::cin, std::cout);
            finishLoopResources(input_sequence, modules.camera_bake, modules.render_timing,
                                modules.renderdoc_capture, TimingFlush::skip);
            return;
        }

        runHeadlessFrames(modules, timeline_fps);
        finishLoopResources(input_sequence, modules.camera_bake, modules.render_timing,
                            modules.renderdoc_capture, TimingFlush::flush);
        return;
    }

    // Owned here, not by runInteractive(): the RPC host outlives the loop and
    // is torn down after finishLoopResources(), as it was before this was a
    // function of its own.
    WindowedRpcPump rpc_pump;
    runInteractive(modules, xr, rpc_pump);
    finishLoopResources(input_sequence, modules.camera_bake, modules.render_timing,
                        modules.renderdoc_capture, TimingFlush::flush);
}

} // namespace Pelican
