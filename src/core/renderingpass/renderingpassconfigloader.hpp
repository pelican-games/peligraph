#pragma once

#include "renderingpass.hpp"
#include "rendertargetcontainer.hpp"
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

std::vector<CompiledRenderingPass> loadCompiledRenderingPassesFromJson(const std::string &json_path,
                                                                       vk::Extent2D base_extent,
                                                                       RenderTargetContainer &rt_container);

} // namespace Pelican
