#pragma once

#include <string>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderingPassContainer;
class RenderTargetContainer;

void registerRenderingPassConfigFromJson(const std::string &json_path, vk::Extent2D base_extent,
                                         RenderTargetContainer &rt_container,
                                         RenderingPassContainer &pass_container);

} // namespace Pelican
