#pragma once

#include "../../project/graphvariantpolicy.hpp"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// A third, startup-compiled graph program.  Unlike flat/xr RenderingPassId it
// is data-only: render_preview executes it against request-local resources and
// therefore never enters Renderer::renderLogicalFrame.
struct PreviewGraphProgram {
    std::string name = "preview";
    std::uint64_t generation = 0;
    std::vector<std::string> pass_names;
    std::vector<std::string> excluded_feature_names;
    CompiledGraphVariantPolicy graph_variant_policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::preview});
    nlohmann::json composed_config = nlohmann::json::object();
};

PreviewGraphProgram precompilePreviewGraph(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled);

} // namespace Pelican
