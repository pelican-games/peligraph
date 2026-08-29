#include "prefab.hpp"

#include <picosha2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(PrefabErrorCode code, std::string message,
                       PrefabErrorContext context = {}) {
    throw PrefabError{code, std::move(message), std::move(context)};
}

void requireObject(const Json &value, PrefabErrorCode code,
                   std::string_view what, PrefabErrorContext context = {}) {
    if (!value.is_object()) {
        fail(code, std::string{what} + " must be an object", std::move(context));
    }
}

void requireOnly(const Json &value, std::initializer_list<std::string_view> keys,
                 PrefabErrorCode code, std::string_view what,
                 PrefabErrorContext context = {}) {
    for (const auto &[key, unused] : value.items()) {
        (void)unused;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            context.json_pointer = "/" + key;
            fail(code, std::string{what} + " contains unknown field: " + key,
                 std::move(context));
        }
    }
}

std::string requiredString(const Json &object, std::string_view key,
                           PrefabErrorCode code, std::string_view what,
                           PrefabErrorContext context = {}) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string() || found->get_ref<const std::string &>().empty()) {
        context.json_pointer = "/" + std::string{key};
        fail(code, std::string{what} + " requires non-empty string " +
                       std::string{key},
             std::move(context));
    }
    return found->get<std::string>();
}

std::string pointerToken(std::string_view token) {
    std::string result;
    for (const auto ch : token) {
        if (ch == '~') result += "~0";
        else if (ch == '/') result += "~1";
        else result += ch;
    }
    return result;
}

bool taggedNode(const Json &value) {
    return value.is_object() && value.contains("$param");
}

bool isComponentIdentifier(std::string_view value) noexcept {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    });
}

void collectBindings(const Json &node, std::string path,
                     std::vector<PrefabTaggedBinding> &bindings,
                     const std::unordered_set<std::string> &parameters,
                     std::unordered_set<std::string> &used,
                     std::string_view prefab_name) {
    if (taggedNode(node)) {
        if (node.size() != 1 || !node.at("$param").is_string() ||
            node.at("$param").get_ref<const std::string &>().empty()) {
            fail(PrefabErrorCode::PrefabDocumentInvalid,
                 "tagged parameter node must contain only a non-empty $param string",
                 {.prefab = std::string{prefab_name}, .json_pointer = path});
        }
        const auto parameter = node.at("$param").get<std::string>();
        if (!parameters.contains(parameter)) {
            fail(PrefabErrorCode::PrefabParameterUnknown,
                 "prefab component references unknown parameter: " + parameter,
                 {.prefab = std::string{prefab_name},
                  .parameter = parameter,
                  .json_pointer = path});
        }
        used.insert(parameter);
        bindings.push_back({.json_pointer = std::move(path),
                            .parameter = parameter});
        return;
    }
    if (node.is_object()) {
        for (const auto &[key, value] : node.items()) {
            collectBindings(value, path + "/" + pointerToken(key), bindings,
                            parameters, used, prefab_name);
        }
    } else if (node.is_array()) {
        for (std::size_t index = 0; index < node.size(); ++index) {
            collectBindings(node[index], path + "/" + std::to_string(index),
                            bindings, parameters, used, prefab_name);
        }
    }
}

Schema::FieldRange parseRange(const Json &value, Schema::FieldType type,
                              std::string_view prefab_name,
                              std::string_view parameter_name) {
    if (!value.is_array() || value.size() != 2) {
        fail(PrefabErrorCode::PrefabDocumentInvalid,
             "parameter range must contain exactly two values",
             {.prefab = std::string{prefab_name},
              .parameter = std::string{parameter_name},
              .json_pointer = "/parameters/range"});
    }
    const auto &rule = Schema::typeRule(type);
    try {
        switch (rule.range_kind) {
        case Schema::RangeKind::Signed:
            if (!value[0].is_number_integer() || !value[1].is_number_integer()) throw std::runtime_error{"signed"};
            return Schema::SignedRange{value[0].get<std::int64_t>(), value[1].get<std::int64_t>()};
        case Schema::RangeKind::Unsigned:
            if (!value[0].is_number_unsigned() || !value[1].is_number_unsigned()) throw std::runtime_error{"unsigned"};
            return Schema::UnsignedRange{value[0].get<std::uint64_t>(), value[1].get<std::uint64_t>()};
        case Schema::RangeKind::Floating:
            if (!value[0].is_number() || !value[1].is_number()) throw std::runtime_error{"floating"};
            return Schema::FloatingRange{value[0].get<double>(), value[1].get<double>()};
        case Schema::RangeKind::None:
            break;
        }
    } catch (const std::exception &) {
        fail(PrefabErrorCode::PrefabDocumentInvalid,
             "parameter range does not match its numeric type",
             {.prefab = std::string{prefab_name},
              .parameter = std::string{parameter_name},
              .json_pointer = "/parameters/range"});
    }
    fail(PrefabErrorCode::PrefabDocumentInvalid,
         "parameter range is valid only for numeric types",
         {.prefab = std::string{prefab_name},
          .parameter = std::string{parameter_name},
          .json_pointer = "/parameters/range"});
}

nlohmann::ordered_json resolveParameterValue(
    const PrefabParameterDeclaration &parameter, const Json &value,
    PrefabErrorContext context) {
    context.parameter = parameter.name;
    try {
        switch (parameter.kind) {
        case PrefabParameterKind::Value:
            return Schema::resolveValue(parameter.value_schema, value,
                                        context.json_pointer.value_or("/prefab/parameters/" + parameter.name));
        case PrefabParameterKind::Asset:
        case PrefabParameterKind::Object:
            if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
                fail(PrefabErrorCode::PrefabParameterType,
                     "asset and object parameters require a non-empty string",
                     context);
            }
            return value;
        }
    } catch (const PrefabError &) {
        throw;
    } catch (const Schema::Error &error) {
        context.json_pointer = std::string{error.path()};
        fail(PrefabErrorCode::PrefabParameterType, error.what(), std::move(context));
    }
    fail(PrefabErrorCode::PrefabParameterType, "unsupported parameter kind",
         std::move(context));
}

void appendLe32(std::vector<unsigned char> &bytes, std::uint32_t value) {
    for (int shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void appendLp(std::vector<unsigned char> &bytes, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("prefab identity input exceeds u32 length");
    }
    appendLe32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

} // namespace

std::string_view prefabErrorCodeName(PrefabErrorCode code) noexcept {
    static constexpr std::array names{
        "prefab_not_found", "prefab_version_unsupported", "prefab_requires_scene_v2",
        "prefab_duplicate_name", "prefab_parameter_unknown", "prefab_parameter_duplicate",
        "prefab_parameter_type", "prefab_parameter_unused", "prefab_binding_not_bindable",
        "prefab_binding_inactive", "prefab_object_ref_unresolved",
        "prefab_object_ref_missing_component", "prefab_transform_forbidden",
        "prefab_dependency_mismatch", "prefab_nested_unsupported", "prefab_provider_stale",
        "prefab_parameter_required", "prefab_instance_id_collision",
        "prefab_generated_read_only", "prefab_generated_id_collision",
        "prefab_instance_transform_missing", "prefab_instance_transform_duplicate",
        "prefab_component_duplicate", "prefab_name_mismatch",
        "prefab_persistence_unsupported", "prefab_path_invalid", "prefab_registry_invalid",
        "prefab_document_invalid", "prefab_instance_invalid", "prefab_instance_id_invalid",
        "prefab_component_key_duplicate"};
    const auto index = static_cast<std::size_t>(code);
    return index < names.size() ? names[index] : "prefab_unknown";
}

PrefabError::PrefabError(PrefabErrorCode code, std::string message,
                         PrefabErrorContext context)
    : std::runtime_error{std::move(message)}, code_{code},
      context_{std::move(context)} {}

bool isPrefabIdentifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    });
}

std::vector<PrefabRegistryEntry>
parsePrefabRegistryEntries(const Json &value) {
    if (!value.is_array()) {
        fail(PrefabErrorCode::PrefabRegistryInvalid,
             "project prefabs must be an array");
    }
    std::vector<PrefabRegistryEntry> result;
    std::unordered_set<std::string> names;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto &entry = value[index];
        PrefabErrorContext context{.line_index = index};
        requireObject(entry, PrefabErrorCode::PrefabRegistryInvalid,
                      "prefab registry entry", context);
        requireOnly(entry, {"name", "path"},
                    PrefabErrorCode::PrefabRegistryInvalid,
                    "prefab registry entry", context);
        auto name = requiredString(entry, "name",
                                   PrefabErrorCode::PrefabRegistryInvalid,
                                   "prefab registry entry", context);
        if (!isPrefabIdentifier(name)) {
            context.prefab = name;
            context.json_pointer = "/prefabs/" + std::to_string(index) + "/name";
            fail(PrefabErrorCode::PrefabRegistryInvalid,
                 "prefab registry name has invalid syntax", context);
        }
        if (!names.insert(name).second) {
            context.prefab = name;
            fail(PrefabErrorCode::PrefabDuplicateName,
                 "duplicate prefab registry name: " + name, context);
        }
        auto path = requiredString(entry, "path",
                                   PrefabErrorCode::PrefabRegistryInvalid,
                                   "prefab registry entry", context);
        const bool drive = path.size() >= 2 &&
                           ((path[0] >= 'A' && path[0] <= 'Z') ||
                            (path[0] >= 'a' && path[0] <= 'z')) &&
                           path[1] == ':';
        bool dotdot = false;
        std::size_t begin = 0;
        while (begin <= path.size()) {
            const auto end = path.find('/', begin);
            const auto part = path.substr(begin, end == std::string::npos ? end : end - begin);
            if (part == "..") dotdot = true;
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (path.starts_with('/') || path.starts_with('\\') || drive || dotdot ||
            path.find('\\') != std::string::npos) {
            context.prefab = name;
            context.json_pointer = "/prefabs/" + std::to_string(index) + "/path";
            fail(PrefabErrorCode::PrefabPathInvalid,
                 "prefab path must be a project-relative /-separated path", context);
        }
        result.push_back({std::move(name), std::move(path)});
    }
    return result;
}

PrefabDocument parsePrefabDocumentJson(const Json &value,
                                       std::string_view expected_name) {
    requireObject(value, PrefabErrorCode::PrefabDocumentInvalid,
                  "prefab document");
    requireOnly(value, {"schema", "version", "name", "parameters", "components"},
                PrefabErrorCode::PrefabDocumentInvalid, "prefab document");
    if (!value.contains("schema") || !value.at("schema").is_string() ||
        value.at("schema") != "pelican.prefab") {
        fail(PrefabErrorCode::PrefabDocumentInvalid,
             "prefab schema must be pelican.prefab",
             {.json_pointer = "/schema"});
    }
    if (!value.contains("version") || !value.at("version").is_number_integer()) {
        fail(PrefabErrorCode::PrefabDocumentInvalid,
             "prefab version must be an integer", {.json_pointer = "/version"});
    }
    if (value.at("version").get<int>() != 1) {
        fail(PrefabErrorCode::PrefabVersionUnsupported,
             "prefab version must be exactly 1", {.json_pointer = "/version"});
    }
    PrefabDocument result;
    result.name = requiredString(value, "name",
                                 PrefabErrorCode::PrefabDocumentInvalid,
                                 "prefab document");
    if (!isPrefabIdentifier(result.name)) {
        fail(PrefabErrorCode::PrefabDocumentInvalid,
             "prefab document name has invalid syntax",
             {.prefab = result.name, .json_pointer = "/name"});
    }
    if (!expected_name.empty() && result.name != expected_name) {
        fail(PrefabErrorCode::PrefabNameMismatch,
             "prefab document name does not match registry name",
             {.prefab = std::string{expected_name}, .json_pointer = "/name"});
    }

    const auto &parameters = value.value("parameters", Json::array());
    if (!parameters.is_array()) {
        fail(PrefabErrorCode::PrefabDocumentInvalid,
             "prefab parameters must be an array",
             {.prefab = result.name, .json_pointer = "/parameters"});
    }
    std::unordered_set<std::string> parameter_names;
    result.parameters.reserve(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto &record = parameters[index];
        const auto base = "/parameters/" + std::to_string(index);
        requireObject(record, PrefabErrorCode::PrefabDocumentInvalid,
                      "prefab parameter", {.prefab = result.name,
                                           .json_pointer = base});
        const auto has_type = record.contains("type");
        const auto has_kind = record.contains("kind");
        if (has_type == has_kind) {
            fail(PrefabErrorCode::PrefabDocumentInvalid,
                 "prefab parameter requires exactly one of type and kind",
                 {.prefab = result.name, .json_pointer = base});
        }
        PrefabParameterDeclaration declaration;
        declaration.name = requiredString(record, "name",
                                          PrefabErrorCode::PrefabDocumentInvalid,
                                          "prefab parameter",
                                          {.prefab = result.name,
                                           .json_pointer = base + "/name"});
        if (!isPrefabIdentifier(declaration.name)) {
            fail(PrefabErrorCode::PrefabDocumentInvalid,
                 "prefab parameter name has invalid syntax",
                 {.prefab = result.name, .parameter = declaration.name,
                  .json_pointer = base + "/name"});
        }
        if (!parameter_names.insert(declaration.name).second) {
            fail(PrefabErrorCode::PrefabParameterDuplicate,
                 "duplicate prefab parameter: " + declaration.name,
                 {.prefab = result.name, .parameter = declaration.name,
                  .json_pointer = base + "/name"});
        }
        if (has_type) {
            requireOnly(record,
                        {"name", "type", "default", "range", "enum_values"},
                        PrefabErrorCode::PrefabDocumentInvalid,
                        "value parameter", {.prefab = result.name,
                                            .parameter = declaration.name});
            declaration.kind = PrefabParameterKind::Value;
            const auto type_name = requiredString(record, "type",
                                                  PrefabErrorCode::PrefabDocumentInvalid,
                                                  "value parameter");
            try {
                declaration.value_schema.name = declaration.name;
                declaration.value_schema.type = Schema::fieldTypeFromName(type_name, base + "/type");
                const auto enum_values = record.find("enum_values");
                if (declaration.value_schema.type == Schema::FieldType::Enum) {
                    if (enum_values == record.end() || !enum_values->is_array() ||
                        enum_values->empty()) {
                        fail(PrefabErrorCode::PrefabDocumentInvalid,
                             "enum parameter requires a non-empty enum_values array",
                             {.prefab = result.name,
                              .parameter = declaration.name,
                              .json_pointer = base + "/enum_values"});
                    }
                    declaration.value_schema.enum_values.reserve(
                        enum_values->size());
                    for (std::size_t enum_index = 0;
                         enum_index < enum_values->size(); ++enum_index) {
                        const auto &entry = (*enum_values)[enum_index];
                        if (!entry.is_string()) {
                            fail(PrefabErrorCode::PrefabDocumentInvalid,
                                 "enum_values must contain only strings",
                                 {.prefab = result.name,
                                  .parameter = declaration.name,
                                  .json_pointer =
                                      base + "/enum_values/" +
                                      std::to_string(enum_index)});
                        }
                        declaration.value_schema.enum_values.push_back(
                            entry.get<std::string>());
                    }
                } else if (enum_values != record.end()) {
                    fail(PrefabErrorCode::PrefabDocumentInvalid,
                         "enum_values is valid only for enum parameters",
                         {.prefab = result.name,
                          .parameter = declaration.name,
                          .json_pointer = base + "/enum_values"});
                }
                if (const auto range = record.find("range"); range != record.end()) {
                    declaration.value_schema.range = parseRange(*range, declaration.value_schema.type,
                                                                result.name, declaration.name);
                }
                if (const auto def = record.find("default"); def != record.end()) {
                    declaration.value_schema.default_value = *def;
                    declaration.default_value = *def;
                }
                Schema::validateDeclaration(declaration.value_schema, base);
            } catch (const Schema::Error &error) {
                const auto invalid_enum_default =
                    declaration.value_schema.type == Schema::FieldType::Enum &&
                    record.contains("default") &&
                    (error.code() == Schema::ErrorCode::InvalidEnum ||
                     error.code() == Schema::ErrorCode::TypeMismatch);
                const auto code = invalid_enum_default
                        ? PrefabErrorCode::PrefabParameterType
                        : PrefabErrorCode::PrefabDocumentInvalid;
                fail(code, error.what(),
                     {.prefab = result.name, .parameter = declaration.name,
                      .json_pointer = std::string{error.path()}});
            }
        } else {
            const auto kind = requiredString(record, "kind",
                                             PrefabErrorCode::PrefabDocumentInvalid,
                                             "prefab parameter");
            if (kind == "asset") {
                requireOnly(record, {"name", "kind", "asset_kind", "default"},
                            PrefabErrorCode::PrefabDocumentInvalid,
                            "asset parameter");
                declaration.kind = PrefabParameterKind::Asset;
                declaration.asset_kind = requiredString(record, "asset_kind",
                                                        PrefabErrorCode::PrefabDocumentInvalid,
                                                        "asset parameter");
                if (const auto def = record.find("default"); def != record.end()) {
                    if (!def->is_string() || def->get_ref<const std::string &>().empty()) {
                        fail(PrefabErrorCode::PrefabDocumentInvalid,
                             "asset parameter default must be a non-empty string",
                             {.prefab = result.name, .parameter = declaration.name,
                              .json_pointer = base + "/default"});
                    }
                    declaration.default_value = *def;
                }
            } else if (kind == "object") {
                requireOnly(record, {"name", "kind", "required_components"},
                            PrefabErrorCode::PrefabDocumentInvalid,
                            "object parameter");
                declaration.kind = PrefabParameterKind::Object;
                if (const auto required = record.find("required_components"); required != record.end()) {
                    if (!required->is_array()) {
                        fail(PrefabErrorCode::PrefabDocumentInvalid,
                             "required_components must be an array",
                             {.prefab = result.name, .parameter = declaration.name,
                              .json_pointer = base + "/required_components"});
                    }
                    std::unordered_set<std::string> seen;
                    for (const auto &name : *required) {
                        if (!name.is_string() ||
                            !isComponentIdentifier(name.get_ref<const std::string &>()) ||
                            !seen.insert(name.get<std::string>()).second) {
                            fail(PrefabErrorCode::PrefabDocumentInvalid,
                                 "required_components must contain unique component identifiers",
                                 {.prefab = result.name, .parameter = declaration.name,
                                  .json_pointer = base + "/required_components"});
                        }
                        declaration.required_components.push_back(name.get<std::string>());
                    }
                }
            } else {
                fail(PrefabErrorCode::PrefabDocumentInvalid,
                     "prefab parameter kind must be asset or object",
                     {.prefab = result.name, .parameter = declaration.name,
                      .json_pointer = base + "/kind"});
            }
        }
        result.parameters.push_back(std::move(declaration));
    }

    if (!value.contains("components") || !value.at("components").is_array() ||
        value.at("components").empty()) {
        fail(PrefabErrorCode::PrefabDocumentInvalid,
             "prefab components must be a non-empty array",
             {.prefab = result.name, .json_pointer = "/components"});
    }
    std::unordered_set<std::string> keys;
    std::unordered_set<std::string> used;
    const auto &components = value.at("components");
    result.components.reserve(components.size());
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto &component = components[index];
        const auto base = "/components/" + std::to_string(index);
        requireObject(component, PrefabErrorCode::PrefabDocumentInvalid,
                      "prefab component", {.prefab = result.name,
                                           .json_pointer = base});
        if (taggedNode(component.value("name", Json{}))) {
            fail(PrefabErrorCode::PrefabBindingNotBindable,
                 "component name is not bindable",
                 {.prefab = result.name, .json_pointer = base + "/name"});
        }
        PrefabComponentDocument parsed;
        parsed.name = requiredString(component, "name",
                                     PrefabErrorCode::PrefabDocumentInvalid,
                                     "prefab component");
        if (!isComponentIdentifier(parsed.name)) {
            fail(PrefabErrorCode::PrefabDocumentInvalid,
                 "prefab component name must match the scene component vocabulary",
                 {.prefab = result.name, .json_pointer = base + "/name"});
        }
        if (parsed.name == "transform") {
            fail(PrefabErrorCode::PrefabTransformForbidden,
                 "prefab components may not contain transform",
                 {.prefab = result.name, .json_pointer = base + "/name"});
        }
        if (component.contains("prefab")) {
            fail(PrefabErrorCode::PrefabNestedUnsupported,
                 "nested prefab expansion is not supported",
                 {.prefab = result.name, .json_pointer = base + "/prefab"});
        }
        parsed.key = parsed.name;
        if (const auto id = component.find("id"); id != component.end()) {
            if (taggedNode(*id)) {
                fail(PrefabErrorCode::PrefabBindingNotBindable,
                     "component id is not bindable",
                     {.prefab = result.name, .json_pointer = base + "/id"});
            }
            if (!id->is_string() || !isPrefabIdentifier(id->get_ref<const std::string &>())) {
                fail(PrefabErrorCode::PrefabDocumentInvalid,
                     "prefab component id has invalid syntax",
                     {.prefab = result.name, .json_pointer = base + "/id"});
            }
            parsed.key = id->get<std::string>();
        }
        if (!keys.insert(parsed.key).second) {
            fail(PrefabErrorCode::PrefabComponentKeyDuplicate,
                 "duplicate prefab component key: " + parsed.key,
                 {.prefab = result.name, .json_pointer = base});
        }
        parsed.body = component;
        parsed.body.erase("id");
        collectBindings(parsed.body, "", parsed.bindings, parameter_names, used,
                        result.name);
        result.components.push_back(std::move(parsed));
    }
    for (const auto &parameter : result.parameters) {
        if (!used.contains(parameter.name)) {
            fail(PrefabErrorCode::PrefabParameterUnused,
                 "prefab parameter is not used: " + parameter.name,
                 {.prefab = result.name, .parameter = parameter.name});
        }
    }
    return result;
}

PrefabDocument parsePrefabDocumentText(std::string_view bytes,
                                       std::string_view expected_name) {
    try {
        return parsePrefabDocumentJson(Json::parse(bytes), expected_name);
    } catch (const PrefabError &) {
        throw;
    } catch (const Json::exception &error) {
        fail(PrefabErrorCode::PrefabDocumentInvalid, error.what(),
             {.prefab = expected_name.empty() ? std::nullopt
                                              : std::optional<std::string>{expected_name}});
    }
}

struct PrefabRegistrySnapshot::Data {
    std::uint64_t generation = 0;
    std::vector<PrefabRegistryRecord> records;
};

PrefabRegistrySnapshot::PrefabRegistrySnapshot()
    : data_{std::make_shared<const Data>()} {}
PrefabRegistrySnapshot::PrefabRegistrySnapshot(std::shared_ptr<const Data> data)
    : data_{std::move(data)} {}
std::uint64_t PrefabRegistrySnapshot::generation() const noexcept { return data_->generation; }
std::span<const PrefabRegistryRecord> PrefabRegistrySnapshot::records() const noexcept { return data_->records; }
bool PrefabRegistrySnapshot::empty() const noexcept { return data_->records.empty(); }
const PrefabRegistryRecord *PrefabRegistrySnapshot::find(std::string_view name) const noexcept {
    const auto found = std::lower_bound(data_->records.begin(), data_->records.end(), name,
                                       [](const auto &entry, std::string_view key) {
                                           return entry.name < key;
                                       });
    return found != data_->records.end() && found->name == name ? &*found : nullptr;
}

PrefabRegistrySnapshot buildPrefabRegistrySnapshot(
    std::span<const PrefabRegistryEntry> entries,
    const std::function<std::string(std::string_view)> &load_raw_bytes,
    std::uint64_t generation) {
    auto data = std::make_shared<PrefabRegistrySnapshot::Data>();
    data->generation = generation;
    data->records.reserve(entries.size());
    for (const auto &entry : entries) {
        std::string bytes;
        try {
            bytes = load_raw_bytes(entry.path);
        } catch (const PrefabError &) {
            throw;
        } catch (const std::exception &error) {
            fail(PrefabErrorCode::PrefabNotFound,
                 "failed to load prefab " + entry.name + ": " + error.what(),
                 {.prefab = entry.name});
        }
        auto document = std::make_shared<const PrefabDocument>(
            parsePrefabDocumentText(bytes, entry.name));
        data->records.push_back({.name = entry.name,
                                 .path = entry.path,
                                 .digest_sha256 = picosha2::hash256_hex_string(bytes.begin(), bytes.end()),
                                 .document = std::move(document)});
    }
    std::sort(data->records.begin(), data->records.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.name < rhs.name; });
    return PrefabRegistrySnapshot{std::move(data)};
}

PrefabInstanceDeclaration resolvePrefabInstance(
    const Json &prefab_block, const PrefabRegistrySnapshot &registry,
    PrefabErrorContext context) {
    requireObject(prefab_block, PrefabErrorCode::PrefabInstanceInvalid,
                  "scene prefab instance", context);
    requireOnly(prefab_block, {"ref", "instance_id", "parameters"},
                PrefabErrorCode::PrefabInstanceInvalid,
                "scene prefab instance", context);
    PrefabInstanceDeclaration result;
    result.ref = requiredString(prefab_block, "ref",
                                PrefabErrorCode::PrefabInstanceInvalid,
                                "scene prefab instance", context);
    context.prefab = result.ref;
    const auto *record = registry.find(result.ref);
    if (record == nullptr) {
        fail(PrefabErrorCode::PrefabNotFound,
             "prefab is not registered: " + result.ref, context);
    }
    result.instance_id = requiredString(prefab_block, "instance_id",
                                        PrefabErrorCode::PrefabInstanceInvalid,
                                        "scene prefab instance", context);
    context.instance_id = result.instance_id;
    if (!isPrefabIdentifier(result.instance_id)) {
        context.json_pointer = "/prefab/instance_id";
        fail(PrefabErrorCode::PrefabInstanceIdInvalid,
             "prefab instance_id has invalid syntax", context);
    }
    const auto &authored = prefab_block.value("parameters", Json::object());
    if (!authored.is_object()) {
        context.json_pointer = "/prefab/parameters";
        fail(PrefabErrorCode::PrefabInstanceInvalid,
             "prefab instance parameters must be an object", context);
    }
    std::unordered_map<std::string, const PrefabParameterDeclaration *> declarations;
    for (const auto &parameter : record->document->parameters) {
        declarations.emplace(parameter.name, &parameter);
    }
    for (const auto &[name, unused] : authored.items()) {
        (void)unused;
        if (!declarations.contains(name)) {
            context.parameter = name;
            context.json_pointer = "/prefab/parameters/" + pointerToken(name);
            fail(PrefabErrorCode::PrefabParameterUnknown,
                 "unknown prefab instance parameter: " + name, context);
        }
    }
    result.parameters.reserve(record->document->parameters.size());
    for (const auto &parameter : record->document->parameters) {
        PrefabResolvedParameter resolved{.name = parameter.name,
                                         .kind = parameter.kind,
                                         .required_components = parameter.required_components};
        const auto found = authored.find(parameter.name);
        const Json *value = nullptr;
        if (found != authored.end()) {
            value = &*found;
            resolved.is_override = true;
            resolved.value_authored = *found;
        } else if (parameter.default_value) {
            value = &*parameter.default_value;
        } else {
            context.parameter = parameter.name;
            context.json_pointer = "/prefab/parameters/" + pointerToken(parameter.name);
            fail(PrefabErrorCode::PrefabParameterRequired,
                 "required prefab parameter is missing: " + parameter.name,
                 context);
        }
        context.json_pointer = "/prefab/parameters/" + pointerToken(parameter.name);
        resolved.value_resolved = resolveParameterValue(parameter, *value, context);
        result.parameters.push_back(std::move(resolved));
    }
    return result;
}

nlohmann::ordered_json substitutePrefabComponent(
    const PrefabComponentDocument &component,
    std::span<const PrefabResolvedParameter> parameters) {
    nlohmann::ordered_json result = component.body;
    for (const auto &binding : component.bindings) {
        const auto found = std::find_if(parameters.begin(), parameters.end(),
                                        [&](const auto &parameter) {
                                            return parameter.name == binding.parameter;
                                        });
        if (found == parameters.end()) {
            throw std::logic_error("validated prefab binding lost its parameter");
        }
        result[nlohmann::ordered_json::json_pointer{binding.json_pointer}] =
            found->value_resolved;
    }
    return result;
}

std::string prefabGeneratedId(std::string_view instance_id,
                              std::string_view component_key) {
    std::vector<unsigned char> bytes;
    appendLp(bytes, "pelican.prefab.gid.v1");
    appendLp(bytes, instance_id);
    appendLp(bytes, component_key);
    std::vector<unsigned char> digest(picosha2::k_digest_size);
    picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "gid_";
    result.reserve(4 + 32);
    for (std::size_t index = 0; index < 16; ++index) {
        result.push_back(hex[digest[index] >> 4]);
        result.push_back(hex[digest[index] & 0x0f]);
    }
    return result;
}

} // namespace Pelican
