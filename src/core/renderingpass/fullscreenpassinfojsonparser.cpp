#include "fullscreenpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>

namespace Pelican {

FullscreenPassInfo parseFullscreenPassInfoFromJson(const nlohmann::json &pass_json,
                                                   const std::string &pass_name) {
    FullscreenPassInfo fullscreen_info;

    if (pass_json.contains("push_constants")) {
        if (!pass_json.at("push_constants").is_string()) {
            throw std::runtime_error("Fullscreen pass push_constants must be a string: " + pass_name);
        }
        fullscreen_info.push_constants =
            stringToFullscreenPushConstantData(pass_json.at("push_constants").get<std::string>());
    } else if (pass_json.contains("needs_projection_matrix")) {
        if (!pass_json.at("needs_projection_matrix").is_boolean()) {
            throw std::runtime_error("Fullscreen pass needs_projection_matrix must be a boolean: " + pass_name);
        }
        if (pass_json.at("needs_projection_matrix").get<bool>()) {
            fullscreen_info.push_constants = FullscreenPushConstantData::eProjectionView;
        }
    }

    if (pass_json.contains("uses_light_data")) {
        if (!pass_json.at("uses_light_data").is_boolean()) {
            throw std::runtime_error("Fullscreen pass uses_light_data must be a boolean: " + pass_name);
        }
        fullscreen_info.uses_light_data = pass_json.at("uses_light_data").get<bool>();
    }

    if (!pass_json.contains("shader")) {
        throw std::runtime_error("Fullscreen pass requires shader: " + pass_name);
    }

    const auto &shader = pass_json.at("shader");
    if (!shader.is_object()) {
        throw std::runtime_error("Fullscreen pass shader must be an object: " + pass_name);
    }
    if (!shader.contains("vertex") || !shader.contains("fragment")) {
        throw std::runtime_error("Fullscreen pass shader requires vertex and fragment: " + pass_name);
    }
    if (!shader.at("vertex").is_string() || !shader.at("fragment").is_string()) {
        throw std::runtime_error("Fullscreen pass shader paths must be strings: " + pass_name);
    }

    fullscreen_info.vert_shader_path = shader.at("vertex").get<std::string>();
    fullscreen_info.frag_shader_path = shader.at("fragment").get<std::string>();
    return fullscreen_info;
}

} // namespace Pelican
