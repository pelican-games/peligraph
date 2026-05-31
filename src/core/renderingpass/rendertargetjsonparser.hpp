#pragma once

#include "rendertargetcontainer.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

void registerRenderTargetsFromJson(const nlohmann::json &data, vk::Extent2D base_extent,
                                   RenderTargetContainer &rt_container);

} // namespace Pelican
