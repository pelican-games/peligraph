#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

PassDefinition parsePassDefinitionFromJson(const nlohmann::json &pass_json,
                                           const RenderTargetNameResolver &rt_resolver,
                                           const RenderTargetMetadataResolver &rt_metadata,
                                           const std::unordered_set<std::string> &buffer_names = {});

} // namespace Pelican
