#pragma once

#include "../container.hpp"
#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(RenderingPassConfigRegistry) {
  public:
    void registerFromJson(const std::string &json_path, vk::Extent2D base_extent) const;
};

} // namespace Pelican
