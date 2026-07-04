#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace Pelican {

class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

std::vector<RenderingPassDefinition>
parseRenderingPassDefinitionsFromConfigJson(const nlohmann::json &rendering_pass_data,
                                            const RenderTargetNameResolver &rt_resolver,
                                            const RenderTargetMetadataResolver &rt_metadata,
                                            const std::unordered_set<std::string> &buffer_names = {});

} // namespace Pelican
