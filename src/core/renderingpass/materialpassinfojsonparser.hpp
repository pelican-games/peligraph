#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

std::optional<MaterialDrawTagFilter>
parseMaterialDrawTagFilterFromJson(
    const nlohmann::json &pass_json,
    std::string_view context);

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json);
void parseMaterialPassScreenInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata);
void parseMaterialPassSurfaceResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata);
void parseMaterialPassResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const std::unordered_set<std::string> &buffer_names);

} // namespace Pelican
