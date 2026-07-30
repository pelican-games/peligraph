#include "genericrasterpassinfojsonparser.hpp"

#include "renderingpassjsonhelpers.hpp"
#include "../../project/logicalrendertype.hpp"

#include <algorithm>
#include <set>
#include <span>
#include <stdexcept>

namespace Pelican {
namespace {

std::vector<std::string> parseAuthoredInputs(
    const nlohmann::json &pass_json,
    const std::string &pass_name) {
    if (!pass_json.contains("input") ||
        pass_json.at("input").is_null()) {
        return {};
    }
    const auto &encoded = pass_json.at("input");
    if (encoded.is_string()) {
        return {encoded.get<std::string>()};
    }
    if (!encoded.is_array()) {
        throw std::runtime_error(
            "Raster pass input must be a string or array: " +
            pass_name);
    }
    std::vector<std::string> result;
    result.reserve(encoded.size());
    for (const auto &entry : encoded) {
        if (!entry.is_string()) {
            throw std::runtime_error(
                "Raster pass input entries must be strings: " +
                pass_name);
        }
        result.push_back(entry.get<std::string>());
    }
    return result;
}

void requireVersionedId(
    std::string_view value,
    std::string_view context) {
    try {
        const auto parsed = parseSemanticTypeId(value);
        if (semanticTypeIdName(parsed) != value) {
            throw std::runtime_error(
                "identifier is not canonical");
        }
    } catch (const std::runtime_error &error) {
        throw std::runtime_error(
            std::string{context} +
            " must use canonical namespace.name@major syntax: " +
            std::string{value} + " (" + error.what() + ")");
    }
}

void validatePorts(
    const std::vector<ShaderResourcePortDefinition> &ports,
    const std::string &pass_name) {
    std::set<std::string, std::less<>>
        mapped_resources;
    for (const auto &port : ports) {
        if (!mapped_resources.insert(
                port.resource).second) {
            throw std::runtime_error(
                "Raster resource_ports cannot map multiple ports "
                "to the same input resource '" +
                port.resource + "': " + pass_name);
        }
        const auto is_image =
            port.kind != ShaderResourcePortKind::buffer;
        const auto expected =
            is_image
                ? ShaderResourcePortAccess::sampled
                : ShaderResourcePortAccess::storage;
        if (effectiveShaderResourcePortAccess(
                port, is_image, false) != expected) {
            throw std::runtime_error(
                "Raster resource port '" + port.name +
                "' must use " +
                std::string{
                    expected ==
                            ShaderResourcePortAccess::sampled
                        ? "sampled"
                        : "storage"} +
                " access: " + pass_name);
        }
    }
}

} // namespace

GenericRasterPassInfo parseGenericRasterPassInfoFromJson(
    const nlohmann::json &pass_json,
    const std::string &pass_name,
    std::size_t color_attachment_count,
    bool has_depth_attachment) {
    GenericRasterPassInfo result;
    result.contract = parseRasterPassContract(
        pass_json, color_attachment_count,
        has_depth_attachment,
        "raster pass '" + pass_name + "'");

    const auto authored_inputs =
        parseAuthoredInputs(pass_json, pass_name);
    result.resource_ports =
        parseShaderResourcePortDefinitions(
            pass_json, authored_inputs,
            std::span<const std::string>{},
            "raster pass '" + pass_name + "'");
    validatePorts(result.resource_ports, pass_name);

    if (!pass_json.contains("shader")) {
        throw std::runtime_error(
            "Raster pass requires shader: " +
            pass_name);
    }
    const auto &shader = pass_json.at("shader");
    if (!shader.is_object()) {
        throw std::runtime_error(
            "Raster pass shader must be an object: " +
            pass_name);
    }
    static const std::set<std::string, std::less<>>
        allowed_shader_fields{
            "implementation", "vertex", "fragment"};
    for (const auto &[key, value] : shader.items()) {
        (void)value;
        if (!allowed_shader_fields.contains(key)) {
            throw std::runtime_error(
                "Unknown raster shader field '" + key +
                "': " + pass_name);
        }
    }
    if (!shader.contains("vertex") ||
        !shader.at("vertex").is_string()) {
        throw std::runtime_error(
            "Raster pass shader requires vertex string: " +
            pass_name);
    }
    if (shader.contains("implementation")) {
        if (!shader.at("implementation").is_string() ||
            shader.at("implementation")
                .get<std::string>()
                .empty()) {
            throw std::runtime_error(
                "Raster pass shader implementation must be a "
                "non-empty string: " +
                pass_name);
        }
        result.shader_implementation =
            shader.at("implementation")
                .get<std::string>();
    }
    requireVersionedId(
        result.shader_implementation,
        "Raster pass shader implementation");

    result.vert_shader =
        makeShaderReference(
            shader.at("vertex").get<std::string>(),
            ShaderStage::vertex);
    if (shader.contains("fragment")) {
        if (!shader.at("fragment").is_string()) {
            throw std::runtime_error(
                "Raster pass fragment shader must be a string: " +
                pass_name);
        }
        result.frag_shader =
            makeShaderReference(
                shader.at("fragment")
                    .get<std::string>(),
                ShaderStage::fragment);
    }
    if (color_attachment_count != 0 &&
        !result.frag_shader) {
        throw std::runtime_error(
            "Raster pass with color outputs requires a fragment "
            "shader: " +
            pass_name);
    }
    return result;
}

} // namespace Pelican
