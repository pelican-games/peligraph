#pragma once

#include "./container.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct EngineLaunchCameraOverride {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};
    float fov_y = 45.0f;
};

DECLARE_MODULE(EngineLaunchConfig) {
  public:
    bool headless = false;
    bool rpc = false;
    vk::Extent2D headless_extent{1280, 720};
    uint32_t headless_frames = 3;
    std::optional<std::filesystem::path> render_out;
    double fps = 60.0;
    bool shader_hot_reload = true;
    bool allow_absolute_paths = false;
    bool dump_frame_plan = false;
    std::optional<std::filesystem::path> play_seq;
    std::optional<std::filesystem::path> play_vat;
    std::filesystem::path seq_mesh{"builtin:sphere"};
    bool seq_loop = false;
    std::optional<EngineLaunchCameraOverride> camera_override;
};

} // namespace Pelican
