#include "debugtextpassinfojsonparser.hpp"

#include <stdexcept>

namespace Pelican {

DebugTextPassInfo parseDebugTextPassInfoFromJson(const nlohmann::json &pass_json,
                                                 const std::string &pass_name) {
    if (!pass_json.contains("shader")) {
        throw std::runtime_error("DebugText pass requires shader: " + pass_name);
    }

    const auto &shader = pass_json.at("shader");
    if (!shader.is_object()) {
        throw std::runtime_error("DebugText pass shader must be an object: " + pass_name);
    }
    if (!shader.contains("vertex") || !shader.contains("fragment")) {
        throw std::runtime_error("DebugText pass shader requires vertex and fragment: " + pass_name);
    }
    if (!shader.at("vertex").is_string() || !shader.at("fragment").is_string()) {
        throw std::runtime_error("DebugText pass shader paths must be strings: " + pass_name);
    }

    return DebugTextPassInfo{
        makeShaderReference(shader.at("vertex").get<std::string>(), ShaderStage::vertex),
        makeShaderReference(shader.at("fragment").get<std::string>(), ShaderStage::fragment),
    };
}

} // namespace Pelican
