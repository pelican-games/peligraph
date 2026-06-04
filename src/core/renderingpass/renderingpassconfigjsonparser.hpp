#pragma once

#include "renderingpass.hpp"
#include "rendertargetcontainer.hpp"
#include "../shader/shadercontainer.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

std::vector<RenderingPassDefinition>
parseRenderingPassDefinitionsFromConfigJson(const nlohmann::json &rendering_pass_data,
                                            RenderTargetContainer &rt_container,
                                            ShaderContainer &shader_container);

} // namespace Pelican
