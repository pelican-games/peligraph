#pragma once

#include "./container.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

enum class XrMode { off, auto_mode, on };

struct EngineLaunchCameraOverride {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};
    float fov_y = 45.0f;
};

DECLARE_MODULE(EngineLaunchConfig) {
  public:
    bool headless = false;
    bool rpc = false;
    // Deterministic drivers never run developer-tool callbacks. Replay and
    // golden harnesses set these when they are not already using headless.
    bool input_replay = false;
    bool golden_mode = false;
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
    double fps = 60.0;
    bool shader_hot_reload = true;
    bool allow_absolute_paths = false;
    bool strict_assets = false;
    bool dump_frame_plan = false;
    bool gpu_labels = false;
    bool force_unorm_color_path_for_testing = false;
    std::optional<std::filesystem::path> input_record;
    std::optional<std::filesystem::path> input_replay_path;
    std::optional<std::string> input_profile;
    std::optional<std::filesystem::path> camera_bake_output;
    std::optional<std::filesystem::path> play_seq;
    std::optional<std::filesystem::path> play_vat;
    std::filesystem::path seq_mesh{"builtin:sphere"};
    bool seq_loop = false;
    std::optional<EngineLaunchCameraOverride> camera_override;
    std::optional<std::filesystem::path> game_logic_dll;
};

} // namespace Pelican
