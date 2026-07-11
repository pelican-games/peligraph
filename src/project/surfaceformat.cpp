#include "surfaceformat.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view surface_magic = "//! pelican.surface v1";

enum class HeaderSection {
    none,
    params,
    textures,
    screen_inputs,
    unknown,
};

struct HeaderLine {
    std::string_view text;
    std::size_t next_offset = 0;
    std::size_t number = 0;
};

using InlineFields = std::vector<std::pair<std::string, std::string>>;

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string{value.substr(begin, end - begin)};
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool isIdentifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (std::isalpha(first) == 0 && value.front() != '_') {
        return false;
    }
    for (const char ch : value.substr(1)) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) == 0 && ch != '_') {
            return false;
        }
    }
    return true;
}

std::string surfaceContext(std::string_view source_name) {
    return "surface '" + std::string{source_name} + "'";
}

std::string lineContext(std::string_view source_name, std::size_t line_number) {
    return surfaceContext(source_name) + " line " + std::to_string(line_number);
}

HeaderLine readLine(std::string_view source, std::size_t offset, std::size_t number) {
    const auto newline = source.find('\n', offset);
    const auto line_end = newline == std::string_view::npos ? source.size() : newline;
    auto text = source.substr(offset, line_end - offset);
    if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }
    return HeaderLine{
        text,
        newline == std::string_view::npos ? source.size() : newline + 1,
        number,
    };
}

std::vector<std::string> splitTopLevel(std::string_view value, char delimiter,
                                       std::string_view context) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    int square_depth = 0;
    int brace_depth = 0;
    char quote = '\0';
    bool escaped = false;

    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == '[') {
            ++square_depth;
        } else if (ch == ']') {
            --square_depth;
        } else if (ch == '{') {
            ++brace_depth;
        } else if (ch == '}') {
            --brace_depth;
        } else if (ch == delimiter && square_depth == 0 && brace_depth == 0) {
            parts.push_back(trim(value.substr(begin, i - begin)));
            begin = i + 1;
        }
        if (square_depth < 0 || brace_depth < 0) {
            throw std::runtime_error(std::string{context} + " has unbalanced delimiters");
        }
    }

    if (quote != '\0' || square_depth != 0 || brace_depth != 0) {
        throw std::runtime_error(std::string{context} + " has unbalanced delimiters");
    }
    parts.push_back(trim(value.substr(begin)));
    return parts;
}

std::size_t findTopLevelColon(std::string_view value) {
    int square_depth = 0;
    int brace_depth = 0;
    char quote = '\0';
    bool escaped = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (quote != '\0') {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == '[') {
            ++square_depth;
        } else if (ch == ']') {
            --square_depth;
        } else if (ch == '{') {
            ++brace_depth;
        } else if (ch == '}') {
            --brace_depth;
        } else if (ch == ':' && square_depth == 0 && brace_depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::string parseStringToken(std::string_view token, std::string_view context) {
    const auto value = trim(token);
    if (value.empty()) {
        throw std::runtime_error(std::string{context} + " must not be empty");
    }
    if (value.front() == '"') {
        try {
            const auto parsed = nlohmann::json::parse(value);
            if (!parsed.is_string()) {
                throw std::runtime_error(std::string{context} + " must be a string");
            }
            return parsed.get<std::string>();
        } catch (const nlohmann::json::exception &) {
            throw std::runtime_error(std::string{context} + " has an invalid quoted string");
        }
    }
    if (value.front() == '\'' && value.size() >= 2 && value.back() == '\'') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

InlineFields parseInlineFields(std::string_view value, std::string_view context) {
    const auto object = trim(value);
    if (object.size() < 2 || object.front() != '{' || object.back() != '}') {
        throw std::runtime_error(std::string{context} + " must be an inline mapping");
    }

    const auto body = std::string_view{object}.substr(1, object.size() - 2);
    if (trim(body).empty()) {
        return {};
    }

    InlineFields fields;
    std::unordered_set<std::string> seen;
    for (const auto &part : splitTopLevel(body, ',', context)) {
        const auto colon = findTopLevelColon(part);
        if (colon == std::string_view::npos) {
            throw std::runtime_error(std::string{context} + " field requires ':'");
        }
        auto key = parseStringToken(std::string_view{part}.substr(0, colon), context);
        auto field_value = trim(std::string_view{part}.substr(colon + 1));
        if (field_value.empty()) {
            throw std::runtime_error(std::string{context} + " field '" + key + "' requires a value");
        }
        if (!seen.insert(key).second) {
            throw std::runtime_error(std::string{context} + " has duplicate field '" + key + "'");
        }
        fields.emplace_back(std::move(key), std::move(field_value));
    }
    return fields;
}

const std::string *findField(const InlineFields &fields, std::string_view name) {
    const auto it = std::find_if(fields.begin(), fields.end(), [name](const auto &field) {
        return field.first == name;
    });
    return it == fields.end() ? nullptr : &it->second;
}

bool isKnownField(std::string_view field, std::initializer_list<std::string_view> known_fields) {
    return std::find(known_fields.begin(), known_fields.end(), field) != known_fields.end();
}

void appendUnknownFieldWarnings(const InlineFields &fields,
                                std::initializer_list<std::string_view> known_fields,
                                std::string_view context,
                                std::vector<std::string> &warnings) {
    for (const auto &[field, value] : fields) {
        (void)value;
        if (!isKnownField(field, known_fields)) {
            warnings.push_back(std::string{context} + " ignored unknown key '" + field + "'");
        }
    }
}

SurfaceLanguage parseLanguage(std::string_view token, std::string_view context) {
    const auto language = parseStringToken(token, context);
    if (language == "glsl") {
        return SurfaceLanguage::glsl;
    }
    if (language == "hlsl") {
        return SurfaceLanguage::hlsl;
    }
    if (language == "slang") {
        return SurfaceLanguage::slang;
    }
    throw std::runtime_error(std::string{context} + " has unknown language '" + language + "'");
}

SurfaceParamType parseParamType(std::string_view token, std::string_view context) {
    const auto type = parseStringToken(token, context);
    if (type == "float") {
        return SurfaceParamType::floating;
    }
    if (type == "vec2") {
        return SurfaceParamType::vec2;
    }
    if (type == "vec3") {
        return SurfaceParamType::vec3;
    }
    if (type == "vec4") {
        return SurfaceParamType::vec4;
    }
    if (type == "int") {
        return SurfaceParamType::integer;
    }
    if (type == "color") {
        return SurfaceParamType::color;
    }
    throw std::runtime_error(std::string{context} + " has unknown type '" + type + "'");
}

std::size_t vectorWidth(SurfaceParamType type) {
    switch (type) {
    case SurfaceParamType::vec2:
        return 2;
    case SurfaceParamType::vec3:
        return 3;
    case SurfaceParamType::vec4:
        return 4;
    case SurfaceParamType::color:
        return 4;
    default:
        return 0;
    }
}

SurfaceParamValue parseParamValue(std::string_view token, SurfaceParamType type,
                                  std::string_view context) {
    nlohmann::json value;
    try {
        value = nlohmann::json::parse(trim(token));
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error(std::string{context} + " must match declared type " +
                                 std::string{surfaceParamTypeName(type)});
    }

    SurfaceParamValue parsed;
    parsed.type = type;
    if (type == SurfaceParamType::floating) {
        if (!value.is_number()) {
            throw std::runtime_error(std::string{context} + " must match declared type float");
        }
        parsed.values[0] = value.get<double>();
        parsed.component_count = 1;
        return parsed;
    }
    if (type == SurfaceParamType::integer) {
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            throw std::runtime_error(std::string{context} + " must match declared type int");
        }
        try {
            parsed.integer_value = value.get<std::int64_t>();
        } catch (const nlohmann::json::exception &) {
            throw std::runtime_error(std::string{context} + " is outside the int range");
        }
        parsed.component_count = 1;
        return parsed;
    }

    if (type == SurfaceParamType::color) {
        if (!value.is_array() || (value.size() != 3 && value.size() != 4)) {
            throw std::runtime_error(std::string{context} +
                                     " must match declared type color (RGB or RGBA)");
        }
        parsed.values[3] = 1.0;
        parsed.component_count = static_cast<std::uint8_t>(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (!value.at(i).is_number()) {
                throw std::runtime_error(std::string{context} +
                                         " must match declared type color (RGB or RGBA)");
            }
            parsed.values[i] = value.at(i).get<double>();
        }
        return parsed;
    }

    const auto width = vectorWidth(type);
    if (!value.is_array() || value.size() != width) {
        throw std::runtime_error(std::string{context} + " must match declared type " +
                                 std::string{surfaceParamTypeName(type)});
    }
    for (std::size_t i = 0; i < width; ++i) {
        if (!value.at(i).is_number()) {
            throw std::runtime_error(std::string{context} + " must match declared type " +
                                     std::string{surfaceParamTypeName(type)});
        }
        parsed.values[i] = value.at(i).get<double>();
    }
    parsed.component_count = static_cast<std::uint8_t>(width);
    return parsed;
}

double parseNumericMetadata(std::string_view token, std::string_view context) {
    try {
        const auto value = nlohmann::json::parse(trim(token));
        if (!value.is_number()) {
            throw std::runtime_error(std::string{context} + " must be numeric");
        }
        return value.get<double>();
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error(std::string{context} + " must be numeric");
    }
}

void validateReference(std::string_view reference, std::string_view context) {
    if (reference.empty()) {
        throw std::runtime_error(std::string{context} + " default must not be empty");
    }
    if (reference.find('\\') != std::string_view::npos) {
        throw std::runtime_error(std::string{context} + " default must use forward slashes: " +
                                 std::string{reference});
    }
    if (!startsWith(reference, "project://") && !startsWith(reference, "engine://")) {
        throw std::runtime_error(std::string{context} +
                                 " default must be a project:// or engine:// reference: " +
                                 std::string{reference});
    }
}

SurfaceParamDefinition parseParamDefinition(std::string_view mapping, std::string_view source_name,
                                            std::size_t line_number,
                                            std::vector<std::string> &warnings) {
    const auto base_context = lineContext(source_name, line_number) + " param";
    const auto fields = parseInlineFields(mapping, base_context);
    const auto *name_field = findField(fields, "name");
    const auto name = name_field == nullptr ? std::string{"<unnamed>"}
                                            : parseStringToken(*name_field, base_context + " name");
    const auto context = surfaceContext(source_name) + " param '" + name + "'";
    appendUnknownFieldWarnings(fields,
                               {"name", "type", "default", "min", "max", "hint", "encoding"},
                               context, warnings);

    if (name_field == nullptr) {
        throw std::runtime_error(context + " requires name");
    }
    if (!isIdentifier(name)) {
        throw std::runtime_error(context + " name must be a shader identifier");
    }
    const auto *type_field = findField(fields, "type");
    if (type_field == nullptr) {
        throw std::runtime_error(context + " requires explicit type");
    }
    const auto type = parseParamType(*type_field, context);
    const auto *default_field = findField(fields, "default");
    if (default_field == nullptr) {
        throw std::runtime_error(context + " requires default");
    }

    SurfaceParamDefinition definition;
    definition.name = name;
    definition.type = type;
    definition.default_value = parseParamValue(*default_field, type, context + " default");
    if (const auto *min_field = findField(fields, "min")) {
        definition.min = parseNumericMetadata(*min_field, context + " min");
    }
    if (const auto *max_field = findField(fields, "max")) {
        definition.max = parseNumericMetadata(*max_field, context + " max");
    }
    if (const auto *hint_field = findField(fields, "hint")) {
        definition.hint = parseStringToken(*hint_field, context + " hint");
    }
    if (const auto *encoding_field = findField(fields, "encoding")) {
        definition.encoding = parseStringToken(*encoding_field, context + " encoding");
        if (definition.encoding != "srgb" && definition.encoding != "linear") {
            throw std::runtime_error(context + " has unknown encoding '" + definition.encoding + "'");
        }
        if (type != SurfaceParamType::color) {
            throw std::runtime_error(context + " encoding is only valid for type color");
        }
    } else if (type == SurfaceParamType::color) {
        definition.encoding = "srgb";
    }
    return definition;
}

SurfaceTextureDefinition parseTextureDefinition(std::string_view mapping,
                                                std::string_view source_name,
                                                std::size_t line_number,
                                                std::vector<std::string> &warnings) {
    const auto base_context = lineContext(source_name, line_number) + " texture";
    const auto fields = parseInlineFields(mapping, base_context);
    const auto *name_field = findField(fields, "name");
    const auto name = name_field == nullptr ? std::string{"<unnamed>"}
                                            : parseStringToken(*name_field, base_context + " name");
    const auto context = surfaceContext(source_name) + " texture '" + name + "'";
    appendUnknownFieldWarnings(fields, {"name", "default", "color_space", "role"}, context,
                               warnings);

    if (name_field == nullptr) {
        throw std::runtime_error(context + " requires name");
    }
    if (!isIdentifier(name)) {
        throw std::runtime_error(context + " name must be a shader identifier");
    }
    const auto *default_field = findField(fields, "default");
    if (default_field == nullptr) {
        throw std::runtime_error(context + " requires default");
    }
    const auto default_reference = parseStringToken(*default_field, context + " default");
    validateReference(default_reference, context);

    const auto *color_space_field = findField(fields, "color_space");
    if (color_space_field == nullptr) {
        throw std::runtime_error(context + " requires color_space");
    }
    const auto color_space = parseStringToken(*color_space_field, context + " color_space");
    if (color_space != "linear" && color_space != "srgb") {
        throw std::runtime_error(context + " has unknown color_space '" + color_space + "'");
    }
    SurfaceTextureRole role = color_space == "srgb" ? SurfaceTextureRole::color
                                                     : SurfaceTextureRole::data;
    if (const auto *role_field = findField(fields, "role")) {
        const auto role_name = parseStringToken(*role_field, context + " role");
        if (role_name == "color") {
            role = SurfaceTextureRole::color;
        } else if (role_name == "data") {
            role = SurfaceTextureRole::data;
        } else {
            throw std::runtime_error(context + " has unknown role '" + role_name + "'");
        }
        if ((role == SurfaceTextureRole::color) != (color_space == "srgb")) {
            throw std::runtime_error(context + " role '" + role_name +
                                     "' conflicts with color_space '" + color_space + "'");
        }
    }
    return SurfaceTextureDefinition{name, default_reference, color_space, role};
}

std::vector<std::string> parseStringArray(std::string_view value, std::string_view context) {
    const auto array = trim(value);
    if (array.size() < 2 || array.front() != '[' || array.back() != ']') {
        throw std::runtime_error(std::string{context} + " must be an array");
    }
    const auto body = std::string_view{array}.substr(1, array.size() - 2);
    if (trim(body).empty()) {
        return {};
    }

    std::vector<std::string> result;
    for (const auto &token : splitTopLevel(body, ',', context)) {
        result.push_back(parseStringToken(token, context));
    }
    return result;
}

bool parseBoolToken(std::string_view token, std::string_view context) {
    try {
        const auto value = nlohmann::json::parse(trim(token));
        if (!value.is_boolean()) {
            throw std::runtime_error(std::string{context} + " must be true or false");
        }
        return value.get<bool>();
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error(std::string{context} + " must be true or false");
    }
}

SurfaceRenderState parseRenderState(std::string_view value, std::string_view source_name,
                                    std::vector<std::string> &warnings) {
    const auto context = surfaceContext(source_name) + " render_state";
    const auto fields = parseInlineFields(value, context);
    appendUnknownFieldWarnings(fields,
                               {"blend", "cull", "depth", "depth_test", "depth_write",
                                "depth_compare"},
                               context, warnings);

    SurfaceRenderState state;
    if (const auto *field = findField(fields, "blend")) {
        const auto name = parseStringToken(*field, context + " blend");
        if (name == "opaque") {
            state.blend = SurfaceBlendMode::opaque;
        } else if (name == "blend") {
            state.blend = SurfaceBlendMode::blend;
        } else if (name == "additive") {
            state.blend = SurfaceBlendMode::additive;
        } else {
            throw std::runtime_error(context + " has unknown blend '" + name + "'");
        }
    }
    if (const auto *field = findField(fields, "cull")) {
        const auto name = parseStringToken(*field, context + " cull");
        if (name == "none") {
            state.cull = SurfaceCullMode::none;
        } else if (name == "front") {
            state.cull = SurfaceCullMode::front;
        } else if (name == "back") {
            state.cull = SurfaceCullMode::back;
        } else {
            throw std::runtime_error(context + " has unknown cull '" + name + "'");
        }
    }

    if (const auto *field = findField(fields, "depth")) {
        const auto name = parseStringToken(*field, context + " depth");
        if (name == "read_write") {
            state.depth_test = true;
            state.depth_write = true;
        } else if (name == "read_only") {
            state.depth_test = true;
            state.depth_write = false;
        } else if (name == "disabled") {
            state.depth_test = false;
            state.depth_write = false;
        } else {
            throw std::runtime_error(context + " has unknown depth mode '" + name + "'");
        }
    }
    if (const auto *field = findField(fields, "depth_test")) {
        state.depth_test = parseBoolToken(*field, context + " depth_test");
    }
    if (const auto *field = findField(fields, "depth_write")) {
        state.depth_write = parseBoolToken(*field, context + " depth_write");
    }
    if (const auto *field = findField(fields, "depth_compare")) {
        const auto name = parseStringToken(*field, context + " depth_compare");
        if (name == "never") state.depth_compare = SurfaceDepthCompare::never;
        else if (name == "less") state.depth_compare = SurfaceDepthCompare::less;
        else if (name == "equal") state.depth_compare = SurfaceDepthCompare::equal;
        else if (name == "less_equal") state.depth_compare = SurfaceDepthCompare::less_equal;
        else if (name == "greater") state.depth_compare = SurfaceDepthCompare::greater;
        else if (name == "not_equal") state.depth_compare = SurfaceDepthCompare::not_equal;
        else if (name == "greater_equal") state.depth_compare = SurfaceDepthCompare::greater_equal;
        else if (name == "always") state.depth_compare = SurfaceDepthCompare::always;
        else throw std::runtime_error(context + " has unknown depth_compare '" + name + "'");
    }
    if (!state.depth_test && state.depth_write) {
        throw std::runtime_error(context +
                                 " requests depth_write while depth_test is disabled");
    }
    return state;
}

void appendScreenInput(SurfaceFormatDocument &document, std::string input,
                       std::string_view source_name) {
    const auto context = surfaceContext(source_name) + " screen_input '" + input + "'";
    if (!isIdentifier(input)) {
        throw std::runtime_error(context + " must be an identifier");
    }
    if (std::find(document.screen_inputs.begin(), document.screen_inputs.end(), input) !=
        document.screen_inputs.end()) {
        throw std::runtime_error(context + " is duplicated");
    }
    document.screen_inputs.push_back(std::move(input));
}

void validateUniqueResourceNames(const SurfaceFormatDocument &document,
                                 std::string_view source_name) {
    std::unordered_set<std::string> names;
    for (const auto &param : document.params) {
        if (!names.insert(param.name).second) {
            throw std::runtime_error(surfaceContext(source_name) + " has duplicate name '" +
                                     param.name + "'");
        }
    }
    for (const auto &texture : document.textures) {
        if (!names.insert(texture.name).second) {
            throw std::runtime_error(surfaceContext(source_name) + " has duplicate name '" +
                                     texture.name + "'");
        }
    }
}

} // namespace

std::string_view surfaceParamTypeName(SurfaceParamType type) {
    switch (type) {
    case SurfaceParamType::floating:
        return "float";
    case SurfaceParamType::vec2:
        return "vec2";
    case SurfaceParamType::vec3:
        return "vec3";
    case SurfaceParamType::vec4:
        return "vec4";
    case SurfaceParamType::integer:
        return "int";
    case SurfaceParamType::color:
        return "color";
    }
    return "unknown";
}

SurfaceFormatDocument parseSurfaceFormat(std::string_view source, std::string_view source_name) {
    const auto context = surfaceContext(source_name);
    if (source.empty()) {
        throw std::runtime_error(context + " is empty; expected '//! pelican.surface v1'");
    }

    auto first_line = readLine(source, 0, 1);
    if (first_line.text != surface_magic) {
        throw std::runtime_error(context + " must start with '//! pelican.surface v1'");
    }

    SurfaceFormatDocument document;
    std::size_t offset = first_line.next_offset;
    std::size_t line_number = 2;
    HeaderSection section = HeaderSection::none;
    bool has_language = false;
    std::unordered_set<std::string> known_top_level_fields;

    while (offset < source.size()) {
        const auto line = readLine(source, offset, line_number);
        if (!startsWith(line.text, "//!")) {
            break;
        }
        if (line.text.size() > 3 && line.text[3] != ' ') {
            throw std::runtime_error(lineContext(source_name, line.number) +
                                     " must use the '//! ' header prefix");
        }

        const auto content = trim(line.text.size() <= 3 ? std::string_view{}
                                                        : line.text.substr(4));
        offset = line.next_offset;
        ++line_number;
        if (content.empty()) {
            continue;
        }

        if (startsWith(content, "-")) {
            const auto item = trim(std::string_view{content}.substr(1));
            if (section == HeaderSection::params) {
                document.params.push_back(
                    parseParamDefinition(item, source_name, line.number, document.warnings));
            } else if (section == HeaderSection::textures) {
                document.textures.push_back(
                    parseTextureDefinition(item, source_name, line.number, document.warnings));
            } else if (section == HeaderSection::screen_inputs) {
                appendScreenInput(document, parseStringToken(item, lineContext(source_name, line.number)),
                                  source_name);
            } else if (section != HeaderSection::unknown) {
                throw std::runtime_error(lineContext(source_name, line.number) +
                                         " has a list item outside a known section");
            }
            continue;
        }

        const auto colon = content.find(':');
        if (colon == std::string::npos) {
            throw std::runtime_error(lineContext(source_name, line.number) +
                                     " requires a 'key: value' header field");
        }
        const auto key = trim(std::string_view{content}.substr(0, colon));
        const auto value = trim(std::string_view{content}.substr(colon + 1));
        if (key.empty()) {
            throw std::runtime_error(lineContext(source_name, line.number) + " has an empty key");
        }

        const bool known_key = key == "language" || key == "params" || key == "textures" ||
                               key == "screen_inputs" || key == "render_state";
        if (known_key && !known_top_level_fields.insert(key).second) {
            throw std::runtime_error(context + " has duplicate header key '" + key + "'");
        }

        section = HeaderSection::none;
        if (key == "language") {
            if (value.empty()) {
                throw std::runtime_error(context + " language requires a value");
            }
            document.language = parseLanguage(value, context);
            has_language = true;
        } else if (key == "params" || key == "textures") {
            if (!value.empty() && value != "[]") {
                throw std::runtime_error(context + " " + key +
                                         " must be [] or a block list of inline mappings");
            }
            if (value.empty()) {
                section = key == "params" ? HeaderSection::params : HeaderSection::textures;
            }
        } else if (key == "screen_inputs") {
            if (value.empty()) {
                section = HeaderSection::screen_inputs;
            } else {
                for (auto input : parseStringArray(value, context + " screen_inputs")) {
                    appendScreenInput(document, std::move(input), source_name);
                }
            }
        } else if (key == "render_state") {
            if (value.empty()) {
                throw std::runtime_error(context + " render_state requires an inline mapping");
            }
            document.render_state = parseRenderState(value, source_name, document.warnings);
        } else {
            document.warnings.push_back(context + " ignored unknown header key '" + key + "'");
            if (value.empty()) {
                section = HeaderSection::unknown;
            }
        }
    }

    if (!has_language) {
        throw std::runtime_error(context + " requires language");
    }
    validateUniqueResourceNames(document, source_name);
    document.code_offset = offset;
    document.code = std::string{source.substr(offset)};
    return document;
}

} // namespace Pelican
