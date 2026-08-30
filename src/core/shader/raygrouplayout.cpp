#include "raygrouplayout.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Pelican {
namespace {

thread_local RayTracingSbtWriteObserver sbt_write_observer;
thread_local RayTracingTlasRecordOffsetOverrideForTesting
    tlas_record_offset_override;
thread_local RayTracingJointTransactionInjector
    joint_transaction_injector;

using json = nlohmann::json;

[[noreturn]] void layoutError(
    std::string_view source, std::string message) {
    throw std::runtime_error(
        "pelican.ray_group_layout_invalid@1: " + message +
        (source.empty() ? std::string{} : " in '" + std::string{source} + "'"));
}

[[noreturn]] void bindingRecordError(
    std::string_view source, std::string message) {
    throw std::runtime_error(
        "pelican.ray_binding_record_invalid@1: " + message +
        (source.empty() ? std::string{} : " in '" + std::string{source} + "'"));
}

bool validName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    });
}

void requireObjectFields(
    const json &value, std::initializer_list<std::string_view> allowed,
    std::string_view domain, std::string_view source) {
    if (!value.is_object()) {
        layoutError(source, std::string{domain} + " must be an object");
    }
    for (auto field = value.begin(); field != value.end(); ++field) {
        const auto known = std::find(allowed.begin(), allowed.end(),
                                     field.key()) != allowed.end();
        if (!known) {
            layoutError(source, std::string{domain} +
                                    " has unknown field '" + field.key() + "'");
        }
    }
}

std::uint32_t alignUp(
    std::uint32_t value, std::uint32_t alignment,
    std::string_view diagnostic) {
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0) {
        throw std::runtime_error(std::string{diagnostic} +
                                 ": alignment must be a non-zero power of two");
    }
    const auto mask = alignment - 1u;
    if (value > std::numeric_limits<std::uint32_t>::max() - mask) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: alignment overflow");
    }
    return (value + mask) & ~mask;
}

struct TypeFacts {
    RayRecordType type;
    std::uint32_t size;
    std::uint32_t alignment;
    std::uint32_t components;
};

TypeFacts typeFacts(std::string_view encoded, std::string_view source) {
    if (encoded == "i32") return {RayRecordType::i32, 4, 4, 1};
    if (encoded == "u32") return {RayRecordType::u32, 4, 4, 1};
    if (encoded == "f32") return {RayRecordType::f32, 4, 4, 1};
    if (encoded == "vec2") return {RayRecordType::vec2, 8, 8, 2};
    if (encoded == "vec3") return {RayRecordType::vec3, 12, 16, 3};
    if (encoded == "vec4") return {RayRecordType::vec4, 16, 16, 4};
    throw std::runtime_error(
        "pelican.ray_record_type_unsupported@1: type '" +
        std::string{encoded} + "'" +
        (source.empty() ? std::string{} : " in '" + std::string{source} + "'"));
}

std::vector<std::string> parseNameStrings(
    const json &value, std::string_view field,
    std::string_view source) {
    if (!value.is_array() || value.empty()) {
        layoutError(source, std::string{field} + " must be a non-empty array");
    }
    std::vector<std::string> result;
    std::set<std::string> unique;
    for (const auto &entry : value) {
        if (!entry.is_string() || !validName(entry.get_ref<const std::string &>())) {
            layoutError(source, std::string{field} +
                                    " names must match ^[A-Za-z0-9_]{1,64}$");
        }
        auto name = entry.get<std::string>();
        if (!unique.emplace(name).second) {
            layoutError(source, std::string{field} +
                                    " contains duplicate name '" + name + "'");
        }
        result.push_back(std::move(name));
    }
    return result;
}

std::vector<std::string> parseNamedObjects(
    const json &value, std::string_view field,
    std::string_view source) {
    if (!value.is_array() || value.empty()) {
        layoutError(source, std::string{field} + " must be a non-empty array");
    }
    std::vector<std::string> result;
    std::set<std::string> unique;
    for (const auto &entry : value) {
        requireObjectFields(entry, {"name"}, field, source);
        if (!entry.contains("name") || !entry.at("name").is_string() ||
            !validName(entry.at("name").get_ref<const std::string &>())) {
            layoutError(source, std::string{field} +
                                    " names must match ^[A-Za-z0-9_]{1,64}$");
        }
        auto name = entry.at("name").get<std::string>();
        if (!unique.emplace(name).second) {
            layoutError(source, std::string{field} +
                                    " contains duplicate name '" + name + "'");
        }
        result.push_back(std::move(name));
    }
    return result;
}

std::uint32_t findIndex(
    std::span<const std::string> names, std::string_view name,
    std::string_view domain) {
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) {
        throw std::runtime_error(
            "pelican.ray_group_name_unknown@1: " + std::string{domain} +
            " '" + std::string{name} + "'");
    }
    return static_cast<std::uint32_t>(found - names.begin());
}

void writeU32(std::span<std::byte> destination, std::uint32_t value) {
    destination[0] = static_cast<std::byte>(value & 0xffu);
    destination[1] = static_cast<std::byte>((value >> 8u) & 0xffu);
    destination[2] = static_cast<std::byte>((value >> 16u) & 0xffu);
    destination[3] = static_cast<std::byte>((value >> 24u) & 0xffu);
}

float checkedFloat(const json &value, std::string_view member,
                   std::string_view source) {
    if (!value.is_number()) {
        bindingRecordError(source, "member '" + std::string{member} +
                                       "' requires an f32 number");
    }
    const auto encoded = value.get<double>();
    const auto converted = static_cast<float>(encoded);
    if (!std::isfinite(encoded) || !std::isfinite(converted)) {
        bindingRecordError(source, "member '" + std::string{member} +
                                       "' is outside finite f32");
    }
    return converted;
}

std::vector<std::byte> encodeRecord(
    const json &record, const RayGroupLayout &layout,
    std::string_view source) {
    if (!record.is_object() || record.size() != layout.record.size()) {
        bindingRecordError(source,
                           "record member set does not match layout");
    }
    for (auto field = record.begin(); field != record.end(); ++field) {
        if (std::none_of(layout.record.begin(), layout.record.end(),
                         [&](const auto &member) { return member.name == field.key(); })) {
            bindingRecordError(source, "unknown record member '" + field.key() + "'");
        }
    }
    std::vector<std::byte> result(layout.data_size, std::byte{0});
    for (const auto &member : layout.record) {
        if (!record.contains(member.name)) {
            bindingRecordError(source, "missing record member '" + member.name + "'");
        }
        const auto &value = record.at(member.name);
        const auto write_scalar = [&](std::uint32_t component, std::uint32_t bits) {
            writeU32(std::span{result}.subspan(member.offset + component * 4u, 4), bits);
        };
        if (member.type == RayRecordType::i32) {
            if (!value.is_number_integer() || value.is_number_unsigned()) {
                bindingRecordError(source, "member '" + member.name + "' requires i32");
            }
            const auto number = value.get<std::int64_t>();
            if (number < std::numeric_limits<std::int32_t>::min() ||
                number > std::numeric_limits<std::int32_t>::max()) {
                bindingRecordError(source, "member '" + member.name + "' is outside i32");
            }
            write_scalar(0, static_cast<std::uint32_t>(static_cast<std::int32_t>(number)));
        } else if (member.type == RayRecordType::u32) {
            std::uint64_t number = 0;
            if (value.is_number_unsigned()) {
                number = value.get<std::uint64_t>();
            } else if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
                number = static_cast<std::uint64_t>(value.get<std::int64_t>());
            } else {
                bindingRecordError(source, "member '" + member.name + "' requires u32");
            }
            if (number > std::numeric_limits<std::uint32_t>::max()) {
                bindingRecordError(source, "member '" + member.name + "' is outside u32");
            }
            write_scalar(0, static_cast<std::uint32_t>(number));
        } else if (member.type == RayRecordType::f32) {
            write_scalar(0, std::bit_cast<std::uint32_t>(
                                checkedFloat(value, member.name, source)));
        } else {
            const auto components = member.size / 4u;
            if (!value.is_array() || value.size() != components) {
                bindingRecordError(source, "member '" + member.name +
                                               "' has wrong vector length");
            }
            for (std::uint32_t component = 0; component < components; ++component) {
                write_scalar(component, std::bit_cast<std::uint32_t>(
                                            checkedFloat(value.at(component), member.name, source)));
            }
        }
    }
    return result;
}

std::string materialReference(const RaySceneRowInput &row) {
    if (!row.material_document.empty()) return "document:" + row.material_document;
    return "asset:" + row.material_asset + "#" +
           std::to_string(row.source_material_index);
}

bool selectorMatches(
    const RayBindingSelector &selector, const RaySceneRowInput &row) {
    if (selector.kind == RayBindingSelector::Kind::document) {
        return !row.material_document.empty() &&
               selector.reference == row.material_document;
    }
    return row.material_document.empty() &&
           selector.reference == row.material_asset &&
           selector.source_material_index == row.source_material_index;
}

} // namespace

std::uint32_t RayGroupLayout::rayTypeIndex(std::string_view name) const {
    return findIndex(ray_types, name, "ray type");
}

std::uint32_t RayGroupLayout::groupIndex(std::string_view name) const {
    return findIndex(groups, name, "group");
}

std::uint32_t RayGroupLayout::missIndex(std::string_view name) const {
    return findIndex(misses, name, "miss");
}

RayGroupLayout parseRayGroupLayout(
    std::string_view raw_bytes, std::string source_reference) {
    json root;
    try {
        root = json::parse(raw_bytes);
    } catch (const json::exception &error) {
        layoutError(source_reference, "invalid JSON: " + std::string{error.what()});
    }
    requireObjectFields(root,
                        {"schema", "version", "ray_types", "default_group",
                         "groups", "misses", "record"},
                        "layout", source_reference);
    if (!root.contains("schema") || root.at("schema") != "pelican.ray_group_layout" ||
        !root.contains("version") || !root.at("version").is_number_integer() ||
        root.at("version") != 1) {
        layoutError(source_reference,
                    "schema must be pelican.ray_group_layout version 1");
    }
    for (const auto required : {"ray_types", "default_group", "groups", "misses", "record"}) {
        if (!root.contains(required)) {
            layoutError(source_reference, "missing field '" + std::string{required} + "'");
        }
    }
    RayGroupLayout result;
    result.source_reference = std::move(source_reference);
    result.digest_sha256 = picosha2::hash256_hex_string(raw_bytes.begin(), raw_bytes.end());
    result.ray_types = parseNameStrings(root.at("ray_types"), "ray_types", result.source_reference);
    result.groups = parseNamedObjects(root.at("groups"), "groups", result.source_reference);
    result.misses = parseNamedObjects(root.at("misses"), "misses", result.source_reference);
    if (!root.at("default_group").is_string() ||
        !validName(root.at("default_group").get_ref<const std::string &>())) {
        layoutError(result.source_reference,
                    "default_group must match ^[A-Za-z0-9_]{1,64}$");
    }
    result.default_group = root.at("default_group").get<std::string>();
    if (std::find(result.groups.begin(), result.groups.end(), result.default_group) ==
        result.groups.end()) {
        layoutError(result.source_reference,
                    "default_group '" + result.default_group + "' is not in groups");
    }
    const auto &record = root.at("record");
    if (!record.is_array()) {
        layoutError(result.source_reference, "record must be an array");
    }
    std::set<std::string> member_names;
    std::uint32_t cursor = 0;
    for (const auto &encoded : record) {
        requireObjectFields(encoded, {"name", "type"}, "record member", result.source_reference);
        if (!encoded.contains("name") || !encoded.at("name").is_string() ||
            !validName(encoded.at("name").get_ref<const std::string &>()) ||
            !encoded.contains("type") || !encoded.at("type").is_string()) {
            layoutError(result.source_reference,
                        "record member requires valid name and string type");
        }
        auto name = encoded.at("name").get<std::string>();
        if (!member_names.emplace(name).second) {
            layoutError(result.source_reference,
                        "record contains duplicate member '" + name + "'");
        }
        const auto facts = typeFacts(encoded.at("type").get_ref<const std::string &>(),
                                     result.source_reference);
        cursor = alignUp(cursor, facts.alignment,
                         "pelican.ray_group_layout_invalid@1");
        result.record.push_back({std::move(name), facts.type, cursor,
                                 facts.size, facts.alignment});
        if (cursor > std::numeric_limits<std::uint32_t>::max() - facts.size) {
            throw std::overflow_error(
                "pelican.sbt.layout_overflow@1: record ABI size overflow");
        }
        cursor += facts.size;
    }
    // A struct's std430 base alignment is the maximum member alignment; the
    // final size is rounded to that alignment. Empty records have size zero.
    std::uint32_t struct_alignment = 1;
    for (const auto &member : result.record) {
        struct_alignment = std::max(struct_alignment, member.alignment);
    }
    result.data_size = result.record.empty()
                           ? 0
                           : alignUp(cursor, struct_alignment,
                                     "pelican.ray_group_layout_invalid@1");
    return result;
}

std::string_view rayRecordTypeName(RayRecordType type) noexcept {
    switch (type) {
    case RayRecordType::i32: return "i32";
    case RayRecordType::u32: return "u32";
    case RayRecordType::f32: return "f32";
    case RayRecordType::vec2: return "vec2";
    case RayRecordType::vec3: return "vec3";
    case RayRecordType::vec4: return "vec4";
    }
    return "unknown";
}

json canonicalRayRecordAbiJson(const RayGroupLayout &layout) {
    json members = json::array();
    for (const auto &member : layout.record) {
        members.push_back({{"name", member.name},
                           {"type", rayRecordTypeName(member.type)},
                           {"offset", member.offset},
                           {"size", member.size},
                           {"alignment", member.alignment}});
    }
    return {{"schema", "pelican.ray_record_abi"},
            {"version", 1},
            {"data_size", layout.data_size},
            {"members", std::move(members)}};
}

std::string makeRayGroupLayoutVirtualInclude(const RayGroupLayout &layout) {
    std::ostringstream source;
    source << "// pelican.ray_group_layout sha256=" << layout.digest_sha256 << '\n';
    for (std::size_t index = 0; index < layout.ray_types.size(); ++index) {
        source << "#define PELICAN_RAY_TYPE_" << layout.ray_types[index]
               << ' ' << index << '\n';
    }
    for (std::size_t index = 0; index < layout.misses.size(); ++index) {
        source << "#define PELICAN_MISS_" << layout.misses[index]
               << ' ' << index << '\n';
    }
    return source.str();
}

RayBindings parseRayBindings(
    std::string_view raw_bytes, const RayGroupLayout &layout,
    std::string source_reference) {
    json root;
    try {
        root = json::parse(raw_bytes);
    } catch (const json::exception &error) {
        throw std::runtime_error(
            "pelican.ray_bindings_invalid@1: invalid JSON: " +
            std::string{error.what()});
    }
    if (!root.is_object()) {
        throw std::runtime_error("pelican.ray_bindings_invalid@1: root must be an object");
    }
    for (auto field = root.begin(); field != root.end(); ++field) {
        if (field.key() != "schema" && field.key() != "version" &&
            field.key() != "bindings") {
            throw std::runtime_error(
                "pelican.ray_bindings_invalid@1: unknown field '" + field.key() + "'");
        }
    }
    if (!root.contains("schema") || root.at("schema") != "pelican.ray_bindings" ||
        !root.contains("version") || !root.at("version").is_number_integer() ||
        root.at("version") != 1 || !root.contains("bindings") ||
        !root.at("bindings").is_array()) {
        throw std::runtime_error(
            "pelican.ray_bindings_invalid@1: schema must be pelican.ray_bindings version 1 with bindings array");
    }
    RayBindings result;
    result.source_reference = std::move(source_reference);
    result.digest_sha256 = picosha2::hash256_hex_string(raw_bytes.begin(), raw_bytes.end());
    for (const auto &entry : root.at("bindings")) {
        if (!entry.is_object()) {
            throw std::runtime_error("pelican.ray_bindings_invalid@1: binding must be an object");
        }
        for (auto field = entry.begin(); field != entry.end(); ++field) {
            if (field.key() != "material" && field.key() != "group" &&
                field.key() != "record") {
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: binding has unknown field '" + field.key() + "'");
            }
        }
        if (!entry.contains("material") || !entry.at("material").is_object() ||
            !entry.contains("group") || !entry.at("group").is_string() ||
            !entry.contains("record")) {
            throw std::runtime_error(
                "pelican.ray_bindings_invalid@1: binding requires material, group, and record");
        }
        const auto &material = entry.at("material");
        for (auto field = material.begin(); field != material.end(); ++field) {
            if (field.key() != "document" && field.key() != "asset" &&
                field.key() != "source_material_index") {
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: material selector has unknown field '" +
                    field.key() + "'");
            }
        }
        const auto has_document = material.contains("document");
        const auto has_asset = material.contains("asset");
        const auto has_index = material.contains("source_material_index");
        if (has_document == has_asset || has_asset != has_index ||
            (has_document && material.size() != 1) ||
            (has_asset && material.size() != 2)) {
            throw std::runtime_error(
                "pelican.ray_bindings_invalid@1: material selector requires exactly document or asset+source_material_index");
        }
        RayBindingSelector selector;
        if (has_document) {
            if (!material.at("document").is_string() ||
                material.at("document").get_ref<const std::string &>().empty()) {
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: document selector must be a non-empty string");
            }
            selector.kind = RayBindingSelector::Kind::document;
            selector.reference = material.at("document").get<std::string>();
        } else {
            if (!material.at("asset").is_string() ||
                material.at("asset").get_ref<const std::string &>().empty()) {
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: asset selector must be a non-empty string");
            }
            std::uint64_t index = 0;
            const auto &encoded = material.at("source_material_index");
            if (encoded.is_number_unsigned()) index = encoded.get<std::uint64_t>();
            else if (encoded.is_number_integer() && encoded.get<std::int64_t>() >= 0)
                index = static_cast<std::uint64_t>(encoded.get<std::int64_t>());
            else
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: source_material_index must be uint32");
            if (index > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: source_material_index exceeds uint32");
            }
            selector.kind = RayBindingSelector::Kind::asset_material;
            selector.reference = material.at("asset").get<std::string>();
            selector.source_material_index = static_cast<std::uint32_t>(index);
        }
        result.bindings.push_back(
            {.selector = std::move(selector),
             .group_index = layout.groupIndex(entry.at("group").get_ref<const std::string &>()),
             .record_data = encodeRecord(entry.at("record"), layout, result.source_reference)});
    }
    return result;
}

std::shared_ptr<const RayTracingProjectConfig>
loadRayTracingProjectConfig(
    const json &authored_root,
    const std::function<std::string(std::string_view)> &load_document,
    bool runtime_shader_compiler_enabled) {
    if (!authored_root.is_object()) {
        throw std::runtime_error(
            "pelican.ray_tracing.root_invalid@1: rendering config root must be an object");
    }
    const auto has_layout = authored_root.contains("ray_group_layout");
    const auto has_bindings = authored_root.contains("ray_bindings");
    if (!has_layout) {
        if (has_bindings) {
            throw std::runtime_error(
                "pelican.ray_tracing.bindings_without_layout@1: ray_bindings requires ray_group_layout");
        }
        return {};
    }
    if (!runtime_shader_compiler_enabled) {
        throw std::runtime_error(
            "pelican.ray_tracing.layout_requires_runtime_compiler@1: ray_group_layout projects require the runtime shader compiler");
    }
    if (!authored_root.at("ray_group_layout").is_string() ||
        authored_root.at("ray_group_layout").get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            "pelican.ray_tracing.root_invalid@1: ray_group_layout must be a non-empty string reference");
    }
    if (!load_document) {
        throw std::runtime_error(
            "pelican.ray_tracing.layout_loader_unavailable@1: no document loader is available");
    }
    auto result = std::make_shared<RayTracingProjectConfig>();
    const auto layout_reference =
        authored_root.at("ray_group_layout").get<std::string>();
    result->layout = parseRayGroupLayout(
        load_document(layout_reference), layout_reference);
    if (has_bindings) {
        if (!authored_root.at("ray_bindings").is_string() ||
            authored_root.at("ray_bindings").get_ref<const std::string &>().empty()) {
            throw std::runtime_error(
                "pelican.ray_tracing.root_invalid@1: ray_bindings must be a non-empty string reference");
        }
        const auto bindings_reference =
            authored_root.at("ray_bindings").get<std::string>();
        result->bindings = parseRayBindings(
            load_document(bindings_reference), result->layout,
            bindings_reference);
    }
    return result;
}

RaySceneRowSnapshot resolveRaySceneRows(
    std::span<const RaySceneRowInput> inputs,
    const RayGroupLayout &layout, const RayBindings *bindings) {
    std::vector<RaySceneRow> resolved;
    resolved.reserve(inputs.size());
    std::vector<bool> binding_matched(bindings ? bindings->bindings.size() : 0, false);
    for (const auto &input : inputs) {
        const RayMaterialBinding *chosen = nullptr;
        if (bindings != nullptr) {
            for (std::size_t index = 0; index < bindings->bindings.size(); ++index) {
                if (!selectorMatches(bindings->bindings[index].selector, input)) continue;
                binding_matched[index] = true;
                if (chosen != nullptr) {
                    throw std::runtime_error(
                        "pelican.ray_binding_selector_duplicate@1: material '" +
                        materialReference(input) + "' resolves more than once");
                }
                chosen = &bindings->bindings[index];
            }
        }
        resolved.push_back(
            {.source_index = input.source_index,
             .group_index = chosen ? chosen->group_index
                                   : layout.groupIndex(layout.default_group),
             .record_data = chosen ? chosen->record_data
                                   : std::vector<std::byte>(layout.data_size, std::byte{0}),
             .scene_identity = input.scene_identity,
             .geometry_identity = input.geometry_identity,
             .material_reference = materialReference(input)});
    }
    if (bindings != nullptr) {
        for (std::size_t index = 0; index < binding_matched.size(); ++index) {
            if (!binding_matched[index]) {
                const auto &selector = bindings->bindings[index].selector;
                throw std::runtime_error(
                    "pelican.ray_binding_selector_unresolved@1: selector '" +
                    selector.reference + "' did not resolve to a scene row");
            }
        }
    }
    std::sort(resolved.begin(), resolved.end(), [](const auto &left, const auto &right) {
        return std::tie(left.scene_identity, left.geometry_identity,
                        left.material_reference, left.source_index) <
               std::tie(right.scene_identity, right.geometry_identity,
                        right.material_reference, right.source_index);
    });
    for (std::size_t index = 1; index < resolved.size(); ++index) {
        const auto &left = resolved[index - 1];
        const auto &right = resolved[index];
        if (left.scene_identity == right.scene_identity &&
            left.geometry_identity == right.geometry_identity &&
            (left.group_index != right.group_index ||
             left.record_data != right.record_data)) {
            throw std::runtime_error(
                "pelican.ray_row_binding_ambiguous@1: geometric row '" +
                left.scene_identity + "/" + left.geometry_identity +
                "' has inconsistent material bindings");
        }
    }
    std::ostringstream identity;
    identity << layout.digest_sha256 << '\n';
    if (bindings != nullptr) identity << bindings->digest_sha256;
    identity << '\n';
    for (std::size_t index = 0; index < resolved.size(); ++index) {
        if (index >= (1u << 24u)) {
            throw std::runtime_error(
                "pelican.ray_row_count_limit@1: scene row q exceeds the 24-bit instance SBT offset domain");
        }
        resolved[index].q = static_cast<std::uint32_t>(index);
        identity << resolved[index].scene_identity << '\0'
                 << resolved[index].geometry_identity << '\0'
                 << resolved[index].material_reference << '\0'
                 << resolved[index].group_index << '\n';
        for (const auto byte : resolved[index].record_data)
            identity << static_cast<char>(byte);
    }
    RaySceneRowSnapshot result;
    const auto encoded = identity.str();
    result.identity_digest_sha256 =
        picosha2::hash256_hex_string(encoded.begin(), encoded.end());
    result.rows = std::move(resolved);
    return result;
}

void validateRayTracingRecordAddressing(
    std::uint32_t sbt_record_offset, std::uint32_t sbt_record_stride,
    std::uint32_t miss_index, const RayGroupLayout &layout,
    const RayTracingRecordAddressing &addressing) {
    const auto ray_type_count = static_cast<std::uint32_t>(layout.ray_types.size());
    if (addressing.record_index_stride != ray_type_count ||
        sbt_record_stride != ray_type_count) {
        throw std::runtime_error(
            "pelican.ray_tracing.record_index_stride_invalid@1: sbtRecordStride=" +
            std::to_string(sbt_record_stride) + " required=" +
            std::to_string(ray_type_count));
    }
    if (std::find(addressing.traced_ray_type_indices.begin(),
                  addressing.traced_ray_type_indices.end(),
                  sbt_record_offset) == addressing.traced_ray_type_indices.end()) {
        throw std::runtime_error(
            "pelican.ray_tracing.ray_type_offset_invalid@1: sbtRecordOffset=" +
            std::to_string(sbt_record_offset) + " is not a traced ray type index");
    }
    if (miss_index >= layout.misses.size()) {
        throw std::runtime_error(
            "pelican.ray_tracing.miss_index_invalid@1: missIndex=" +
            std::to_string(miss_index) + " miss_count=" +
            std::to_string(layout.misses.size()));
    }
}

std::uint32_t rayTracingInstanceRecordOffset(
    std::uint32_t q, std::uint32_t ray_type_count) {
    constexpr std::uint32_t max_offset = (1u << 24u) - 1u;
    if (ray_type_count == 0 || q > max_offset / ray_type_count) {
        throw std::runtime_error(
            "pelican.ray_row_count_limit@1: q*ray_type_count exceeds the 24-bit instance SBT offset domain");
    }
    return q * ray_type_count;
}

RayTracingPackedHitTable packRayTracingHitTable(
    const RayGroupLayout &layout,
    const RayTracingHitTablePackRequest &request) {
    if (layout.ray_types.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        layout.groups.size() >
            std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: ray type or group count exceeds uint32");
    }
    const auto ray_type_count = static_cast<std::uint32_t>(layout.ray_types.size());
    const auto group_count = static_cast<std::uint32_t>(layout.groups.size());
    if (ray_type_count == 0 || group_count == 0 ||
        request.shader_group_handle_size == 0) {
        throw std::runtime_error(
            "pelican.sbt.layout_invalid@1: ray type count, group count, and shaderGroupHandleSize must be non-zero");
    }
    const auto data_start = alignUp(request.shader_group_handle_size,
                                    request.shader_group_handle_alignment,
                                    "pelican.sbt.layout_invalid@1");
    if (data_start > std::numeric_limits<std::uint32_t>::max() - layout.data_size) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: record byte size overflow");
    }
    const auto byte_stride = alignUp(data_start + layout.data_size,
                                     request.shader_group_handle_alignment,
                                     "pelican.sbt.layout_invalid@1");
    if (byte_stride > request.max_shader_group_stride) {
        throw std::runtime_error(
            "pelican.ray_tracing.shader_group_stride_limit@1: record_byte_stride=" +
            std::to_string(byte_stride) + " maxShaderGroupStride=" +
            std::to_string(request.max_shader_group_stride));
    }
    const auto expected_handle_count =
        static_cast<std::uint64_t>(group_count) * ray_type_count;
    if (expected_handle_count >
        std::numeric_limits<std::uint64_t>::max() /
            request.shader_group_handle_size) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: H[g,r] handle byte size overflow");
    }
    const auto expected_handle_bytes =
        expected_handle_count * request.shader_group_handle_size;
    if (expected_handle_bytes != request.group_handles.size()) {
        throw std::runtime_error(
            "pelican.sbt.handle_table_invalid@1: H[g,r] handle byte count mismatch");
    }
    std::vector<bool> traced(ray_type_count,
                             request.traced_ray_type_indices.empty());
    for (const auto ray : request.traced_ray_type_indices) {
        if (ray >= ray_type_count || traced[ray]) {
            throw std::runtime_error(
                "pelican.sbt.row_snapshot_invalid@1: traced ray type set is out of range or duplicated");
        }
        traced[ray] = true;
    }
    const auto record_count_64 =
        static_cast<std::uint64_t>(request.rows.size()) * ray_type_count;
    constexpr std::uint64_t max_instance_sbt_offset =
        (std::uint64_t{1} << 24u) - 1u;
    if (!request.rows.empty() &&
        static_cast<std::uint64_t>(request.rows.size() - 1u) >
            max_instance_sbt_offset / ray_type_count) {
        throw std::runtime_error(
            "pelican.ray_row_count_limit@1: final q*ray_type_count exceeds the 24-bit instance SBT offset domain");
    }
    if (record_count_64 > std::numeric_limits<std::uint32_t>::max() ||
        (record_count_64 != 0 &&
         byte_stride > std::numeric_limits<std::size_t>::max() / record_count_64)) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: hit record count or byte size overflow");
    }
    std::vector<std::byte> bytes(
        static_cast<std::size_t>(record_count_64) * byte_stride,
        std::byte{0});
    std::vector<bool> q_seen(request.rows.size(), false);
    for (const auto &row : request.rows) {
        if (row.q >= request.rows.size() || row.group_index >= group_count ||
            row.record_data.size() != layout.data_size || q_seen[row.q]) {
            throw std::runtime_error(
                "pelican.sbt.row_snapshot_invalid@1: row q permutation, group, or record data is invalid");
        }
        q_seen[row.q] = true;
        const auto base = rayTracingInstanceRecordOffset(row.q, ray_type_count);
        for (std::uint32_t ray = 0; ray < ray_type_count; ++ray) {
            const auto record_index = static_cast<std::uint64_t>(base) + ray;
            const auto destination = static_cast<std::size_t>(record_index) * byte_stride;
            const auto handle_index =
                static_cast<std::uint64_t>(row.group_index) * ray_type_count + ray;
            const auto handle_source = static_cast<std::size_t>(handle_index) *
                                       request.shader_group_handle_size;
            std::memcpy(bytes.data() + destination,
                        request.group_handles.data() + handle_source,
                        request.shader_group_handle_size);
            if (traced[ray] && !row.record_data.empty()) {
                std::memcpy(bytes.data() + destination + data_start,
                            row.record_data.data(), row.record_data.size());
            }
        }
    }
    return {.data_start = data_start,
            .record_index_stride = ray_type_count,
            .record_byte_stride = byte_stride,
            .record_count = static_cast<std::uint32_t>(record_count_64),
            .bytes = std::move(bytes)};
}

ScopedRayTracingSbtWriteObserver::ScopedRayTracingSbtWriteObserver(
    RayTracingSbtWriteObserver observer)
    : previous_{std::move(sbt_write_observer)} {
    sbt_write_observer = std::move(observer);
}

ScopedRayTracingSbtWriteObserver::~ScopedRayTracingSbtWriteObserver() {
    sbt_write_observer = std::move(previous_);
}

void observeRayTracingSbtWrite(
    const RayTracingSbtWriteObservation &observation) {
    if (sbt_write_observer) sbt_write_observer(observation);
}

ScopedRayTracingTlasRecordOffsetOverrideForTesting::
    ScopedRayTracingTlasRecordOffsetOverrideForTesting(
        RayTracingTlasRecordOffsetOverrideForTesting override_offset)
    : previous_{std::move(tlas_record_offset_override)} {
    tlas_record_offset_override = std::move(override_offset);
}

ScopedRayTracingTlasRecordOffsetOverrideForTesting::
    ~ScopedRayTracingTlasRecordOffsetOverrideForTesting() {
    tlas_record_offset_override = std::move(previous_);
}

std::uint32_t overrideRayTracingTlasRecordOffsetForTesting(
    std::uint32_t q, std::uint32_t correct_offset) {
    return tlas_record_offset_override
               ? tlas_record_offset_override(q, correct_offset)
               : correct_offset;
}

ScopedRayTracingJointTransactionInjector::
    ScopedRayTracingJointTransactionInjector(
        RayTracingJointTransactionInjector injector)
    : previous_{std::move(joint_transaction_injector)} {
    joint_transaction_injector = std::move(injector);
}

ScopedRayTracingJointTransactionInjector::
    ~ScopedRayTracingJointTransactionInjector() {
    joint_transaction_injector = std::move(previous_);
}

void injectRayTracingJointTransactionPoint(
    RayTracingJointTransactionPoint point) {
    if (joint_transaction_injector) {
        joint_transaction_injector(point);
    }
}

} // namespace Pelican
