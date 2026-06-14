#pragma once

#include "./container.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(EngineLaunchConfig) {
  public:
    bool headless = false;
    vk::Extent2D headless_extent{1280, 720};
    uint32_t headless_frames = 3;
    std::optional<std::filesystem::path> render_out;
    double fps = 60.0;
    bool shader_hot_reload = true;
};

} // namespace Pelican
