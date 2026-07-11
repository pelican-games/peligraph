#include "materialformat.hpp"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Pelican {

namespace {

constexpr std::string_view material_schema = "pelican.material";
constexpr int supported_material_version = 1;

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool hasScheme(std::string_view value) {
    return value.find("://") != std::string_view::npos;
}

bool containsBackslash(std::string_view value) {
    return value.find('\\') != std::string_view::npos;
}

bool isR7Identifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_lower = ch >= 'a' && ch <= 'z';
        if (!is_digit && !is_upper && !is_lower && ch != '_') {
            return false;
        }
    }
    return true;
}

bool isGlslIdentifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    const char first = value.front();
    const bool valid_first = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_';
    if (!valid_first) {
        return false;
    }
    for (const char ch : value.substr(1)) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_lower = ch >= 'a' && ch <= 'z';
        if (!is_digit && !is_upper && !is_lower && ch != '_') {
            return false;
        }
    }
    return true;
}

bool isKnownKey(std::string_view key, std::initializer_list<std::string_view> known_keys) {
    return std::find(known_keys.begin(), known_keys.end(), key) != known_keys.end();
}

void appendUnknownKeyWarnings(const nlohmann::json &object,
                              std::initializer_list<std::string_view> known_keys,
                              std::string_view context,
                              std::vector<std::string> &warnings) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!isKnownKey(it.key(), known_keys)) {
            warnings.push_back(std::string{context} + " ignored unknown key '" + it.key() + "'");
        }
    }
}

std::string materialContext(const std::string &name) {
    return "material '" + name + "'";
}

const nlohmann::json &requireMember(const nlohmann::json &object, std::string_view key,
                                    std::string_view context) {
    const auto it = object.find(key);
    if (it == object.end()) {
        throw std::runtime_error(std::string{context} + " requires " + std::string{key});
    }
    return it.value();
}

std::string requireString(const nlohmann::json &object, std::string_view key,
                          std::string_view context) {
    const auto &value = requireMember(object, key, context);
    if (!value.is_string()) {
        throw std::runtime_error(std::string{context} + " requires string " + std::string{key});
    }
    return value.get<std::string>();
}

std::optional<std::string> optionalString(const nlohmann::json &object, std::string_view key,
                                          std::string_view context) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        throw std::runtime_error(std::string{context} + " optional " + std::string{key} +
                                 " must be a string");
    }
    return it->get<std::string>();
}

double requireNumber(const nlohmann::json &object, std::string_view key, std::string_view context) {
    const auto &value = requireMember(object, key, context);
    if (!value.is_number()) {
        throw std::runtime_error(std::string{context} + " requires numeric " + std::string{key});
    }
    return value.get<double>();
}

template <size_t N>
std::array<double, N> requireNumberArray(const nlohmann::json &object, std::string_view key,
                                         std::string_view context) {
    const auto &value = requireMember(object, key, context);
    if (!value.is_array() || value.size() != N) {
        throw std::runtime_error(std::string{context} + " requires numeric vec" + std::to_string(N) +
                                 " " + std::string{key});
    }

    std::array<double, N> parsed{};
    for (size_t i = 0; i < N; ++i) {
        if (!value.at(i).is_number()) {
            throw std::runtime_error(std::string{context} + " requires numeric vec" + std::to_string(N) +
                                     " " + std::string{key});
        }
        parsed[i] = value.at(i).get<double>();
    }
    return parsed;
}

void validateMaterialName(std::string_view name) {
    if (!isR7Identifier(name)) {
        throw std::runtime_error("material name must match [a-zA-Z0-9_]: " + std::string{name});
    }
}

void validateTextureReference(std::string_view ref, std::string_view context, std::string_view key) {
    if (ref.empty()) {
        throw std::runtime_error(std::string{context} + " " + std::string{key} + " must not be empty");
    }
    if (containsBackslash(ref)) {
        throw std::runtime_error(std::string{context} + " " + std::string{key} +
                                 " must use forward slashes: " + std::string{ref});
    }
    if (!startsWith(ref, "project://") && !startsWith(ref, "engine://")) {
        throw std::runtime_error(std::string{context} + " " + std::string{key} +
                                 " must be a project:// or engine:// reference: " + std::string{ref});
    }
}

void validateShaderStem(std::string_view ref, std::string_view context) {
    if (ref.empty()) {
        throw std::runtime_error(std::string{context} + " shader must not be empty");
    }
    if (containsBackslash(ref)) {
        throw std::runtime_error(std::string{context} + " shader must use forward slashes: " +
                                 std::string{ref});
    }
    if (hasScheme(ref) && !startsWith(ref, "project://") && !startsWith(ref, "engine://")) {
        throw std::runtime_error(std::string{context} +
                                 " shader must be a project://, engine://, or bare stem reference: " +
                                 std::string{ref});
    }

    const auto slash = ref.find_last_of('/');
    const auto filename = slash == std::string_view::npos ? ref : ref.substr(slash + 1);
    if (filename.empty() || filename.find('.') != std::string_view::npos) {
        throw std::runtime_error(std::string{context} + " shader must be an extensionless stem: " +
                                 std::string{ref});
    }
}

void validateSurfaceReference(std::string_view ref, std::string_view context) {
    if (ref.empty()) {
        throw std::runtime_error(std::string{context} + " surface must not be empty");
    }
    if (containsBackslash(ref)) {
        throw std::runtime_error(std::string{context} + " surface must use forward slashes: " +
                                 std::string{ref});
    }
    if (hasScheme(ref) && !startsWith(ref, "project://") && !startsWith(ref, "engine://")) {
        throw std::runtime_error(std::string{context} +
                                 " surface must be a project://, engine://, or bare .surface reference: " +
                                 std::string{ref});
    }
    const auto slash = ref.find_last_of('/');
    const auto filename = slash == std::string_view::npos ? ref : ref.substr(slash + 1);
    if (filename.size() <= std::string_view{".surface"}.size() || !endsWith(filename, ".surface")) {
        throw std::runtime_error(std::string{context} + " surface must end in .surface: " +
                                 std::string{ref});
    }
}

void validateGlslIdentifier(std::string_view value, std::string_view context, std::string_view kind) {
    if (!isGlslIdentifier(value)) {
        throw std::runtime_error(std::string{context} + " " + std::string{kind} +
                                 " must be a GLSL identifier: " + std::string{value});
    }
}

void validateEnvelope(const nlohmann::json &document, std::vector<std::string> &warnings) {
    if (!document.is_object()) {
        throw std::runtime_error("material document must be an object");
    }
    appendUnknownKeyWarnings(document, {"schema", "version", "materials"}, "material document", warnings);
    if (document.value("schema", std::string{}) != material_schema) {
        throw std::runtime_error("material document schema is not supported");
    }
    if (!document.contains("version") || !document.at("version").is_number_integer()) {
        throw std::runtime_error("material document requires numeric version");
    }
    if (document.at("version").get<int>() != supported_material_version) {
        throw std::runtime_error("material document version is not supported");
    }
    if (!document.contains("materials") || !document.at("materials").is_array()) {
        throw std::runtime_error("material document requires materials array");
    }
}

MaterialBase parseBase(const nlohmann::json &material, const std::string &name,
                       std::vector<std::string> &warnings) {
    MaterialBase base;
    const auto base_it = material.find("base");
    if (base_it == material.end()) {
        return base;
    }
    const auto context = materialContext(name) + " base";
    if (!base_it->is_object()) {
        throw std::runtime_error(context + " must be an object");
    }

    const auto &base_json = *base_it;
    appendUnknownKeyWarnings(base_json,
                             {"baseColorFactor",
                              "baseColorTexture",
                              "metallicFactor",
                              "roughnessFactor",
                              "metallicRoughnessTexture",
                              "normalTexture",
                              "occlusionTexture",
                              "emissiveFactor",
                              "emissiveTexture"},
                             context, warnings);

    if (base_json.contains("baseColorFactor")) {
        base.base_color_factor = requireNumberArray<4>(base_json, "baseColorFactor", context);
    }
    if (const auto ref = optionalString(base_json, "baseColorTexture", context)) {
        validateTextureReference(*ref, context, "baseColorTexture");
        base.base_color_texture = *ref;
    }
    if (base_json.contains("metallicFactor")) {
        base.metallic_factor = requireNumber(base_json, "metallicFactor", context);
    }
    if (base_json.contains("roughnessFactor")) {
        base.roughness_factor = requireNumber(base_json, "roughnessFactor", context);
    }
    if (const auto ref = optionalString(base_json, "metallicRoughnessTexture", context)) {
        validateTextureReference(*ref, context, "metallicRoughnessTexture");
        base.metallic_roughness_texture = *ref;
    }
    if (const auto ref = optionalString(base_json, "normalTexture", context)) {
        validateTextureReference(*ref, context, "normalTexture");
        base.normal_texture = *ref;
    }
    if (const auto ref = optionalString(base_json, "occlusionTexture", context)) {
        validateTextureReference(*ref, context, "occlusionTexture");
        base.occlusion_texture = *ref;
    }
    if (base_json.contains("emissiveFactor")) {
        base.emissive_factor = requireNumberArray<3>(base_json, "emissiveFactor", context);
    }
    if (const auto ref = optionalString(base_json, "emissiveTexture", context)) {
        validateTextureReference(*ref, context, "emissiveTexture");
        base.emissive_texture = *ref;
    }
    return base;
}

std::vector<std::string> parseDefines(const nlohmann::json &material, const std::string &name) {
    std::vector<std::string> defines;
    const auto it = material.find("defines");
    if (it == material.end()) {
        return defines;
    }
    const auto context = materialContext(name);
    if (!it->is_array()) {
        throw std::runtime_error(context + " defines must be an array of boolean flag names");
    }

    std::unordered_set<std::string> seen;
    defines.reserve(it->size());
    for (const auto &entry : *it) {
        if (!entry.is_string()) {
            throw std::runtime_error(context + " defines entries must be string boolean flag names");
        }
        auto define = entry.get<std::string>();
        validateGlslIdentifier(define, context, "define");
        if (!seen.insert(define).second) {
            throw std::runtime_error(context + " duplicate define: " + define);
        }
        defines.push_back(std::move(define));
    }
    return defines;
}

const SurfaceParamDefinition *findSurfaceParam(const SurfaceFormatDocument &surface,
                                               std::string_view name) {
    const auto it = std::find_if(surface.params.begin(), surface.params.end(), [name](const auto &param) {
        return param.name == name;
    });
    return it == surface.params.end() ? nullptr : &*it;
}

size_t vectorWidth(SurfaceParamType type) {
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

SurfaceParamValue parseOverrideValue(const nlohmann::json &value, SurfaceParamType type,
                                     std::string_view context) {
    const auto type_error = [&context, type]() {
        return std::runtime_error(std::string{context} + " must match declared type " +
                                  std::string{surfaceParamTypeName(type)});
    };

    SurfaceParamValue parsed;
    parsed.type = type;
    if (type == SurfaceParamType::floating) {
        if (!value.is_number()) {
            throw type_error();
        }
        parsed.values[0] = value.get<double>();
        parsed.component_count = 1;
        return parsed;
    }
    if (type == SurfaceParamType::integer) {
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            throw type_error();
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
            throw type_error();
        }
        parsed.values[3] = 1.0;
        parsed.component_count = static_cast<std::uint8_t>(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (!value.at(i).is_number()) {
                throw type_error();
            }
            parsed.values[i] = value.at(i).get<double>();
        }
        return parsed;
    }

    const auto width = vectorWidth(type);
    if (!value.is_array() || value.size() != width) {
        throw type_error();
    }
    for (size_t i = 0; i < width; ++i) {
        if (!value.at(i).is_number()) {
            throw type_error();
        }
        parsed.values[i] = value.at(i).get<double>();
    }
    parsed.component_count = static_cast<std::uint8_t>(width);
    return parsed;
}

std::vector<MaterialValue> parseValues(const nlohmann::json &material, const std::string &name,
                                       const std::optional<std::string> &surface_reference,
                                       const MaterialSurfaceCatalog &surfaces) {
    std::vector<MaterialValue> values;
    const auto it = material.find("values");
    if (it == material.end()) {
        return values;
    }

    const auto context = materialContext(name);
    if (!it->is_object()) {
        throw std::runtime_error(context + " values must be an object");
    }
    if (!surface_reference) {
        throw std::runtime_error(context + " values require a named .surface reference");
    }
    if (it->empty()) {
        return values;
    }

    const auto surface_it = surfaces.find(*surface_reference);
    if (surface_it == surfaces.end()) {
        throw std::runtime_error(context + " cannot validate values because surface '" +
                                 *surface_reference + "' was not provided");
    }

    values.reserve(it->size());
    for (auto value = it->begin(); value != it->end(); ++value) {
        validateGlslIdentifier(value.key(), context, "value");
        const auto *declaration = findSurfaceParam(surface_it->second, value.key());
        if (declaration == nullptr) {
            throw std::runtime_error(context + " value '" + value.key() +
                                     "' is not declared by surface '" + *surface_reference + "'");
        }
        const auto value_context = context + " value '" + value.key() + "' for surface '" +
                                   *surface_reference + "'";
        values.push_back(MaterialValue{
            value.key(),
            parseOverrideValue(value.value(), declaration->type, value_context),
        });
    }
    return values;
}

MaterialDefinition parseMaterial(const nlohmann::json &material, size_t index,
                                 const MaterialSurfaceCatalog &surfaces,
                                 std::vector<std::string> &warnings) {
    if (!material.is_object()) {
        throw std::runtime_error("materials[" + std::to_string(index) + "] must be an object");
    }

    const auto name = requireString(material, "name", "materials[" + std::to_string(index) + "]");
    validateMaterialName(name);
    const auto context = materialContext(name);
    if (material.contains("params")) {
        throw std::runtime_error(context +
                                 " field 'params' declarations are not supported in v1; move them to "
                                 ".surface and use 'values' for overrides");
    }
    if (material.contains("textures")) {
        throw std::runtime_error(context +
                                 " field 'textures' declarations are not supported in v1; move them to "
                                 ".surface");
    }
    appendUnknownKeyWarnings(material, {"name", "base", "shader", "defines", "surface", "values"},
                             context, warnings);

    MaterialDefinition parsed;
    parsed.name = name;
    parsed.base = parseBase(material, name, warnings);
    parsed.defines = parseDefines(material, name);
    if (const auto shader = optionalString(material, "shader", context)) {
        validateShaderStem(*shader, context);
        parsed.shader = *shader;
    }
    if (const auto surface = optionalString(material, "surface", context)) {
        validateSurfaceReference(*surface, context);
        parsed.surface = *surface;
    }
    parsed.values = parseValues(material, name, parsed.surface, surfaces);
    return parsed;
}

} // namespace

MaterialFormatDocument parseMaterialFormatJson(const nlohmann::json &document_json) {
    return parseMaterialFormatJson(document_json, {});
}

MaterialFormatDocument parseMaterialFormatJson(const nlohmann::json &document_json,
                                               const MaterialSurfaceCatalog &surfaces) {
    MaterialFormatDocument document;
    validateEnvelope(document_json, document.warnings);

    const auto &materials_json = document_json.at("materials");
    document.materials.reserve(materials_json.size());
    std::unordered_set<std::string> material_names;
    for (size_t i = 0; i < materials_json.size(); ++i) {
        auto material = parseMaterial(materials_json.at(i), i, surfaces, document.warnings);
        if (!material_names.insert(material.name).second) {
            throw std::runtime_error("duplicate material name: " + material.name);
        }
        document.materials.push_back(std::move(material));
    }
    return document;
}

} // namespace Pelican
