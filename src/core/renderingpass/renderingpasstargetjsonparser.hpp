#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican {

class RenderTargetContainer;

std::vector<GlobalRenderTargetId> parseColorOutputTargetsFromJson(RenderTargetContainer &rt_container,
                                                                  nlohmann::json color_output);
GlobalRenderTargetId parseDepthOutputTargetFromJson(RenderTargetContainer &rt_container,
                                                    const nlohmann::json &depth_output);
std::vector<GlobalRenderTargetId> parseInputTargetsFromJson(RenderTargetContainer &rt_container,
                                                            nlohmann::json input_output);

} // namespace Pelican
