#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

PassDefinition parsePassDefinitionFromJson(const nlohmann::json &pass_json,
                                           const RenderTargetNameResolver &rt_resolver,
                                           const RenderTargetMetadataResolver &rt_metadata);

} // namespace Pelican
