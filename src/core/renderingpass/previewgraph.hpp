#pragma once

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
    nlohmann::json composed_config = nlohmann::json::object();
};

bool includeFeatureInPreviewGraph(std::string_view feature_name,
                                  const nlohmann::json &feature);
void validatePreviewGraphConfig(const nlohmann::json &config);

PreviewGraphProgram precompilePreviewGraph(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled);

} // namespace Pelican
