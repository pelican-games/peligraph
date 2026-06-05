#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

class RenderTargetNameResolver;

void parsePassOutputTargetsFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                    const nlohmann::json &pass_json);
void parsePassInputTargetsFromJson(PassDefinition &pass_def, const RenderTargetNameResolver &rt_resolver,
                                   const nlohmann::json &pass_json);
std::vector<GlobalRenderTargetId> parseColorOutputTargetsFromJson(const RenderTargetNameResolver &rt_resolver,
                                                                  nlohmann::json color_output);
GlobalRenderTargetId parseDepthOutputTargetFromJson(const RenderTargetNameResolver &rt_resolver,
                                                    const nlohmann::json &depth_output);
std::vector<GlobalRenderTargetId> parseInputTargetsFromJson(const RenderTargetNameResolver &rt_resolver,
                                                            nlohmann::json input_output);

} // namespace Pelican
