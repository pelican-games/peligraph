#pragma once

#include "../container.hpp"
#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderingPassContainer;
class RenderTargetContainer;

DECLARE_MODULE(RenderingPassConfigRegistry) {
  public:
    void registerFromJson(const std::string &json_path, vk::Extent2D base_extent,
                          RenderTargetContainer &rt_container,
                          RenderingPassContainer &pass_container) const;
};

} // namespace Pelican
