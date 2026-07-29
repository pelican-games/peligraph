#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace Pelican {

class RenderTargetNameResolver;

void parsePassOutputTargetsFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                    const nlohmann::json &pass_json);
void parsePassInputTargetsFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                   const nlohmann::json &pass_json,
                                   const std::unordered_set<std::string> &buffer_names = {});
std::vector<RasterAttachmentView> parseColorOutputTargetsFromJson(
    const RenderTargetNameResolver &rt_resolver,
    nlohmann::json color_output);
RasterAttachmentView parseDepthOutputTargetFromJson(
    const RenderTargetNameResolver &rt_resolver,
    const nlohmann::json &depth_output);
std::vector<GlobalRenderTargetId> parseInputTargetsFromJson(const RenderTargetNameResolver &rt_resolver,
                                                            nlohmann::json input_output);
void parseInputResourcesFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                 nlohmann::json input_output,
                                 const std::unordered_set<std::string> &buffer_names);

} // namespace Pelican
