#include "gizmopassinfojsonparser.hpp"

#include <stdexcept>

namespace Pelican {

GizmoPassInfo parseGizmoPassInfoFromJson(const nlohmann::json &pass_json,
                                         const std::string &pass_name) {
    if (!pass_json.contains("shader")) {
        throw std::runtime_error("Gizmo pass requires shader: " + pass_name);
    }
    const auto &shader = pass_json.at("shader");
    if (!shader.is_object() || !shader.contains("vertex") ||
        !shader.contains("fragment") || !shader.at("vertex").is_string() ||
        !shader.at("fragment").is_string()) {
        throw std::runtime_error(
            "Gizmo pass shader requires vertex and fragment strings: " +
            pass_name);
    }
    return GizmoPassInfo{
        makeShaderReference(shader.at("vertex").get<std::string>(),
                            ShaderStage::vertex),
        makeShaderReference(shader.at("fragment").get<std::string>(),
                            ShaderStage::fragment),
    };
}

} // namespace Pelican
