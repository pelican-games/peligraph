#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

RenderingPassDefinition parseRenderingPassDefinitionFromJson(const nlohmann::json &pass_set_json,
                                                             const RenderTargetNameResolver &rt_resolver,
                                                             const RenderTargetMetadataResolver &rt_metadata);

} // namespace Pelican
