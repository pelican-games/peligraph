#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

std::vector<PassDefinition> parsePassSequenceFromJson(const nlohmann::json &pass_set_json,
                                                      const std::string &rendering_pass_name,
                                                      const RenderTargetNameResolver &rt_resolver,
                                                      const RenderTargetMetadataResolver &rt_metadata,
                                                      const std::unordered_set<std::string> &buffer_names = {});

} // namespace Pelican
