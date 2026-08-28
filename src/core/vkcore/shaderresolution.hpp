#pragma once

#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string_view>

namespace Pelican {

class PathResolver;
struct CompiledRenderingPass;

struct ShaderResolutionFamilyContract {
    std::string_view family;
    std::string_view state;
    std::string_view owner;
    std::span<const std::string_view> stages;
};

// Complete RenderPassType (14) + compute-task (1) table. It remains available
// when PELICAN_WITH_IMGUI=OFF so the conditional C++ variant cannot erase the
// public wire contract from matrix validation.
std::span<const ShaderResolutionFamilyContract>
shaderResolutionFamilyContracts();

// Renderer-owned enrichment boundary.  framePlanToJson deliberately has no
// shader knowledge; currentFramePlanJson invokes this after obtaining the
// resolved runtime pass set.
void appendShaderResolution(
    nlohmann::json &frame_plan,
    const CompiledRenderingPass &rendering_pass,
    const PathResolver &path_resolver);

} // namespace Pelican
