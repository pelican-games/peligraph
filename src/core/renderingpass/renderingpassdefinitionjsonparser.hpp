#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

RenderingPassDefinition parseRenderingPassDefinitionFromJson(const nlohmann::json &pass_set_json,
                                                             const RenderTargetNameResolver &rt_resolver,
                                                             const RenderTargetMetadataResolver &rt_metadata,
                                                             const std::unordered_set<std::string> &buffer_names = {});

} // namespace Pelican
