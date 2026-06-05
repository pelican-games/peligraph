#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

std::vector<PassDefinition> parsePassSequenceFromJson(const nlohmann::json &pass_set_json,
                                                      const std::string &rendering_pass_name,
                                                      const RenderTargetNameResolver &rt_resolver,
                                                      const RenderTargetMetadataResolver &rt_metadata);

} // namespace Pelican
