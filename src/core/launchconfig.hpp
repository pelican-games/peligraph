#pragma once

#include "./audio/audiobackendkind.hpp"
#include "./container.hpp"
#include "./osc/osclistenerconfig.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

enum class XrMode { off, auto_mode, on };

enum class VulkanValidationMode {
    build_default,
    disabled,
    enabled,
};

struct EngineLaunchCameraOverride {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};
    float fov_y = 45.0f;
};

enum class EngineLaunchFreeCameraPreset {
    Blender,
    Unity,
};

struct EngineLaunchFreeCamera {
    EngineLaunchFreeCameraPreset preset = EngineLaunchFreeCameraPreset::Blender;
    float orbit_distance = 5.0f;
    float sensitivity = 1.5f;
};

// PM-1b (--start-state). The play state an engine starts in. `edit` is the
// editor's: behaviors dormant, game clock stopped, an editor camera prepared
// (docs/design_play_mode.md sections 2 and 4.4).
enum class EngineStartState {
    playing,
    edit,
};

// A monitor index is present when the option was given; size is absent for
// the borderless full-monitor default. The pattern switch selects the
// diagnostic instead of the rendered picture.
struct OutputWindowLaunchOptions {
    bool enabled = false;
    std::uint32_t monitor = 0;
    std::optional<vk::Extent2D> size;
    bool pattern = false;
};

DECLARE_MODULE(EngineLaunchConfig) {
  public:
    bool headless = false;
    // PM-1b: --start-state. Only `edit` changes anything; an omitted flag and
    // `playing` are the same launch.
    EngineStartState start_state = EngineStartState::playing;
    bool rpc = false;
    // EW-1 (--embedded): another process's window will host this one, so the
    // window is created hidden, unfocused and never fullscreen. Nothing else
    // changes: time and game behaviour are those of a normal windowed run.
    bool embedded = false;
    // BS-1 (--render-fallback, needs --rpc): a rendering configuration that
    // cannot be composed at start does not stop the engine. It starts on the
    // engine's built-in fallback generation instead, says why in
    // get_status.render_pipeline, and takes the fixed configuration through
    // reload_render_documents or a render edit. Without it the refusal stays
    // the startup fatal error it always was.
    bool render_fallback = false;
    // Deterministic drivers never run developer-tool callbacks. Replay and
    // golden harnesses set these when they are not already using headless.
    bool input_replay = false;
    bool golden_mode = false;
    // AU-3: the audio backend the launch named (--audio-backend, else the
    // PELICAN_AUDIO_BACKEND environment variable). Absent, the window decides:
    // device with one, offline headless. A named device that cannot open still
    // falls back to null.
    std::optional<AudioBackendKind> audio_backend;
    XrMode xr_mode = XrMode::off;
    // Keep the command-line intent after activation normalizes xr_mode.  XR1a
    // needs this once, before GPU resources exist, to distinguish auto
    // fallback from an explicit-on hard error during Vulkan bootstrap.
    XrMode xr_requested_mode = XrMode::off;
    bool xr_active = false;
    vk::Extent2D headless_extent{1280, 720};
    uint32_t headless_frames = 3;
    bool headless_frames_explicit = false;
    std::optional<std::filesystem::path> render_out;
    // VO-1a: the existing directory the document's output.file sinks write
    // raw frames into. Absent means no sink is active, although the lowered
    // document still retains their targets for the report's cost.
    std::optional<std::filesystem::path> output_dir;
    // VO-4: --output-spout, the only thing that arms output.spout sinks. Apart
    // from output_dir. Studio's embedded engine doesn't pass it.
    bool output_spout = false;
    // VO-5b: --output-spout-transport cpu, a machine's setting and not a
    // project's: the way out on stage, and the control for measuring. It only
    // takes away Spout's optional Vulkan requests and the GPU route.
    bool output_spout_cpu = false;
    // External H.264 encoder. Absent searches PATH at the first video frame.
    std::optional<std::filesystem::path> ffmpeg;
    // Per sink; a sink that reaches it becomes finished.
    std::uint32_t output_max_frames = 1000;
    // Across every sink of the run; a write that would pass it leaves the sink
    // finished and writes no file.
    std::uint64_t output_max_bytes = 4ull * 1024ull * 1024ull * 1024ull;
    // Offline output. The fixed-step frame the run's first capture belongs to,
    // counted from 1; 0 means the run starts at the beginning as it always did.
    // The command line names a time in seconds and the parser quantizes it to
    // the frame grid once, here, so the rest of the engine never has to ask
    // whether a time falls between two frames. The frames before this one are
    // replayed, not skipped: the state that decides what a frame looks like --
    // graph clocks, game systems, animation components -- is not a function of
    // the clock alone.
    std::uint64_t start_frames = 0;
    // --start-time was given (even time 0): this run's first output frame was
    // named by the command line, so it is an offline deterministic launch.
    bool start_time_explicit = false;
    double fps = 60.0;
    bool shader_hot_reload = true;
    bool allow_absolute_paths = false;
    bool strict_assets = false;
    bool dump_frame_plan = false;
    bool gpu_labels = false;
    std::string gpu;
    VulkanValidationMode vulkan_validation =
        VulkanValidationMode::build_default;
    // Tool-declared startup inputs. These are deliberately absent from
    // ProjectBasicConfig, so project loading cannot opt into or persist them.
    std::vector<std::string> render_feature_overlays;
    std::vector<std::string> input_action_overlays;
    bool force_unorm_color_path_for_testing = false;
    std::optional<std::filesystem::path> input_record;
    std::optional<std::filesystem::path> input_replay_path;
    std::optional<std::filesystem::path> live_record;
    std::optional<std::filesystem::path> live_replay_path;
    std::optional<std::string> input_profile;
    // OSC-2b: the process-wide UDP listener. Absent means no socket is opened.
    // Configured only by player options, never by the project file.
    std::optional<OscListenerConfig> osc_listener;
    // Configured only by player options, never by the project file.
    bool joystick_monitor = false;
    // OSC-5b: the process-wide UDP sender. Absent means no socket is opened.
    // Configured only by player options, never by the project file.
    std::optional<OscSendConfig> osc_send;
    std::optional<EngineLaunchFreeCamera> free_camera;
    std::optional<std::filesystem::path> camera_bake_output;
    std::optional<std::filesystem::path> play_seq;
    std::optional<std::filesystem::path> play_vat;
    std::filesystem::path seq_mesh{"builtin:sphere"};
    bool seq_loop = false;
    std::optional<EngineLaunchCameraOverride> camera_override;
    std::optional<std::filesystem::path> game_logic_dll;
    OutputWindowLaunchOptions output_window;
};

} // namespace Pelican
