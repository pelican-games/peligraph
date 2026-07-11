#pragma once

#include "rendertargetdefinition.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

nlohmann::json resolveRenderTargetFormatClassesV2(const nlohmann::json &data,
                                                   vk::Format frame_target_format,
                                                   vk::Extent2D frame_target_extent,
                                                   bool hdr_enabled);
std::vector<RenderTargetDefinition> parseRenderTargetDefinitionsFromJson(const nlohmann::json &data);

} // namespace Pelican
