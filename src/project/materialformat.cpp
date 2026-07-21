#include "materialformat.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <sstream>
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

const SurfaceTextureDefinition *findSurfaceTexture(const SurfaceFormatDocument &surface,
                                                   std::string_view name) {
    const auto found = std::find_if(surface.textures.begin(), surface.textures.end(),
                                    [name](const auto &texture) {
                                        return texture.name == name;
                                    });
    return found == surface.textures.end() ? nullptr : &*found;
}

MaterialAlphaMode parseAlphaMode(std::string_view value, std::string_view context) {
    if (value == "opaque") return MaterialAlphaMode::opaque;
    if (value == "mask") return MaterialAlphaMode::mask;
    if (value == "blend") return MaterialAlphaMode::blend;
    throw std::runtime_error(std::string{context} +
                             " alpha_mode must be opaque, mask, or blend: " +
                             std::string{value});
}

MaterialVariantRouting parseRoutingObject(const nlohmann::json &routing,
                                          std::string_view context,
                                          std::vector<std::string> *warnings = nullptr) {
    if (!routing.is_object()) {
        throw std::runtime_error(std::string{context} + " routing must be an object");
    }
    if (warnings != nullptr) {
        appendUnknownKeyWarnings(routing, {"alpha_mode", "double_sided"},
                                 std::string{context} + " routing", *warnings);
    } else {
        for (auto it = routing.begin(); it != routing.end(); ++it) {
            if (!isKnownKey(it.key(), {"alpha_mode", "double_sided"})) {
                throw std::runtime_error(std::string{context} +
                                         " routing has unknown key '" + it.key() + "'");
            }
        }
    }

    const auto &alpha = requireMember(routing, "alpha_mode",
                                      std::string{context} + " routing");
    if (!alpha.is_string()) {
        throw std::runtime_error(std::string{context} +
                                 " routing alpha_mode must be a string");
    }
    const auto &double_sided = requireMember(routing, "double_sided",
                                             std::string{context} + " routing");
    if (!double_sided.is_boolean()) {
        throw std::runtime_error(std::string{context} +
                                 " routing double_sided must be boolean");
    }
    return MaterialVariantRouting{
        parseAlphaMode(alpha.get<std::string>(), std::string{context} + " routing"),
        double_sided.get<bool>(),
    };
}

std::optional<MaterialVariantRouting>
parseMaterialRouting(const nlohmann::json &material, const std::string &name,
                     std::vector<std::string> &warnings) {
    const auto found = material.find("routing");
    if (found == material.end()) return std::nullopt;
    return parseRoutingObject(*found, materialContext(name), &warnings);
}

MaterialRenderPath parseMaterialRenderPath(const nlohmann::json &material,
                                           const std::string &name) {
    const auto found = material.find("render_path");
    if (found == material.end()) return MaterialRenderPath::automatic;
    if (!found->is_string()) {
        throw std::runtime_error(materialContext(name) +
                                 " render_path must be auto, deferred, or forward");
    }
    const auto value = found->get<std::string>();
    if (value == "auto") return MaterialRenderPath::automatic;
    if (value == "deferred") return MaterialRenderPath::deferred;
    if (value == "forward") return MaterialRenderPath::forward;
    throw std::runtime_error(materialContext(name) +
                             " render_path must be auto, deferred, or forward: " + value);
}

std::optional<std::string> parseExactMaterialPass(const nlohmann::json &material,
                                                  const std::string &name) {
    const auto pass = optionalString(material, "pass", materialContext(name));
    if (!pass) return std::nullopt;
    if (!isR7Identifier(*pass)) {
        throw std::runtime_error(materialContext(name) +
                                 " pass must match [a-zA-Z0-9_]: " + *pass);
    }
    return pass;
}

std::vector<MaterialTextureOverride>
parseTextureOverrides(const nlohmann::json &material, const std::string &name,
                      const std::optional<std::string> &surface_reference,
                      const MaterialSurfaceCatalog &surfaces) {
    std::vector<MaterialTextureOverride> overrides;
    const auto found = material.find("textures");
    if (found == material.end()) return overrides;

    const auto context = materialContext(name);
    if (!found->is_object()) {
        throw std::runtime_error(context +
                                 " textures must be an object of declared-name to string reference overrides");
    }
    if (!surface_reference) {
        throw std::runtime_error(context +
                                 " textures overrides require a named .surface reference");
    }
    if (found->empty()) return overrides;

    const auto surface = surfaces.find(*surface_reference);
    if (surface == surfaces.end()) {
        throw std::runtime_error(context + " cannot validate textures because surface '" +
                                 *surface_reference + "' was not provided");
    }

    overrides.reserve(found->size());
    for (auto value = found->begin(); value != found->end(); ++value) {
        validateGlslIdentifier(value.key(), context, "texture override");
        if (!value->is_string()) {
            throw std::runtime_error(context + " texture override '" + value.key() +
                                     "' must be a string reference matching its declared texture type");
        }
        if (findSurfaceTexture(surface->second, value.key()) == nullptr) {
            throw std::runtime_error(context + " texture override '" + value.key() +
                                     "' is not declared by surface '" +
                                     *surface_reference + "'");
        }
        auto reference = value->get<std::string>();
        validateTextureReference(reference, context + " texture override '" + value.key() + "'",
                                 "reference");
        overrides.push_back({value.key(), std::move(reference)});
    }
    return overrides;
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
    appendUnknownKeyWarnings(material,
                             {"name", "base", "shader", "defines", "surface", "values",
                              "textures", "routing", "render_path", "pass"},
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
    parsed.routing = parseMaterialRouting(material, name, warnings);
    parsed.render_path = parseMaterialRenderPath(material, name);
    parsed.exact_pass = parseExactMaterialPass(material, name);
    parsed.values = parseValues(material, name, parsed.surface, surfaces);
    parsed.texture_overrides =
        parseTextureOverrides(material, name, parsed.surface, surfaces);
    return parsed;
}

std::uint32_t requireBindingIndex(const nlohmann::json &binding, std::string_view key,
                                  std::string_view context) {
    const auto &value = requireMember(binding, key, context);
    if (!value.is_number_unsigned() && !value.is_number_integer()) {
        throw std::runtime_error(std::string{context} + " requires non-negative integer " +
                                 std::string{key});
    }
    try {
        const auto parsed = value.get<std::int64_t>();
        if (parsed < 0 || static_cast<std::uint64_t>(parsed) >
                              std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(std::string{context} + " " + std::string{key} +
                                     " is outside uint32 range");
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const nlohmann::json::exception &) {
        throw std::runtime_error(std::string{context} + " " + std::string{key} +
                                 " is outside uint32 range");
    }
}

void requireOnlyKeys(const nlohmann::json &object,
                     std::initializer_list<std::string_view> known_keys,
                     std::string_view context) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!isKnownKey(it.key(), known_keys)) {
            throw std::runtime_error(std::string{context} + " has unknown key '" +
                                     it.key() + "'");
        }
    }
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

PrimitiveMaterialBindingDocument
parsePrimitiveMaterialBindingJson(const nlohmann::json &document_json) {
    constexpr std::string_view schema = "pelican.material_bindings";
    constexpr int version = 1;
    if (!document_json.is_object()) {
        throw std::runtime_error("material binding document must be an object");
    }
    requireOnlyKeys(document_json, {"schema", "version", "model", "bindings"},
                    "material binding document");
    if (document_json.value("schema", std::string{}) != schema) {
        throw std::runtime_error("material binding document schema must be '" +
                                 std::string{schema} + "'");
    }
    if (!document_json.contains("version") ||
        !document_json.at("version").is_number_integer() ||
        document_json.at("version").get<int>() != version) {
        throw std::runtime_error("material binding document version must be exactly 1");
    }

    PrimitiveMaterialBindingDocument document;
    document.model = requireString(document_json, "model", "material binding document");
    if (document.model.empty() || containsBackslash(document.model) ||
        document.model.find('#') != std::string::npos) {
        throw std::runtime_error("material binding model '" + document.model +
                                 "' must be a whole GLB/glTF reference without a fragment");
    }
    const auto model_path = std::string_view{document.model};
    if (!endsWith(model_path, ".glb") && !endsWith(model_path, ".gltf") &&
        !endsWith(model_path, ".vrm")) {
        throw std::runtime_error("material binding model '" + document.model +
                                 "' must end in .glb, .gltf, or .vrm");
    }

    const auto &bindings = requireMember(document_json, "bindings",
                                         "material binding document");
    if (!bindings.is_array() || bindings.empty()) {
        throw std::runtime_error("material binding document requires a non-empty bindings array");
    }

    std::unordered_set<std::string> usd_paths;
    std::unordered_map<std::uint64_t, std::string> targets;
    document.bindings.reserve(bindings.size());
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto &binding = bindings.at(index);
        const auto index_context = "material binding bindings[" + std::to_string(index) + "]";
        if (!binding.is_object()) {
            throw std::runtime_error(index_context + " must be an object");
        }
        requireOnlyKeys(binding,
                        {"usd_path", "mesh", "primitive", "material", "routing"},
                        index_context);
        auto usd_path = requireString(binding, "usd_path", index_context);
        const auto context = index_context + " USD path '" + usd_path + "'";
        if (usd_path.empty() || usd_path.front() != '/' || containsBackslash(usd_path) ||
            usd_path.find('#') != std::string::npos) {
            throw std::runtime_error(context +
                                     " must be an absolute USD prim/subset path without a fragment");
        }
        if (!usd_paths.insert(usd_path).second) {
            throw std::runtime_error("duplicate material binding USD path '" + usd_path + "'");
        }
        const auto mesh = requireBindingIndex(binding, "mesh", context);
        const auto primitive = requireBindingIndex(binding, "primitive", context);
        const auto target = (static_cast<std::uint64_t>(mesh) << 32u) | primitive;
        if (const auto existing = targets.find(target); existing != targets.end()) {
            throw std::runtime_error("material binding collision at GLB mesh " +
                                     std::to_string(mesh) + " primitive " +
                                     std::to_string(primitive) + " between USD paths '" +
                                     existing->second + "' and '" + usd_path + "'");
        }
        targets.emplace(target, usd_path);

        auto material = requireString(binding, "material", context);
        try {
            validateMaterialName(material);
        } catch (const std::exception &error) {
            throw std::runtime_error(context + " material '" + material + "': " +
                                     error.what());
        }
        const auto &routing = requireMember(binding, "routing", context);
        document.bindings.push_back(PrimitiveMaterialBinding{
            std::move(usd_path), mesh, primitive, std::move(material),
            parseRoutingObject(routing, context),
        });
    }
    return document;
}

std::string_view materialAlphaModeName(MaterialAlphaMode mode) {
    switch (mode) {
    case MaterialAlphaMode::opaque: return "opaque";
    case MaterialAlphaMode::mask: return "mask";
    case MaterialAlphaMode::blend: return "blend";
    }
    return "unknown";
}

std::string_view materialRenderPathName(MaterialRenderPath path) {
    switch (path) {
    case MaterialRenderPath::automatic: return "auto";
    case MaterialRenderPath::deferred: return "deferred";
    case MaterialRenderPath::forward: return "forward";
    }
    return "unknown";
}

std::string_view materialVariantName(const MaterialVariantRouting &routing) {
    if (routing.double_sided) {
        switch (routing.alpha_mode) {
        case MaterialAlphaMode::opaque: return "opaque_double_sided";
        case MaterialAlphaMode::mask: return "mask_double_sided";
        case MaterialAlphaMode::blend: return "blend_double_sided";
        }
    } else {
        switch (routing.alpha_mode) {
        case MaterialAlphaMode::opaque: return "opaque_single_sided";
        case MaterialAlphaMode::mask: return "mask_single_sided";
        case MaterialAlphaMode::blend: return "blend_single_sided";
        }
    }
    return "unknown";
}

SurfaceRenderState materialVariantRenderState(const MaterialVariantRouting &routing) {
    SurfaceRenderState state;
    state.blend = routing.alpha_mode == MaterialAlphaMode::blend
                      ? SurfaceBlendMode::blend
                      : SurfaceBlendMode::opaque;
    state.cull = routing.double_sided ? SurfaceCullMode::none : SurfaceCullMode::back;
    state.depth_test = true;
    state.depth_write = routing.alpha_mode != MaterialAlphaMode::blend;
    state.depth_compare = SurfaceDepthCompare::less;
    return state;
}

bool materialVariantKeepsFace(const MaterialVariantRouting &routing,
                              bool front_facing) {
    return front_facing || routing.double_sided;
}

bool materialMaskKeepsFragment(double alpha, double alpha_cutoff) {
    return std::isfinite(alpha) && std::isfinite(alpha_cutoff) && alpha >= alpha_cutoff;
}

std::string dumpPrimitiveMaterialBindings(
    const PrimitiveMaterialBindingDocument &document) {
    auto bindings = document.bindings;
    std::sort(bindings.begin(), bindings.end(), [](const auto &left, const auto &right) {
        if (left.mesh_index != right.mesh_index) return left.mesh_index < right.mesh_index;
        if (left.primitive_index != right.primitive_index)
            return left.primitive_index < right.primitive_index;
        return left.usd_path < right.usd_path;
    });
    std::ostringstream out;
    out << "schema: pelican.material_bindings v1\n";
    out << "model: " << document.model << '\n';
    out << "bindings:\n";
    for (const auto &binding : bindings) {
        const auto state = materialVariantRenderState(binding.routing);
        out << "  " << binding.usd_path << " -> mesh=" << binding.mesh_index
            << " primitive=" << binding.primitive_index
            << " material=" << binding.material
            << " variant=" << materialVariantName(binding.routing)
            << " alpha=" << materialAlphaModeName(binding.routing.alpha_mode)
            << " cull=" << (state.cull == SurfaceCullMode::back ? "back" : "none")
            << " front=true back="
            << (materialVariantKeepsFace(binding.routing, false) ? "true" : "false")
            << " blend=" << (state.blend == SurfaceBlendMode::blend ? "blend" : "opaque")
            << " depth_test=true depth_write="
            << (state.depth_write ? "true" : "false");
        if (binding.routing.alpha_mode == MaterialAlphaMode::mask) {
            out << " discard=alpha<alpha_cutoff";
        }
        out << '\n';
    }
    return out.str();
}

} // namespace Pelican
