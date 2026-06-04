#pragma once

#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

RenderingPassDefinition parseRenderingPassDefinitionFromJson(const nlohmann::json &pass_set_json,
                                                             RenderTargetContainer &rt_container);

} // namespace Pelican
