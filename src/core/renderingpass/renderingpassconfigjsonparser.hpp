#pragma once

#include "renderingpass.hpp"
#include "rendertargetcontainer.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

std::vector<RenderingPassDefinition>
parseRenderingPassDefinitionsFromConfigJson(const nlohmann::json &rendering_pass_data,
                                            RenderTargetContainer &rt_container);

} // namespace Pelican
