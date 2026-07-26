#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json);
void parseMaterialPassScreenInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata);
void parseMaterialPassSurfaceResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata);

} // namespace Pelican
