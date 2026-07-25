#include "fullscreenpassinfojsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <set>
#include <stdexcept>

namespace Pelican {

namespace {

FullscreenInputFilter parseInputFilter(
    const nlohmann::json &sampling, const std::string &pass_name,
    std::size_t input_index) {
    if (!sampling.contains("filter")) {
        return FullscreenInputFilter::linear;
    }
    if (!sampling.at("filter").is_string()) {
        throw std::runtime_error(
            "Fullscreen pass input_sampling filter must be a string at input " +
            std::to_string(input_index) + ": " + pass_name);
    }
    const auto value = sampling.at("filter").get<std::string>();
    if (value == "linear") return FullscreenInputFilter::linear;
    if (value == "nearest") return FullscreenInputFilter::nearest;
    throw std::runtime_error(
        "Unknown fullscreen input_sampling filter '" + value +
        "' at input " + std::to_string(input_index) + ": " +
        pass_name);
}

FullscreenInputAddressMode parseInputAddressMode(
    const nlohmann::json &sampling, const std::string &pass_name,
    std::size_t input_index) {
    if (!sampling.contains("address")) {
        return FullscreenInputAddressMode::repeat;
    }
    if (!sampling.at("address").is_string()) {
        throw std::runtime_error(
            "Fullscreen pass input_sampling address must be a string at input " +
            std::to_string(input_index) + ": " + pass_name);
    }
    const auto value = sampling.at("address").get<std::string>();
    if (value == "repeat") {
        return FullscreenInputAddressMode::repeat;
    }
    if (value == "mirrored_repeat") {
        return FullscreenInputAddressMode::mirrored_repeat;
    }
    if (value == "clamp_to_edge") {
        return FullscreenInputAddressMode::clamp_to_edge;
    }
    throw std::runtime_error(
        "Unknown fullscreen input_sampling address '" + value +
        "' at input " + std::to_string(input_index) + ": " +
        pass_name);
}

std::vector<FullscreenInputSampling> parseInputSampling(
    const nlohmann::json &pass_json, const std::string &pass_name) {
    if (!pass_json.contains("input_sampling")) return {};
    const auto &encoded = pass_json.at("input_sampling");
    if (!encoded.is_array()) {
        throw std::runtime_error(
            "Fullscreen pass input_sampling must be an array: " +
            pass_name);
    }
    std::vector<FullscreenInputSampling> result;
    result.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto &entry = encoded.at(index);
        if (!entry.is_object()) {
            throw std::runtime_error(
                "Fullscreen pass input_sampling entries must be objects at input " +
                std::to_string(index) + ": " + pass_name);
        }
        static const std::set<std::string, std::less<>> allowed{
            "filter", "address"};
        for (const auto &[key, value] : entry.items()) {
            (void)value;
            if (!allowed.contains(key)) {
                throw std::runtime_error(
                    "Unknown fullscreen input_sampling field '" + key +
                    "' at input " + std::to_string(index) + ": " +
                    pass_name);
            }
        }
        result.push_back(FullscreenInputSampling{
            .filter = parseInputFilter(entry, pass_name, index),
            .address_mode =
                parseInputAddressMode(entry, pass_name, index),
        });
    }
    return result;
}

} // namespace

FullscreenPassInfo parseFullscreenPassInfoFromJson(const nlohmann::json &pass_json,
                                                   const std::string &pass_name) {
    FullscreenPassInfo fullscreen_info;
    fullscreen_info.input_sampling =
        parseInputSampling(pass_json, pass_name);

    if (pass_json.contains("needs_projection_matrix")) {
        throw std::runtime_error(
            "Fullscreen pass needs_projection_matrix is deprecated; use push_constants: projection_view: " +
            pass_name);
    }

    if (pass_json.contains("push_constants")) {
        if (!pass_json.at("push_constants").is_string()) {
            throw std::runtime_error("Fullscreen pass push_constants must be a string: " + pass_name);
        }
        fullscreen_info.push_constants =
            stringToFullscreenPushConstantData(pass_json.at("push_constants").get<std::string>());
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

    fullscreen_info.vert_shader =
        makeShaderReference(shader.at("vertex").get<std::string>(), ShaderStage::vertex);
    fullscreen_info.frag_shader =
        makeShaderReference(shader.at("fragment").get<std::string>(), ShaderStage::fragment);
    return fullscreen_info;
}

} // namespace Pelican
