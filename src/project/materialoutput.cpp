#include "materialoutput.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace Pelican {
namespace {

bool isIdentifier(std::string_view value) {
    if (value.empty()) return false;
    const auto first =
        static_cast<unsigned char>(value.front());
    if (std::isalpha(first) == 0 && value.front() != '_') {
        return false;
    }
    return std::all_of(
        value.begin() + 1, value.end(),
        [](unsigned char character) {
            return std::isalnum(character) != 0 ||
                   character == '_';
        });
}

std::string requireString(
    const nlohmann::json &object, std::string_view field,
    std::string_view context) {
    const auto found =
        object.find(std::string{field});
    if (found == object.end() || !found->is_string() ||
        found->get<std::string>().empty()) {
        throw std::runtime_error(
            std::string{context} + " requires non-empty string '" +
            std::string{field} + "'");
    }
    return found->get<std::string>();
}

void requireOnlyFields(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context) {
    for (auto field = object.begin();
         field != object.end(); ++field) {
        if (std::find(
                allowed.begin(), allowed.end(),
                field.key()) == allowed.end()) {
            throw std::runtime_error(
                std::string{context} +
                " has unknown field '" + field.key() + "'");
        }
    }
}

bool isFloating(MaterialOutputType type) {
    return materialOutputNumericClass(type) ==
           MaterialOutputNumericClass::floating;
}

} // namespace

std::string_view materialOutputTypeName(
    MaterialOutputType type) {
    switch (type) {
    case MaterialOutputType::floating: return "float";
    case MaterialOutputType::vec2: return "vec2";
    case MaterialOutputType::vec3: return "vec3";
    case MaterialOutputType::vec4: return "vec4";
    case MaterialOutputType::integer: return "int";
    case MaterialOutputType::ivec2: return "ivec2";
    case MaterialOutputType::ivec3: return "ivec3";
    case MaterialOutputType::ivec4: return "ivec4";
    case MaterialOutputType::unsigned_integer: return "uint";
    case MaterialOutputType::uvec2: return "uvec2";
    case MaterialOutputType::uvec3: return "uvec3";
    case MaterialOutputType::uvec4: return "uvec4";
    }
    throw std::runtime_error("unknown material output type");
}

std::optional<MaterialOutputType>
materialOutputTypeFromName(std::string_view name) {
    if (name == "float") return MaterialOutputType::floating;
    if (name == "vec2") return MaterialOutputType::vec2;
    if (name == "vec3") return MaterialOutputType::vec3;
    if (name == "vec4") return MaterialOutputType::vec4;
    if (name == "int") return MaterialOutputType::integer;
    if (name == "ivec2") return MaterialOutputType::ivec2;
    if (name == "ivec3") return MaterialOutputType::ivec3;
    if (name == "ivec4") return MaterialOutputType::ivec4;
    if (name == "uint") {
        return MaterialOutputType::unsigned_integer;
    }
    if (name == "uvec2") return MaterialOutputType::uvec2;
    if (name == "uvec3") return MaterialOutputType::uvec3;
    if (name == "uvec4") return MaterialOutputType::uvec4;
    return std::nullopt;
}

MaterialOutputNumericClass materialOutputNumericClass(
    MaterialOutputType type) {
    switch (type) {
    case MaterialOutputType::floating:
    case MaterialOutputType::vec2:
    case MaterialOutputType::vec3:
    case MaterialOutputType::vec4:
        return MaterialOutputNumericClass::floating;
    case MaterialOutputType::integer:
    case MaterialOutputType::ivec2:
    case MaterialOutputType::ivec3:
    case MaterialOutputType::ivec4:
        return MaterialOutputNumericClass::signed_integer;
    case MaterialOutputType::unsigned_integer:
    case MaterialOutputType::uvec2:
    case MaterialOutputType::uvec3:
    case MaterialOutputType::uvec4:
        return MaterialOutputNumericClass::unsigned_integer;
    }
    throw std::runtime_error("unknown material output type");
}

std::string_view materialOutputSourceName(
    MaterialOutputSource source) {
    switch (source) {
    case MaterialOutputSource::custom: return "custom";
    case MaterialOutputSource::surface_base_color:
        return "surface.base_color";
    case MaterialOutputSource::surface_normal:
        return "surface.normal";
    case MaterialOutputSource::surface_normal_encoded:
        return "surface.normal_encoded";
    case MaterialOutputSource::surface_material:
        return "surface.material";
    case MaterialOutputSource::input_world_position:
        return "input.world_position";
    case MaterialOutputSource::surface_emissive:
        return "surface.emissive";
    case MaterialOutputSource::lighting_scene_color:
        return "lighting.scene_color";
    }
    throw std::runtime_error("unknown material output source");
}

std::optional<MaterialOutputSource>
materialOutputSourceFromName(std::string_view name) {
    if (name == "custom") return MaterialOutputSource::custom;
    if (name == "surface.base_color") {
        return MaterialOutputSource::surface_base_color;
    }
    if (name == "surface.normal") {
        return MaterialOutputSource::surface_normal;
    }
    if (name == "surface.normal_encoded") {
        return MaterialOutputSource::surface_normal_encoded;
    }
    if (name == "surface.material") {
        return MaterialOutputSource::surface_material;
    }
    if (name == "input.world_position") {
        return MaterialOutputSource::input_world_position;
    }
    if (name == "surface.emissive") {
        return MaterialOutputSource::surface_emissive;
    }
    if (name == "lighting.scene_color") {
        return MaterialOutputSource::lighting_scene_color;
    }
    return std::nullopt;
}

void validateMaterialOutputSchema(
    const MaterialOutputSchema &schema,
    std::string_view context) {
    if (schema.name.empty()) {
        throw std::runtime_error(
            std::string{context} + " name must not be empty");
    }
    if (schema.outputs.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " outputs must not be empty");
    }
    std::unordered_set<std::string> names;
    for (std::size_t location = 0;
         location < schema.outputs.size(); ++location) {
        const auto &output = schema.outputs[location];
        const auto output_context =
            std::string{context} + " output[" +
            std::to_string(location) + "]";
        if (!isIdentifier(output.name)) {
            throw std::runtime_error(
                output_context + " name '" + output.name +
                "' must be a shader identifier");
        }
        if (!names.insert(output.name).second) {
            throw std::runtime_error(
                std::string{context} +
                " output names must be unique: " +
                output.name);
        }
        if (output.source != MaterialOutputSource::custom &&
            !isFloating(output.type)) {
            throw std::runtime_error(
                output_context + " source '" +
                std::string{
                    materialOutputSourceName(output.source)} +
                "' requires a floating output type");
        }
    }
}

MaterialOutputSchema parseMaterialOutputSchema(
    const nlohmann::json &declaration,
    std::string_view context) {
    if (!declaration.is_object()) {
        throw std::runtime_error(
            std::string{context} + " must be an object");
    }
    requireOnlyFields(
        declaration,
        {"schema", "version", "name", "outputs"},
        context);
    const auto schema_name =
        requireString(declaration, "schema", context);
    if (schema_name != "pelican.material_outputs") {
        throw std::runtime_error(
            std::string{context} +
            " has unsupported schema '" + schema_name + "'");
    }
    const auto version = declaration.find("version");
    if (version == declaration.end() ||
        !version->is_number_integer() ||
        version->get<int>() != 1) {
        throw std::runtime_error(
            std::string{context} +
            " requires version 1");
    }
    const auto outputs = declaration.find("outputs");
    if (outputs == declaration.end() ||
        !outputs->is_array()) {
        throw std::runtime_error(
            std::string{context} +
            " requires array outputs");
    }

    MaterialOutputSchema result;
    result.name = requireString(
        declaration, "name", context);
    result.outputs.reserve(outputs->size());
    for (std::size_t location = 0;
         location < outputs->size(); ++location) {
        const auto &encoded = outputs->at(location);
        const auto output_context =
            std::string{context} + " output[" +
            std::to_string(location) + "]";
        if (!encoded.is_object()) {
            throw std::runtime_error(
                output_context + " must be an object");
        }
        requireOnlyFields(
            encoded, {"name", "type", "source"},
            output_context);
        const auto type_name =
            requireString(encoded, "type", output_context);
        const auto type =
            materialOutputTypeFromName(type_name);
        if (!type) {
            throw std::runtime_error(
                output_context + " has unknown type '" +
                type_name + "'");
        }
        const auto source_name =
            requireString(encoded, "source", output_context);
        const auto source =
            materialOutputSourceFromName(source_name);
        if (!source) {
            throw std::runtime_error(
                output_context + " has unknown source '" +
                source_name + "'");
        }
        result.outputs.push_back(MaterialOutputField{
            requireString(encoded, "name", output_context),
            *type,
            *source,
        });
    }
    validateMaterialOutputSchema(result, context);
    return result;
}

std::string materialOutputSchemaFingerprint(
    const MaterialOutputSchema &schema) {
    validateMaterialOutputSchema(schema);
    std::ostringstream result;
    result << "pelican.material_outputs@1:" << schema.name;
    for (std::size_t location = 0;
         location < schema.outputs.size(); ++location) {
        const auto &output = schema.outputs[location];
        result << ';' << location << ':' << output.name << ':'
               << materialOutputTypeName(output.type) << ':'
               << materialOutputSourceName(output.source);
    }
    return result.str();
}

nlohmann::json materialOutputSchemaToJson(
    const MaterialOutputSchema &schema) {
    validateMaterialOutputSchema(schema);
    nlohmann::json outputs = nlohmann::json::array();
    for (const auto &output : schema.outputs) {
        outputs.push_back({
            {"name", output.name},
            {"type", materialOutputTypeName(output.type)},
            {"source", materialOutputSourceName(output.source)},
        });
    }
    return {
        {"schema", "pelican.material_outputs"},
        {"version", 1},
        {"name", schema.name},
        {"outputs", std::move(outputs)},
    };
}

} // namespace Pelican
