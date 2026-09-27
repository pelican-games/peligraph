#pragma once

#include "./audio/audiobackendkind.hpp"
#include "./container.hpp"

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

DECLARE_MODULE(EngineLaunchConfig) {
  public:
    bool headless = false;
    bool rpc = false;
    // EW-1 (--embedded): another process's window will host this one, so the
    // window is created hidden, unfocused and never fullscreen. Nothing else
    // changes: time and game behaviour are those of a normal windowed run.
    bool embedded = false;
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
    // Offline output. The fixed-step frame the run's first capture belongs to,
    // counted from 1; 0 means the run starts at the beginning as it always did.
    // The command line names a time in seconds and the parser quantizes it to
    // the frame grid once, here, so the rest of the engine never has to ask
    // whether a time falls between two frames. The frames before this one are
    // replayed, not skipped: the state that decides what a frame looks like --
    // graph clocks, game systems, animation components -- is not a function of
    // the clock alone.
    std::uint64_t start_frames = 0;
    double fps = 60.0;
    bool shader_hot_reload = true;
    bool allow_absolute_paths = false;
    bool strict_assets = false;
    bool dump_frame_plan = false;
    bool gpu_labels = false;
    VulkanValidationMode vulkan_validation =
        VulkanValidationMode::build_default;
    // Tool-declared startup inputs. These are deliberately absent from
    // ProjectBasicConfig, so project loading cannot opt into or persist them.
    std::vector<std::string> render_feature_overlays;
    std::vector<std::string> input_action_overlays;
    bool force_unorm_color_path_for_testing = false;
    std::optional<std::filesystem::path> input_record;
    std::optional<std::filesystem::path> input_replay_path;
    std::optional<std::string> input_profile;
    std::optional<EngineLaunchFreeCamera> free_camera;
    std::optional<std::filesystem::path> camera_bake_output;
    std::optional<std::filesystem::path> play_seq;
    std::optional<std::filesystem::path> play_vat;
    std::filesystem::path seq_mesh{"builtin:sphere"};
    bool seq_loop = false;
    std::optional<EngineLaunchCameraOverride> camera_override;
    std::optional<std::filesystem::path> game_logic_dll;
};

} // namespace Pelican
