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

MaterialOutputBlendEquation parseBlendEquation(
    const nlohmann::json &declaration,
    std::string_view context) {
    if (!declaration.is_object()) {
        throw std::runtime_error(
            std::string{context} + " must be an object");
    }
    requireOnlyFields(
        declaration, {"src", "dst", "op"}, context);
    const auto source_name =
        requireString(declaration, "src", context);
    const auto destination_name =
        requireString(declaration, "dst", context);
    const auto operation_name =
        requireString(declaration, "op", context);
    const auto source =
        materialOutputBlendFactorFromName(source_name);
    const auto destination =
        materialOutputBlendFactorFromName(destination_name);
    const auto operation =
        materialOutputBlendOperationFromName(operation_name);
    if (!source) {
        throw std::runtime_error(
            std::string{context} +
            " has unknown src blend factor '" +
            source_name + "'");
    }
    if (!destination) {
        throw std::runtime_error(
            std::string{context} +
            " has unknown dst blend factor '" +
            destination_name + "'");
    }
    if (!operation) {
        throw std::runtime_error(
            std::string{context} +
            " has unknown blend operation '" +
            operation_name + "'");
    }
    return {*source, *destination, *operation};
}

MaterialOutputBlendState blendPreset(
    std::string_view name, std::string_view context) {
    MaterialOutputBlendState result;
    if (name == "opaque") return result;
    result.enabled = true;
    if (name == "blend") {
        result.color = {
            MaterialOutputBlendFactor::source_alpha,
            MaterialOutputBlendFactor::
                one_minus_source_alpha,
            MaterialOutputBlendOperation::add};
        result.alpha = {
            MaterialOutputBlendFactor::one,
            MaterialOutputBlendFactor::
                one_minus_source_alpha,
            MaterialOutputBlendOperation::add};
        return result;
    }
    if (name == "additive") {
        result.color = {
            MaterialOutputBlendFactor::source_alpha,
            MaterialOutputBlendFactor::one,
            MaterialOutputBlendOperation::add};
        result.alpha = {
            MaterialOutputBlendFactor::one,
            MaterialOutputBlendFactor::one,
            MaterialOutputBlendOperation::add};
        return result;
    }
    throw std::runtime_error(
        std::string{context} +
        " has unknown blend preset '" +
        std::string{name} + "'");
}

MaterialOutputBlendState parseBlendState(
    const nlohmann::json &declaration,
    std::string_view context) {
    if (declaration.is_string()) {
        return blendPreset(
            declaration.get<std::string>(), context);
    }
    if (!declaration.is_object()) {
        throw std::runtime_error(
            std::string{context} +
            " must be opaque, blend, additive, or an object");
    }
    requireOnlyFields(
        declaration, {"color", "alpha"}, context);
    const auto color = declaration.find("color");
    const auto alpha = declaration.find("alpha");
    if (color == declaration.end() ||
        alpha == declaration.end()) {
        throw std::runtime_error(
            std::string{context} +
            " requires color and alpha equations");
    }
    return {
        true,
        parseBlendEquation(
            *color, std::string{context} + " color"),
        parseBlendEquation(
            *alpha, std::string{context} + " alpha"),
    };
}

std::uint8_t parseWriteMask(
    const nlohmann::json &declaration,
    std::string_view context) {
    if (!declaration.is_string()) {
        throw std::runtime_error(
            std::string{context} + " must be a string");
    }
    const auto value =
        declaration.get<std::string>();
    if (value == "none") return 0;
    if (value.empty()) {
        throw std::runtime_error(
            std::string{context} +
            " must use 'none' for an empty mask");
    }
    std::uint8_t result = 0;
    for (const auto channel : value) {
        std::uint8_t bit = 0;
        switch (channel) {
        case 'r': bit = materialOutputWriteRed; break;
        case 'g': bit = materialOutputWriteGreen; break;
        case 'b': bit = materialOutputWriteBlue; break;
        case 'a': bit = materialOutputWriteAlpha; break;
        default:
            throw std::runtime_error(
                std::string{context} +
                " contains unknown channel '" +
                std::string{channel} + "'");
        }
        if ((result & bit) != 0) {
            throw std::runtime_error(
                std::string{context} +
                " contains duplicate channel '" +
                std::string{channel} + "'");
        }
        result |= bit;
    }
    return result;
}

std::string writeMaskName(std::uint8_t mask) {
    if (mask == 0) return "none";
    std::string result;
    if ((mask & materialOutputWriteRed) != 0)
        result.push_back('r');
    if ((mask & materialOutputWriteGreen) != 0)
        result.push_back('g');
    if ((mask & materialOutputWriteBlue) != 0)
        result.push_back('b');
    if ((mask & materialOutputWriteAlpha) != 0)
        result.push_back('a');
    return result;
}

nlohmann::json blendEquationToJson(
    const MaterialOutputBlendEquation &equation) {
    return {
        {"src", materialOutputBlendFactorName(
                    equation.source)},
        {"dst", materialOutputBlendFactorName(
                    equation.destination)},
        {"op", materialOutputBlendOperationName(
                   equation.operation)},
    };
}

nlohmann::json blendStateToJson(
    const MaterialOutputBlendState &state) {
    if (!state.enabled) return "opaque";
    return {
        {"color", blendEquationToJson(state.color)},
        {"alpha", blendEquationToJson(state.alpha)},
    };
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

std::string_view materialOutputBlendFactorName(
    MaterialOutputBlendFactor factor) {
    switch (factor) {
    case MaterialOutputBlendFactor::zero: return "zero";
    case MaterialOutputBlendFactor::one: return "one";
    case MaterialOutputBlendFactor::source_color:
        return "src_color";
    case MaterialOutputBlendFactor::one_minus_source_color:
        return "one_minus_src_color";
    case MaterialOutputBlendFactor::destination_color:
        return "dst_color";
    case MaterialOutputBlendFactor::
        one_minus_destination_color:
        return "one_minus_dst_color";
    case MaterialOutputBlendFactor::source_alpha:
        return "src_alpha";
    case MaterialOutputBlendFactor::one_minus_source_alpha:
        return "one_minus_src_alpha";
    case MaterialOutputBlendFactor::destination_alpha:
        return "dst_alpha";
    case MaterialOutputBlendFactor::
        one_minus_destination_alpha:
        return "one_minus_dst_alpha";
    case MaterialOutputBlendFactor::source_alpha_saturate:
        return "src_alpha_saturate";
    }
    throw std::runtime_error(
        "unknown material output blend factor");
}

std::optional<MaterialOutputBlendFactor>
materialOutputBlendFactorFromName(std::string_view name) {
    if (name == "zero")
        return MaterialOutputBlendFactor::zero;
    if (name == "one")
        return MaterialOutputBlendFactor::one;
    if (name == "src_color")
        return MaterialOutputBlendFactor::source_color;
    if (name == "one_minus_src_color")
        return MaterialOutputBlendFactor::
            one_minus_source_color;
    if (name == "dst_color")
        return MaterialOutputBlendFactor::destination_color;
    if (name == "one_minus_dst_color")
        return MaterialOutputBlendFactor::
            one_minus_destination_color;
    if (name == "src_alpha")
        return MaterialOutputBlendFactor::source_alpha;
    if (name == "one_minus_src_alpha")
        return MaterialOutputBlendFactor::
            one_minus_source_alpha;
    if (name == "dst_alpha")
        return MaterialOutputBlendFactor::destination_alpha;
    if (name == "one_minus_dst_alpha")
        return MaterialOutputBlendFactor::
            one_minus_destination_alpha;
    if (name == "src_alpha_saturate")
        return MaterialOutputBlendFactor::
            source_alpha_saturate;
    return std::nullopt;
}

std::string_view materialOutputBlendOperationName(
    MaterialOutputBlendOperation operation) {
    switch (operation) {
    case MaterialOutputBlendOperation::add: return "add";
    case MaterialOutputBlendOperation::subtract:
        return "subtract";
    case MaterialOutputBlendOperation::reverse_subtract:
        return "reverse_subtract";
    case MaterialOutputBlendOperation::minimum: return "min";
    case MaterialOutputBlendOperation::maximum: return "max";
    }
    throw std::runtime_error(
        "unknown material output blend operation");
}

std::optional<MaterialOutputBlendOperation>
materialOutputBlendOperationFromName(std::string_view name) {
    if (name == "add")
        return MaterialOutputBlendOperation::add;
    if (name == "subtract")
        return MaterialOutputBlendOperation::subtract;
    if (name == "reverse_subtract")
        return MaterialOutputBlendOperation::reverse_subtract;
    if (name == "min")
        return MaterialOutputBlendOperation::minimum;
    if (name == "max")
        return MaterialOutputBlendOperation::maximum;
    return std::nullopt;
}

void validateMaterialOutputAttachmentStates(
    const std::vector<MaterialOutputAttachmentState> &states,
    const MaterialOutputSchema &schema,
    std::string_view context) {
    validateMaterialOutputSchema(schema, context);
    std::unordered_set<std::string> names;
    std::size_t previous_location = 0;
    bool has_previous = false;
    for (const auto &state : states) {
        const auto found = std::find_if(
            schema.outputs.begin(), schema.outputs.end(),
            [&](const auto &output) {
                return output.name == state.output;
            });
        if (found == schema.outputs.end()) {
            throw std::runtime_error(
                std::string{context} +
                " references unknown output '" +
                state.output + "'");
        }
        if (!names.insert(state.output).second) {
            throw std::runtime_error(
                std::string{context} +
                " contains duplicate output '" +
                state.output + "'");
        }
        const auto location = static_cast<std::size_t>(
            std::distance(schema.outputs.begin(), found));
        if (has_previous && location <= previous_location) {
            throw std::runtime_error(
                std::string{context} +
                " states must be stored in material output order");
        }
        previous_location = location;
        has_previous = true;
        if (!state.blend && !state.write_mask) {
            throw std::runtime_error(
                std::string{context} + " output '" +
                state.output +
                "' must override blend or write_mask");
        }
        if (state.write_mask &&
            (*state.write_mask &
             static_cast<std::uint8_t>(
                 ~materialOutputWriteRgba)) !=
                0) {
            throw std::runtime_error(
                std::string{context} + " output '" +
                state.output +
                "' has invalid write_mask bits");
        }
        if (state.blend && state.blend->enabled &&
            materialOutputNumericClass(found->type) !=
                MaterialOutputNumericClass::floating) {
            throw std::runtime_error(
                std::string{context} + " output '" +
                state.output +
                "' cannot enable blending for an integer output");
        }
    }
}

std::vector<MaterialOutputAttachmentState>
parseMaterialOutputAttachmentStates(
    const nlohmann::json &declaration,
    const MaterialOutputSchema &schema,
    std::string_view context) {
    if (!declaration.is_object()) {
        throw std::runtime_error(
            std::string{context} + " must be an object");
    }
    std::vector<MaterialOutputAttachmentState> result;
    result.reserve(declaration.size());
    for (const auto &output : schema.outputs) {
        const auto encoded =
            declaration.find(output.name);
        if (encoded == declaration.end()) continue;
        const auto output_context =
            std::string{context} + " output '" +
            output.name + "'";
        if (!encoded->is_object()) {
            throw std::runtime_error(
                output_context + " must be an object");
        }
        requireOnlyFields(
            *encoded, {"blend", "write_mask"},
            output_context);
        MaterialOutputAttachmentState state{
            .output = output.name};
        if (const auto blend = encoded->find("blend");
            blend != encoded->end()) {
            state.blend = parseBlendState(
                *blend, output_context + " blend");
        }
        if (const auto write_mask =
                encoded->find("write_mask");
            write_mask != encoded->end()) {
            state.write_mask = parseWriteMask(
                *write_mask,
                output_context + " write_mask");
        }
        result.push_back(std::move(state));
    }
    for (auto entry = declaration.begin();
         entry != declaration.end(); ++entry) {
        const auto known = std::any_of(
            schema.outputs.begin(), schema.outputs.end(),
            [&](const auto &output) {
                return output.name == entry.key();
            });
        if (!known) {
            throw std::runtime_error(
                std::string{context} +
                " references unknown output '" +
                entry.key() + "'");
        }
    }
    validateMaterialOutputAttachmentStates(
        result, schema, context);
    return result;
}

std::string materialOutputAttachmentStatesFingerprint(
    const std::vector<MaterialOutputAttachmentState> &states,
    const MaterialOutputSchema &schema) {
    validateMaterialOutputAttachmentStates(states, schema);
    std::ostringstream result;
    result << "pelican.material_output_states@1:"
           << materialOutputSchemaFingerprint(schema);
    for (const auto &state : states) {
        result << ';' << state.output;
        if (state.blend) {
            result << ":blend="
                   << state.blend->enabled;
            if (state.blend->enabled) {
                const auto append_equation =
                    [&](const auto &equation) {
                        result << ','
                               << materialOutputBlendFactorName(
                                      equation.source)
                               << ','
                               << materialOutputBlendFactorName(
                                      equation.destination)
                               << ','
                               << materialOutputBlendOperationName(
                                      equation.operation);
                    };
                append_equation(state.blend->color);
                append_equation(state.blend->alpha);
            }
        }
        if (state.write_mask) {
            result << ":write="
                   << writeMaskName(
                          *state.write_mask);
        }
    }
    return result.str();
}

nlohmann::json materialOutputAttachmentStatesToJson(
    const std::vector<MaterialOutputAttachmentState> &states,
    const MaterialOutputSchema &schema) {
    validateMaterialOutputAttachmentStates(states, schema);
    nlohmann::json result = nlohmann::json::object();
    for (const auto &state : states) {
        nlohmann::json encoded = nlohmann::json::object();
        if (state.blend) {
            encoded["blend"] =
                blendStateToJson(*state.blend);
        }
        if (state.write_mask) {
            encoded["write_mask"] =
                writeMaskName(*state.write_mask);
        }
        result[state.output] = std::move(encoded);
    }
    return result;
}

} // namespace Pelican
