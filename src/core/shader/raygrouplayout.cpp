#include "raygrouplayout.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <glm/matrix.hpp>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Pelican {

nlohmann::ordered_json rayRecordFieldsJson(std::span<const RayRecordMemberLayout> members, std::span<const std::byte> data) {
    auto fields = nlohmann::ordered_json::array();
    for (const auto &member : members) {
        std::string_view type;
        std::size_t count = 1;
        switch (member.type) {
        case RayRecordType::i32: type = "i32"; break;
        case RayRecordType::u32: type = "u32"; break;
        case RayRecordType::f32: type = "f32"; break;
        case RayRecordType::vec2: type = "vec2"; count = 2; break;
        case RayRecordType::vec3: type = "vec3"; count = 3; break;
        case RayRecordType::vec4: type = "vec4"; count = 4; break;
        }
        if (member.offset > data.size() || count * 4 > data.size() - member.offset)
            throw std::runtime_error("Published ray record schema exceeds row data");
        const auto *bytes = data.data() + member.offset;
        nlohmann::ordered_json value;
        if (member.type == RayRecordType::i32) {
            std::int32_t number;
            std::memcpy(&number, bytes, sizeof(number));
            value = number;
        } else if (member.type == RayRecordType::u32) {
            std::uint32_t number;
            std::memcpy(&number, bytes, sizeof(number));
            value = number;
        } else {
            auto components = nlohmann::ordered_json::array();
            for (std::size_t index = 0; index < count; ++index) {
                float number;
                std::memcpy(&number, bytes + index * sizeof(number), sizeof(number));
                components.push_back(number);
            }
            value = count == 1 ? components.at(0) : components;
        }
        fields.push_back({{"name", member.name}, {"type", type},
                          {"offset", member.offset}, {"value", std::move(value)}});
    }
    return fields;
}


MaterialRowIdentity materialRowIdentity(const RaySceneRowInput &row) {
    if (!row.material_document.empty()) {
        return MaterialRowDocumentIdentity{
            .document = row.material_document,
            .material_name = row.material_name,
        };
    }
    if (!row.material_asset.empty() && row.source_material_index) {
        if (*row.source_material_index ==
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                "pelican.ray_material_index_out_of_range@1: source_material_index is outside the valid material index domain");
        }
        return MaterialRowAssetIdentity{
            .asset = row.material_asset,
            .source_material_index = *row.source_material_index,
        };
    }
    return MaterialRowIdentityUnavailable{};
}

namespace {

static_assert(alignof(RayMaterialTableRow) == 16);
static_assert(offsetof(RayMaterialTableRow, base_color_factor) == 0);
static_assert(offsetof(RayMaterialTableRow, alpha_cutoff) == 16);
static_assert(offsetof(RayMaterialTableRow, alpha_mode) == 20);
static_assert(offsetof(RayMaterialTableRow, base_color_texture_index) == 24);
static_assert(offsetof(RayMaterialTableRow, reserved) == 28);
static_assert(sizeof(RayMaterialTableRow) == 32);
static_assert(alignof(RayProfileMaterialTableRow) == 16);
static_assert(offsetof(RayProfileMaterialTableRow, alpha_mode) == 0);
static_assert(offsetof(RayProfileMaterialTableRow, cutoff) == 4);
static_assert(offsetof(RayProfileMaterialTableRow, base_factor_a) == 8);
static_assert(offsetof(RayProfileMaterialTableRow, texture_index) == 12);
static_assert(offsetof(RayProfileMaterialTableRow, uv_offset_scale) == 16);
static_assert(offsetof(RayProfileMaterialTableRow, uv_rotation) == 32);
static_assert(offsetof(RayProfileMaterialTableRow, uses_vertex_color) == 36);
static_assert(offsetof(RayProfileMaterialTableRow, reserved) == 40);
static_assert(sizeof(RayProfileMaterialTableRow) == 48);
static_assert(alignof(RayGeometryTableRow) == 8);
static_assert(offsetof(RayGeometryTableRow, vertex_address) == 0);
static_assert(offsetof(RayGeometryTableRow, index_address) == 8);
static_assert(offsetof(RayGeometryTableRow, vertex_stride) == 16);
static_assert(offsetof(RayGeometryTableRow, index_type) == 20);
static_assert(offsetof(RayGeometryTableRow, attribute_presence) == 24);
static_assert(offsetof(RayGeometryTableRow, reserved) == 28);
static_assert(sizeof(RayGeometryTableRow) == 32);

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

std::string_view engineRecordFieldName(
    RayEngineRecordField field) noexcept {
    switch (field) {
    case RayEngineRecordField::material_index:
        return "material_index";
    case RayEngineRecordField::geometry_index:
        return "geometry_index";
    case RayEngineRecordField::geometry_kind:
        return "geometry_kind";
    case RayEngineRecordField::procedural_transform:
        return "procedural_transform";
    }
    return "unknown";
}

std::string_view glslRecordTypeName(RayRecordType type) noexcept {
    switch (type) {
    case RayRecordType::i32: return "int";
    case RayRecordType::u32: return "uint";
    case RayRecordType::f32: return "float";
    case RayRecordType::vec2: return "vec2";
    case RayRecordType::vec3: return "vec3";
    case RayRecordType::vec4: return "vec4";
    }
    return "uint";
}

RayRecordMemberLayout appendRecordMember(
    std::string name, const TypeFacts &facts, std::uint32_t &cursor) {
    cursor = alignUp(cursor, facts.alignment,
                     "pelican.ray_group_layout_invalid@1");
    RayRecordMemberLayout result{std::move(name), facts.type, cursor,
                                 facts.size, facts.alignment};
    if (cursor > std::numeric_limits<std::uint32_t>::max() - facts.size) {
        throw std::overflow_error(
            "pelican.sbt.layout_overflow@1: record ABI size overflow");
    }
    cursor += facts.size;
    return result;
}

std::vector<RayRecordMemberLayout> parseRecordMembers(
    const json &record, std::uint32_t &cursor, bool reserve_engine_names,
    std::string_view source) {
    if (!record.is_array()) {
        layoutError(source, "record must be an array");
    }
    std::vector<RayRecordMemberLayout> members;
    std::set<std::string> member_names;
    for (const auto &encoded : record) {
        requireObjectFields(encoded, {"name", "type"}, "record member", source);
        if (!encoded.contains("name") || !encoded.at("name").is_string() ||
            !validName(encoded.at("name").get_ref<const std::string &>()) ||
            !encoded.contains("type") || !encoded.at("type").is_string()) {
            layoutError(source,
                        "record member requires valid name and string type");
        }
        auto name = encoded.at("name").get<std::string>();
        if (reserve_engine_names &&
            (name == "material_index" || name == "geometry_index" ||
             name == "geometry_kind")) {
            throw std::runtime_error(
                "pelican.ray_record_name_reserved@1: authored record member '" +
                name + "' is reserved for the engine");
        }
        if (!member_names.emplace(name).second) {
            layoutError(source,
                        "record contains duplicate member '" + name + "'");
        }
        const auto facts = typeFacts(encoded.at("type").get_ref<const std::string &>(),
                                     source);
        auto member = appendRecordMember(std::move(name), facts, cursor);
        members.push_back(std::move(member));
    }
    return members;
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

std::pair<std::vector<std::string>, std::vector<RayGroupType>>
parseTypedGroupObjects(const json &value, std::string_view source) {
    if (!value.is_array() || value.empty()) {
        layoutError(source, "groups must be a non-empty array");
    }
    std::vector<std::string> names;
    std::vector<RayGroupType> types;
    std::set<std::string> unique;
    for (const auto &entry : value) {
        requireObjectFields(entry, {"name", "type"}, "groups", source);
        if (!entry.contains("name") || !entry.at("name").is_string() ||
            !validName(entry.at("name").get_ref<const std::string &>()) ||
            !entry.contains("type") || !entry.at("type").is_string()) {
            layoutError(source, "groups require valid name and type");
        }
        auto name = entry.at("name").get<std::string>();
        if (!unique.emplace(name).second) {
            layoutError(source, "groups contains duplicate name '" + name +
                                    "'");
        }
        const auto &type = entry.at("type").get_ref<const std::string &>();
        if (type == "triangles") {
            types.push_back(RayGroupType::triangles);
        } else if (type == "procedural") {
            types.push_back(RayGroupType::procedural);
        } else {
            layoutError(source,
                        "group type must be 'triangles' or 'procedural'");
        }
        names.push_back(std::move(name));
    }
    return {std::move(names), std::move(types)};
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
    const json &record, std::span<const RayRecordMemberLayout> members,
    std::uint32_t data_size,
    std::string_view source) {
    if (!record.is_object() || record.size() != members.size()) {
        bindingRecordError(source,
                           "record member set does not match layout");
    }
    for (auto field = record.begin(); field != record.end(); ++field) {
        if (std::none_of(members.begin(), members.end(),
                         [&](const auto &member) { return member.name == field.key(); })) {
            bindingRecordError(source, "unknown record member '" + field.key() + "'");
        }
    }
    std::vector<std::byte> result(data_size, std::byte{0});
    for (const auto &member : members) {
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

std::vector<std::byte> encodeRecord(
    const json &record, const RayGroupLayout &layout,
    std::string_view source) {
    return encodeRecord(record, layout.record, layout.data_size, source);
}

std::string materialReference(const MaterialRowIdentity &identity,
                              bool table_key) {
    if (const auto *document =
            std::get_if<MaterialRowDocumentIdentity>(&identity)) {
        auto result = "document:" + document->document;
        if (table_key) result += "#" + document->material_name;
        return result;
    }
    if (const auto *asset = std::get_if<MaterialRowAssetIdentity>(&identity)) {
        return "asset:" + asset->asset + "#" +
               std::to_string(asset->source_material_index);
    }
    return "unavailable";
}

bool selectorMatches(
    const RayBindingSelector &selector, const MaterialRowIdentity &identity,
    std::string_view procedural_asset) {
    if (selector.kind == RayBindingSelector::Kind::procedural) {
        return !procedural_asset.empty() &&
               selector.reference == procedural_asset;
    }
    if (selector.kind == RayBindingSelector::Kind::document) {
        const auto *document =
            std::get_if<MaterialRowDocumentIdentity>(&identity);
        return document != nullptr && selector.reference == document->document;
    }
    const auto *asset = std::get_if<MaterialRowAssetIdentity>(&identity);
    return asset != nullptr && selector.reference == asset->asset &&
           selector.source_material_index == asset->source_material_index;
}

[[noreturn]] void rayAsError(std::string_view code,
                             std::string_view source,
                             std::string message) {
    throw std::runtime_error(
        "pelican." + std::string{code} + "@1: " + std::move(message) +
        (source.empty() ? std::string{}
                        : " in '" + std::string{source} + "'"));
}

std::uint32_t parseRayFlags(const json &encoded, bool geometry,
                            std::string_view source) {
    if (!encoded.is_array()) {
        rayAsError("ray_as_rule_malformed", source,
                   geometry ? "geometry_flags must be a string array"
                            : "instance_flags must be a string array");
    }
    std::uint32_t result = 0;
    std::set<std::string> seen;
    for (const auto &item : encoded) {
        if (!item.is_string()) {
            rayAsError("ray_as_rule_malformed", source,
                       "flag values must be strings");
        }
        const auto &name = item.get_ref<const std::string &>();
        if (!seen.emplace(name).second) {
            rayAsError("ray_as_rule_malformed", source,
                       "duplicate flag '" + name + "'");
        }
        if (geometry) {
            if (name == "opaque") {
                result |= rayGeometryOpaque;
            } else if (name == "no_duplicate_any_hit_invocation") {
                result |= rayGeometryNoDuplicateAnyHitInvocation;
            } else {
                rayAsError("ray_as_unknown_flag", source,
                           "unknown geometry flag '" + name + "'");
            }
        } else if (name == "triangle_facing_cull_disable") {
            result |= rayInstanceTriangleFacingCullDisable;
        } else if (name == "triangle_flip_facing") {
            result |= rayInstanceTriangleFlipFacing;
        } else if (name == "force_opaque") {
            result |= rayInstanceForceOpaque;
        } else if (name == "force_no_opaque") {
            result |= rayInstanceForceNoOpaque;
        } else {
            rayAsError("ray_as_unknown_flag", source,
                       "unknown instance flag '" + name + "'");
        }
    }
    if (!geometry && (result & rayInstanceForceOpaque) != 0 &&
        (result & rayInstanceForceNoOpaque) != 0) {
        rayAsError("ray_as_conflicting_instance_flags", source,
                   "force_opaque and force_no_opaque are mutually exclusive");
    }
    return result;
}

std::uint8_t parseRayMask(const json &encoded, std::string_view source) {
    std::uint64_t value = 0;
    if (encoded.is_number_unsigned()) {
        value = encoded.get<std::uint64_t>();
    } else if (encoded.is_number_integer() &&
               encoded.get<std::int64_t>() >= 0) {
        value = static_cast<std::uint64_t>(encoded.get<std::int64_t>());
    } else {
        rayAsError("ray_as_rule_malformed", source,
                   "defaults.mask must be an unsigned integer");
    }
    if (value > 0xffu) {
        rayAsError("ray_as_field_range", source,
                   "defaults.mask exceeds 8 bits");
    }
    return static_cast<std::uint8_t>(value);
}

RayAccelerationStructureDeclaration parseAccelerationStructure(
    const json &encoded, std::string_view source) {
    requireObjectFields(encoded, {"defaults", "procedural_defaults", "rules", "triangle_space", "procedural_space"},
                        "acceleration_structure", source);
    if (!encoded.contains("defaults")) {
        rayAsError("ray_as_rule_malformed", source,
                   "acceleration_structure.defaults is required");
    }
    const auto &defaults = encoded.at("defaults");
    requireObjectFields(defaults,
                        {"mask", "instance_flags", "geometry_flags"},
                        "acceleration_structure.defaults", source);
    RayAccelerationStructureDeclaration result;
    if (const auto space = encoded.find("procedural_space"); space != encoded.end()) {
        if (!space->is_string() || (*space != "geometry" && *space != "world"))
            rayAsError("ray_as_rule_malformed", source,
                       "acceleration_structure.procedural_space must be geometry or world");
        result.procedural_world_space = *space == "world";
    }
    if (const auto space = encoded.find("triangle_space"); space != encoded.end()) {
        if (!space->is_string())
            rayAsError("ray_as_rule_malformed", source,
                       "acceleration_structure.triangle_space must be geometry or world");
        const auto &name = space->get_ref<const std::string &>();
        if (name != "geometry" && name != "world")
            rayAsError("ray_as_rule_malformed", source,
                       "acceleration_structure.triangle_space must be geometry or world");
        result.triangle_world_space = name == "world";
    }
    if (const auto mask = defaults.find("mask"); mask != defaults.end()) {
        result.defaults.mask = parseRayMask(*mask, source);
    }
    if (const auto flags = defaults.find("instance_flags");
        flags != defaults.end()) {
        result.defaults.instance_flags = parseRayFlags(*flags, false, source);
    }
    if (const auto flags = defaults.find("geometry_flags");
        flags != defaults.end()) {
        result.defaults.geometry_flags = parseRayFlags(*flags, true, source);
    }
    if (const auto procedural = encoded.find("procedural_defaults");
        procedural != encoded.end()) {
        requireObjectFields(*procedural, {"geometry_flags"},
                            "acceleration_structure.procedural_defaults",
                            source);
        if (const auto flags = procedural->find("geometry_flags");
            flags != procedural->end()) {
            result.procedural_defaults.geometry_flags =
                parseRayFlags(*flags, true, source);
        }
    }
    const auto rules = encoded.find("rules");
    if (rules == encoded.end()) return result;
    if (!rules->is_array()) {
        rayAsError("ray_as_rule_malformed", source,
                   "acceleration_structure.rules must be an array");
    }
    std::set<std::string> exact_keys;
    for (const auto &rule : *rules) {
        if (!rule.is_object()) {
            rayAsError("ray_as_rule_malformed", source,
                       "each AS rule must be an object");
        }
        for (auto field = rule.begin(); field != rule.end(); ++field) {
            if (field.key() != "material" &&
                field.key() != "geometry_flags") {
                rayAsError("ray_as_rule_malformed", source,
                           "AS rule has unknown field '" + field.key() + "'");
            }
        }
        if (!rule.contains("material") ||
            !rule.contains("geometry_flags") ||
            !rule.at("material").is_object()) {
            rayAsError("ray_as_rule_malformed", source,
                       "AS rule requires material and geometry_flags");
        }
        const auto &material = rule.at("material");
        for (auto field = material.begin(); field != material.end(); ++field) {
            if (field.key() != "document" &&
                field.key() != "material_name" &&
                field.key() != "asset" &&
                field.key() != "source_material_index") {
                rayAsError("ray_as_rule_malformed", source,
                           "AS material selector has unknown field '" +
                               field.key() + "'");
            }
        }
        const bool has_document = material.contains("document");
        const bool has_name = material.contains("material_name");
        const bool has_asset = material.contains("asset");
        const bool has_index = material.contains("source_material_index");
        const bool document_shape = has_document && !has_asset && !has_index;
        const bool asset_shape = has_asset && has_index &&
                                 !has_document && !has_name;
        if (!document_shape && !asset_shape) {
            rayAsError("ray_as_rule_malformed", source,
                       "material selector must be document with optional material_name or asset+source_material_index");
        }
        RayAsMaterialSelector selector;
        if (document_shape) {
            if (!material.at("document").is_string() ||
                material.at("document").get_ref<const std::string &>().empty() ||
                (has_name &&
                 (!material.at("material_name").is_string() ||
                  material.at("material_name")
                      .get_ref<const std::string &>()
                      .empty()))) {
                rayAsError("ray_as_rule_malformed", source,
                           "document selector strings must be non-empty when present");
            }
            selector.kind = RayAsMaterialSelector::Kind::document_material;
            selector.reference = material.at("document").get<std::string>();
            if (has_name) {
                selector.material_name =
                    material.at("material_name").get<std::string>();
            }
        } else {
            if (!material.at("asset").is_string() ||
                material.at("asset").get_ref<const std::string &>().empty()) {
                rayAsError("ray_as_rule_malformed", source,
                           "asset selector reference must be non-empty");
            }
            std::uint64_t index = 0;
            const auto &value = material.at("source_material_index");
            if (value.is_number_unsigned()) {
                index = value.get<std::uint64_t>();
            } else if (value.is_number_integer() &&
                       value.get<std::int64_t>() >= 0) {
                index = static_cast<std::uint64_t>(value.get<std::int64_t>());
            } else {
                rayAsError("ray_as_rule_malformed", source,
                           "source_material_index must be uint32");
            }
            if (index > std::numeric_limits<std::uint32_t>::max()) {
                rayAsError("ray_as_rule_malformed", source,
                           "source_material_index exceeds uint32");
            }
            selector.kind = RayAsMaterialSelector::Kind::asset_material;
            selector.reference = material.at("asset").get<std::string>();
            selector.source_material_index =
                static_cast<std::uint32_t>(index);
        }
        const auto key = selector.canonicalKey();
        if (!exact_keys.emplace(key).second) {
            rayAsError("ray_as_duplicate_rule", source,
                       "duplicate material selector '" + key + "'");
        }
        result.rules.push_back(
            {.material = std::move(selector),
             .geometry_flags =
                 parseRayFlags(rule.at("geometry_flags"), true, source)});
    }
    return result;
}

bool rayAsSelectorMatches(const RayAsMaterialSelector &selector,
                          const MaterialRowIdentity &identity) {
    if (selector.kind ==
        RayAsMaterialSelector::Kind::document_material) {
        const auto *document =
            std::get_if<MaterialRowDocumentIdentity>(&identity);
        return document != nullptr && selector.reference == document->document &&
               (selector.material_name.empty() ||
                selector.material_name == document->material_name);
    }
    const auto *asset = std::get_if<MaterialRowAssetIdentity>(&identity);
    return asset != nullptr && selector.reference == asset->asset &&
           selector.source_material_index == asset->source_material_index;
}

} // namespace

std::string RayAsMaterialSelector::canonicalKey() const {
    if (kind == Kind::document_material) {
        return "document:" + reference +
               (material_name.empty() ? "" : "#" + material_name);
    }
    return "asset:" + reference + "#" +
           std::to_string(source_material_index);
}

void RayLayoutRuntimeState::configure(RayLayoutSemanticMode mode) {
    if (configured_) {
        validate(mode);
        return;
    }
    configured_ = true;
    mode_ = mode;
}

void RayLayoutRuntimeState::validate(RayLayoutSemanticMode mode) const {
    if (configured_ && mode_ != mode) {
        throw std::runtime_error(
            "pelican.ray_layout_mode_change_requires_restart@1: ray layout semantic mode cannot change during a process lifetime");
    }
}

RayLayoutSemanticMode rayLayoutSemanticMode(
    const RayTracingProjectConfig *config) noexcept {
    return config == nullptr ? RayLayoutSemanticMode::LegacyNoLayout
                             : config->layout.semantic_mode;
}

std::uint32_t RayGroupLayout::rayTypeIndex(std::string_view name) const {
    return findIndex(ray_types, name, "ray type");
}

std::uint32_t RayGroupLayout::groupIndex(std::string_view name) const {
    return findIndex(groups, name, "group");
}

std::uint32_t RayGroupLayout::missIndex(std::string_view name) const {
    return findIndex(misses, name, "miss");
}

std::optional<std::uint32_t> RayGroupLayout::engineRecordOffset(
    RayEngineRecordField field) const noexcept {
    if (std::find(engine_record.begin(), engine_record.end(), field) == engine_record.end())
        return std::nullopt;
    const auto name = field == RayEngineRecordField::procedural_transform
                          ? std::string_view{"geometry_to_world_0"} : engineRecordFieldName(field);
    const auto found = std::find_if(
        physical_record.begin(), physical_record.end(),
        [name](const auto &member) { return member.name == name; });
    if (found == physical_record.end()) return std::nullopt;
    return found->offset;
}

RayCallableRecordLayout parseRayCallableRecordLayout(
    const nlohmann::json &members, std::string_view diagnostic) {
    std::uint32_t cursor = 0;
    RayCallableRecordLayout result;
    result.members = parseRecordMembers(members, cursor, false, diagnostic);
    std::uint32_t alignment = 1;
    for (const auto &member : result.members)
        alignment = std::max(alignment, member.alignment);
    result.data_size = alignUp(cursor, alignment, "pelican.ray_group_layout_invalid@1");
    return result;
}

std::vector<std::byte> encodeRayCallableRecord(
    const nlohmann::json &values, const RayCallableRecordLayout &layout,
    std::string_view diagnostic) {
    return encodeRecord(values, layout.members, layout.data_size, diagnostic);
}

std::vector<std::vector<std::byte>> encodeRayCallableRecords(
    const nlohmann::json &records, const RayCallableRecordLayout &layout,
    std::size_t callable_count, std::string_view diagnostic) {
    if (!records.is_array() || records.size() != callable_count)
        throw std::runtime_error("pelican.ray_tracing.callable_records_invalid@1: task '" +
            std::string{diagnostic} + "' callable_records must contain one record per callable");
    std::vector<std::vector<std::byte>> result;
    result.reserve(records.size());
    for (const auto &record : records)
        result.push_back(encodeRayCallableRecord(record, layout, diagnostic));
    return result;
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
                         "groups", "misses", "engine_record", "record",
                         "portable_profile", "acceleration_structure"},
                        "layout", source_reference);
    if (!root.contains("schema") || root.at("schema") != "pelican.ray_group_layout" ||
        !root.contains("version") || !root.at("version").is_number_integer() ||
        (root.at("version") != 1 && root.at("version") != 2 &&
         root.at("version") != 3)) {
        layoutError(source_reference,
                    "schema must be pelican.ray_group_layout version 1, 2, or 3");
    }
    const auto version = root.at("version").get<std::uint32_t>();
    if (version < 3 && root.contains("acceleration_structure") &&
        root.at("acceleration_structure").contains("procedural_space"))
        layoutError(source_reference, "acceleration_structure.procedural_space requires layout version 3");
    if (version == 1 && root.contains("acceleration_structure")) {
        rayAsError("ray_as_declaration_requires_layout_v2", source_reference,
                   "acceleration_structure requires layout version 2");
    }
    if (version < 3 && root.contains("acceleration_structure") &&
        root.at("acceleration_structure").is_object() &&
        root.at("acceleration_structure").contains(
            "procedural_defaults")) {
        throw std::runtime_error(
            "pelican.ray_group_type_requires_layout_v3@1: procedural_defaults requires layout version 3");
    }
    for (const auto required : {"ray_types", "default_group", "groups", "misses", "record"}) {
        if (!root.contains(required)) {
            layoutError(source_reference, "missing field '" + std::string{required} + "'");
        }
    }
    RayGroupLayout result;
    result.source_reference = std::move(source_reference);
    result.version = version;
    result.semantic_mode = version == 1
                               ? RayLayoutSemanticMode::LayoutV1Q
                               : version == 2
                                     ? RayLayoutSemanticMode::LayoutV2
                                     : RayLayoutSemanticMode::LayoutV3;
    if (version == 1) {
        // Preserve the exact v1 ABI/digest behavior byte-for-byte.
        result.digest_sha256 =
            picosha2::hash256_hex_string(raw_bytes.begin(), raw_bytes.end());
    } else {
        // AS declarations are not shader/SBT inputs.  Hashing the layout with
        // that member removed makes the separation structural.
        auto sbt_root = root;
        sbt_root.erase("acceleration_structure");
        const auto sbt_bytes = sbt_root.dump();
        result.digest_sha256 = picosha2::hash256_hex_string(
            sbt_bytes.begin(), sbt_bytes.end());
        if (const auto acceleration = root.find("acceleration_structure");
            acceleration != root.end()) {
            result.acceleration_structure = parseAccelerationStructure(
                *acceleration, result.source_reference);
            const auto as_bytes = acceleration->dump();
            result.acceleration_structure_digest_sha256 =
                picosha2::hash256_hex_string(as_bytes.begin(),
                                             as_bytes.end());
        } else {
            constexpr std::string_view default_as = "layout-v2-default-as";
            result.acceleration_structure_digest_sha256 =
                picosha2::hash256_hex_string(default_as.begin(),
                                             default_as.end());
        }
    }
    if (const auto profile = root.find("portable_profile");
        profile != root.end()) {
        if (!profile->is_string() ||
            profile->get_ref<const std::string &>() !=
                "material_alpha_v1") {
            layoutError(
                result.source_reference,
                "portable_profile must be 'material_alpha_v1' when present");
        }
        result.portable_profile =
            RayPortableProfile::material_alpha_v1;
    }
    result.ray_types = parseNameStrings(root.at("ray_types"), "ray_types", result.source_reference);
    if (version == 3) {
        auto [names, types] =
            parseTypedGroupObjects(root.at("groups"), result.source_reference);
        result.groups = std::move(names);
        result.group_types = std::move(types);
    } else {
        result.groups = parseNamedObjects(root.at("groups"), "groups",
                                          result.source_reference);
        result.group_types.assign(result.groups.size(),
                                  RayGroupType::triangles);
    }
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
    std::set<RayEngineRecordField> requested_engine_fields;
    if (const auto engine = root.find("engine_record");
        engine != root.end()) {
        if (!engine->is_array() || engine->empty()) {
            layoutError(result.source_reference,
                        "engine_record must be a non-empty array when present");
        }
        for (const auto &encoded : *engine) {
            if (!encoded.is_string()) {
                throw std::runtime_error(
                    "pelican.ray_engine_record_unknown@1: engine record field must be a string");
            }
            const auto &name = encoded.get_ref<const std::string &>();
            std::optional<RayEngineRecordField> field;
            if (name == "material_index") {
                field = RayEngineRecordField::material_index;
            } else if (name == "geometry_index") {
                field = RayEngineRecordField::geometry_index;
            } else if (name == "geometry_kind") {
                field = RayEngineRecordField::geometry_kind;
            } else if (name == "procedural_transform") {
                if (version < 3) layoutError(result.source_reference, "procedural_transform requires layout version 3");
                field = RayEngineRecordField::procedural_transform;
            } else {
                throw std::runtime_error(
                    "pelican.ray_engine_record_unknown@1: unknown engine record field '" +
                    name + "'");
            }
            if (!requested_engine_fields.emplace(*field).second) {
                throw std::runtime_error(
                    "pelican.ray_engine_record_duplicate@1: duplicate engine record field '" +
                    name + "'");
            }
        }
    }
    for (const auto field : {RayEngineRecordField::material_index,
                             RayEngineRecordField::geometry_index,
                             RayEngineRecordField::geometry_kind,
                             RayEngineRecordField::procedural_transform}) {
        if (requested_engine_fields.contains(field)) {
            result.engine_record.push_back(field);
        }
    }
    if (result.usesMaterialAlphaProfile() &&
        requested_engine_fields !=
            std::set<RayEngineRecordField>{
                RayEngineRecordField::material_index,
                RayEngineRecordField::geometry_index}) {
        throw std::runtime_error(
            "pelican.ray_profile_engine_record_required@1: portable_profile 'material_alpha_v1' requires engine_record material_index and geometry_index" +
            (result.source_reference.empty()
                 ? std::string{}
                 : " in '" + result.source_reference + "'"));
    }

    const auto &record = root.at("record");
    if (!record.is_array()) {
        layoutError(result.source_reference, "record must be an array");
    }
    if (result.acceleration_structure && result.acceleration_structure->procedural_world_space &&
        !requested_engine_fields.contains(RayEngineRecordField::procedural_transform))
        layoutError(result.source_reference, "procedural_space world requires engine_record procedural_transform");
    std::uint32_t cursor = 0;
    const TypeFacts engine_facts{RayRecordType::u32, 4, 4, 1};
    for (const auto field : result.engine_record) {
        if (field == RayEngineRecordField::procedural_transform) {
            for (const auto *prefix : {"geometry_to_world_", "world_to_geometry_"})
                for (unsigned row = 0; row < 3; ++row)
                    result.physical_record.push_back(appendRecordMember(
                        std::string{prefix} + std::to_string(row), typeFacts("vec4", result.source_reference), cursor));
            continue;
        }
        result.physical_record.push_back(appendRecordMember(
            std::string{engineRecordFieldName(field)}, engine_facts,
            cursor));
    }
    result.record = parseRecordMembers(record, cursor, true, result.source_reference);
    for (const auto &member : result.record)
        if (std::ranges::any_of(result.physical_record, [&](const auto &engine) { return engine.name == member.name; }))
            layoutError(result.source_reference, "record name conflicts with engine field: " + member.name);
    result.physical_record.insert(result.physical_record.end(),
                                  result.record.begin(), result.record.end());
    // A struct's std430 base alignment is the maximum member alignment; the
    // final size is rounded to that alignment. Empty records have size zero.
    std::uint32_t struct_alignment = 1;
    for (const auto &member : result.physical_record) {
        struct_alignment = std::max(struct_alignment, member.alignment);
    }
    result.data_size = result.physical_record.empty()
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
    for (const auto &member : layout.physical_record) {
        members.push_back({{"name", member.name},
                           {"type", rayRecordTypeName(member.type)},
                           {"offset", member.offset},
                           {"size", member.size},
                           {"alignment", member.alignment}});
    }
    json result{{"schema", "pelican.ray_record_abi"},
                {"version", 1},
                {"data_size", layout.data_size},
                {"members", std::move(members)}};
    if (layout.usesRayTables()) {
        json engine = json::array();
        for (const auto field : layout.engine_record) {
            engine.push_back(engineRecordFieldName(field));
        }
        result["engine_record"] = std::move(engine);
        // Array stride of the single physical std430 struct.
        result["stride"] = layout.data_size;
    }
    return result;
}

std::string makeRayGroupLayoutVirtualInclude(const RayGroupLayout &layout) {
    std::ostringstream source;
    source << "// pelican.ray_group_layout sha256=" << layout.digest_sha256 << '\n';
    source << "const uint pelican_ray_type_count = "
           << layout.ray_types.size() << "u;\n";
    for (std::size_t index = 0; index < layout.ray_types.size(); ++index) {
        source << "#define PELICAN_RAY_TYPE_" << layout.ray_types[index]
               << ' ' << index << '\n';
    }
    for (std::size_t index = 0; index < layout.misses.size(); ++index) {
        source << "#define PELICAN_MISS_" << layout.misses[index]
               << ' ' << index << '\n';
    }
    if (!layout.usesRayTables()) return source.str();

    source << "#extension GL_EXT_nonuniform_qualifier : require\n";
    source << "#extension GL_EXT_buffer_reference2 : require\n";
    source << "#define PELICAN_RAY_ENGINE_RECORD_MEMBERS \\\n";
    const auto engine_member_count = layout.physical_record.size() - layout.record.size();
    for (std::size_t index = 0; index < engine_member_count; ++index) {
        source << "    " << glslRecordTypeName(layout.physical_record[index].type) << ' '
               << layout.physical_record[index].name << ';';
        if (index + 1 != engine_member_count) source << " \\\n";
        else source << '\n';
    }
    if (layout.usesMaterialAlphaProfile()) {
        source << "#define PELICAN_RAY_PROFILE_RECORD_MEMBERS \\\n";
        for (std::size_t index = 0;
             index < layout.physical_record.size(); ++index) {
            source << "    "
                   << glslRecordTypeName(
                          layout.physical_record[index].type)
                   << ' ' << layout.physical_record[index].name << ';';
            if (index + 1 != layout.physical_record.size())
                source << " \\\n";
            else
                source << '\n';
        }
    }
    if (layout.engineRecordOffset(RayEngineRecordField::procedural_transform)) {
        source << R"glsl(
#define PELICAN_RAY_HAS_PROCEDURAL_TRANSFORM 1
#define pelicanProceduralGeometryPosition(record, position) vec3(dot((record).world_to_geometry_0, vec4((position), 1.0)), dot((record).world_to_geometry_1, vec4((position), 1.0)), dot((record).world_to_geometry_2, vec4((position), 1.0)))
#define pelicanProceduralGeometryDirection(record, direction) vec3(dot((record).world_to_geometry_0.xyz, (direction)), dot((record).world_to_geometry_1.xyz, (direction)), dot((record).world_to_geometry_2.xyz, (direction)))
#define pelicanProceduralRayOrigin(record) pelicanProceduralGeometryPosition(record, gl_WorldRayOriginEXT)
#define pelicanProceduralRayDirection(record) pelicanProceduralGeometryDirection(record, gl_WorldRayDirectionEXT)
#define pelicanProceduralWorldPosition(record, position) vec3(dot((record).geometry_to_world_0, vec4((position), 1.0)), dot((record).geometry_to_world_1, vec4((position), 1.0)), dot((record).geometry_to_world_2, vec4((position), 1.0)))
#define pelicanProceduralWorldNormal(record, normal) normalize(mat3((record).world_to_geometry_0.xyz, (record).world_to_geometry_1.xyz, (record).world_to_geometry_2.xyz) * (normal))
)glsl";
    }
    source << "const uint pelican_ray_missing_texture = 0xffffffffu;\n";
    source << "const uint pelican_ray_attribute_normal = 1u;\n";
    source << "const uint pelican_ray_attribute_uv0 = 2u;\n";
    source << "const uint pelican_ray_attribute_color0 = 4u;\n";
    if (layout.usesMaterialAlphaProfile()) {
        source << R"glsl(
struct PelicanRayMaterialRow {
    uint alpha_mode;
    float cutoff;
    float base_factor_a;
    uint texture_index;
    vec4 uv_offset_scale;
    float uv_rotation;
    uint uses_vertex_color;
    uvec2 reserved;
};)glsl";
    } else {
        source << R"glsl(
struct PelicanRayMaterialRow {
    vec4 base_color_factor;
    float alpha_cutoff;
    uint alpha_mode;
    uint base_color_texture_index;
    uint reserved;
};)glsl";
    }
    source << R"glsl(
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PelicanRayVertexWords {
    uint words[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PelicanRayIndexWords {
    uint words[];
};
struct PelicanRayGeometryRow {
    PelicanRayVertexWords vertices;
    PelicanRayIndexWords indices;
    uint vertex_stride;
    uint index_type;
    uint attribute_presence;
    uint reserved;
};
layout(set = 3, binding = 0, std430) readonly buffer PelicanRayMaterialTable {
    PelicanRayMaterialRow pelican_ray_material_rows[];
};
layout(set = 3, binding = 1) uniform sampler2D pelican_ray_material_textures[1024];
layout(set = 3, binding = 2, std430) readonly buffer PelicanRayGeometryTable {
    PelicanRayGeometryRow pelican_ray_geometry_rows[];
};
PelicanRayMaterialRow pelicanRayMaterial(uint material_index) {
    return pelican_ray_material_rows[material_index];
})glsl";
    if (layout.usesMaterialAlphaProfile()) {
        source << R"glsl(
vec4 pelicanRaySampleBaseColor(uint material_index, vec2 uv) {
    PelicanRayMaterialRow row = pelicanRayMaterial(material_index);
    float texel_alpha = row.texture_index == pelican_ray_missing_texture
        ? 1.0
        : textureLod(pelican_ray_material_textures[nonuniformEXT(row.texture_index)], uv, 0.0).a;
    return vec4(1.0, 1.0, 1.0, row.base_factor_a * texel_alpha);
})glsl";
    } else {
        source << R"glsl(
vec4 pelicanRaySampleBaseColor(uint material_index, vec2 uv) {
    PelicanRayMaterialRow row = pelicanRayMaterial(material_index);
    vec4 texel = row.base_color_texture_index == pelican_ray_missing_texture
        ? vec4(1.0)
        : texture(pelican_ray_material_textures[nonuniformEXT(row.base_color_texture_index)], uv);
    return row.base_color_factor * texel;
})glsl";
    }
    source << R"glsl(
uvec3 pelicanRayTriangleVertexIndices(PelicanRayGeometryRow row, uint primitive_id) {
    uint first = primitive_id * 3u;
    return uvec3(row.indices.words[first], row.indices.words[first + 1u],
                 row.indices.words[first + 2u]);
}
vec3 pelicanRayBarycentricWeights(vec2 barycentric) {
    return vec3(1.0 - barycentric.x - barycentric.y,
                barycentric.x, barycentric.y);
}
vec2 pelicanRayVertexUV(PelicanRayGeometryRow row, uint vertex_index) {
    uint first = vertex_index * (row.vertex_stride / 4u) + 10u;
    return vec2(uintBitsToFloat(row.vertices.words[first]),
                uintBitsToFloat(row.vertices.words[first + 1u]));
}
vec3 pelicanRayVertexNormal(PelicanRayGeometryRow row, uint vertex_index) {
    uint first = vertex_index * (row.vertex_stride / 4u) + 3u;
    return vec3(uintBitsToFloat(row.vertices.words[first]),
                uintBitsToFloat(row.vertices.words[first + 1u]),
                uintBitsToFloat(row.vertices.words[first + 2u]));
}
vec4 pelicanRayVertexColor0(PelicanRayGeometryRow row, uint vertex_index) {
    uint first = vertex_index * (row.vertex_stride / 4u) + 12u;
    return vec4(uintBitsToFloat(row.vertices.words[first]),
                uintBitsToFloat(row.vertices.words[first + 1u]),
                uintBitsToFloat(row.vertices.words[first + 2u]),
                uintBitsToFloat(row.vertices.words[first + 3u]));
}
vec2 pelicanRayFetchUV(uint geometry_index, uint primitive_id, vec2 barycentric) {
    PelicanRayGeometryRow row = pelican_ray_geometry_rows[geometry_index];
    uvec3 vertex = pelicanRayTriangleVertexIndices(row, primitive_id);
    vec3 weight = pelicanRayBarycentricWeights(barycentric);
    return pelicanRayVertexUV(row, vertex.x) * weight.x +
           pelicanRayVertexUV(row, vertex.y) * weight.y +
           pelicanRayVertexUV(row, vertex.z) * weight.z;
}
vec3 pelicanRayFetchNormal(uint geometry_index, uint primitive_id, vec2 barycentric) {
    PelicanRayGeometryRow row = pelican_ray_geometry_rows[geometry_index];
    uvec3 vertex = pelicanRayTriangleVertexIndices(row, primitive_id);
    vec3 weight = pelicanRayBarycentricWeights(barycentric);
    return pelicanRayVertexNormal(row, vertex.x) * weight.x +
           pelicanRayVertexNormal(row, vertex.y) * weight.y +
           pelicanRayVertexNormal(row, vertex.z) * weight.z;
}
vec4 pelicanRayFetchColor0(uint geometry_index, uint primitive_id, vec2 barycentric) {
    PelicanRayGeometryRow row = pelican_ray_geometry_rows[geometry_index];
    uvec3 vertex = pelicanRayTriangleVertexIndices(row, primitive_id);
    vec3 weight = pelicanRayBarycentricWeights(barycentric);
    return pelicanRayVertexColor0(row, vertex.x) * weight.x +
           pelicanRayVertexColor0(row, vertex.y) * weight.y +
           pelicanRayVertexColor0(row, vertex.z) * weight.z;
}
)glsl";
    return source.str();
}

std::string makeMaterialAlphaAnyHitSource(
    const RayGroupLayout &layout) {
    if (!layout.usesMaterialAlphaProfile()) {
        throw std::logic_error(
            "material alpha any-hit source requested without an active portable profile");
    }
    if (!layout.engineRecordOffset(
            RayEngineRecordField::material_index) ||
        !layout.engineRecordOffset(
            RayEngineRecordField::geometry_index)) {
        throw std::logic_error(
            "material alpha profile lost its material/geometry record fields");
    }
    // Deliberately has no rayPayloadInEXT declaration. The RT validator has
    // one narrowly-scoped exception for this engine-generated any-hit stage.
    return R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
hitAttributeEXT vec2 pelican_alpha_hit_attribute;
layout(shaderRecordEXT, std430) buffer PelicanAlphaRecord {
    PELICAN_RAY_PROFILE_RECORD_MEMBERS
} pelican_alpha_record;
void main() {
    PelicanRayMaterialRow row =
        pelicanRayMaterial(pelican_alpha_record.material_index);
    if (row.alpha_mode != 1u) {
        return;
    }
    vec2 uv = vec2(0.0);
    if (row.texture_index != pelican_ray_missing_texture) {
        uv = pelicanRayFetchUV(pelican_alpha_record.geometry_index,
                               gl_PrimitiveID,
                               pelican_alpha_hit_attribute);
        float cosine = cos(row.uv_rotation);
        float sine = sin(row.uv_rotation);
        mat2 rotation = mat2(cosine, sine, -sine, cosine);
        uv = row.uv_offset_scale.xy +
             rotation * (uv * row.uv_offset_scale.zw);
    }
    float texture_alpha =
        row.texture_index == pelican_ray_missing_texture
            ? 1.0
            : textureLod(
                  pelican_ray_material_textures[
                      nonuniformEXT(row.texture_index)],
                  uv, 0.0).a;
    float vertex_alpha = 1.0;
    if (row.uses_vertex_color != 0u) {
        vertex_alpha = pelicanRayFetchColor0(
            pelican_alpha_record.geometry_index,
            gl_PrimitiveID,
            pelican_alpha_hit_attribute).a;
    }
    float opacity = clamp(
        texture_alpha * row.base_factor_a * vertex_alpha,
        0.0, 1.0);
    float cutoff = clamp(row.cutoff, 0.0, 1.0);
    if (opacity < cutoff) {
        ignoreIntersectionEXT;
    }
}
)glsl";
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
        (root.at("version") != 1 && root.at("version") != 2) ||
        !root.contains("bindings") ||
        !root.at("bindings").is_array()) {
        throw std::runtime_error(
            "pelican.ray_bindings_invalid@1: schema must be pelican.ray_bindings version 1 or 2 with bindings array");
    }
    const auto version = root.at("version").get<std::uint32_t>();
    RayBindings result;
    result.source_reference = std::move(source_reference);
    result.version = version;
    result.digest_sha256 = picosha2::hash256_hex_string(raw_bytes.begin(), raw_bytes.end());
    std::set<std::string, std::less<>> procedural_selectors;
    for (const auto &entry : root.at("bindings")) {
        if (!entry.is_object()) {
            throw std::runtime_error("pelican.ray_bindings_invalid@1: binding must be an object");
        }
        for (auto field = entry.begin(); field != entry.end(); ++field) {
            if (field.key() != "material" && field.key() != "procedural" &&
                field.key() != "group" &&
                field.key() != "record") {
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: binding has unknown field '" + field.key() + "'");
            }
        }
        const auto has_material = entry.contains("material");
        const auto has_procedural = entry.contains("procedural");
        if (has_material == has_procedural) {
            throw std::runtime_error(
                "pelican.ray_binding_selector_exactly_one@1: binding requires exactly one of material or procedural");
        }
        if (!entry.contains("group") || !entry.at("group").is_string() ||
            entry.at("group").get_ref<const std::string &>().empty()) {
            throw std::runtime_error(
                "pelican.ray_bindings_invalid@1: binding requires non-empty group");
        }
        const auto group_index = layout.groupIndex(
            entry.at("group").get_ref<const std::string &>());
        if (has_procedural) {
            if (version != 2) {
                throw std::runtime_error(
                    "pelican.ray_binding_selector_exactly_one@1: procedural selectors require ray_bindings version 2");
            }
            if (entry.contains("record")) {
                throw std::runtime_error(
                    "pelican.procedural_binding_record_forbidden@1: procedural record data is owned by asset_data");
            }
            if (!entry.at("procedural").is_string() ||
                entry.at("procedural").get_ref<const std::string &>().empty()) {
                throw std::runtime_error(
                    "pelican.ray_bindings_invalid@1: procedural selector must be a non-empty asset name");
            }
            auto asset = entry.at("procedural").get<std::string>();
            if (!procedural_selectors.emplace(asset).second) {
                throw std::runtime_error(
                    "pelican.procedural_selector_duplicate@1: duplicate procedural selector '" +
                    asset + "'");
            }
            result.bindings.push_back({
                .selector = RayBindingSelector{
                    .kind = RayBindingSelector::Kind::procedural,
                    .reference = std::move(asset)},
                .group_index = group_index,
            });
            continue;
        }
        if (!entry.at("material").is_object() ||
            !entry.contains("record")) {
            throw std::runtime_error(
                "pelican.ray_bindings_invalid@1: material binding requires selector, group, and record");
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
             .group_index = group_index,
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
        if (const auto *runtime =
                FastModuleContainer::tryGet<RayLayoutRuntimeState>()) {
            runtime->validate(RayLayoutSemanticMode::LegacyNoLayout);
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
    if (const auto *runtime =
            FastModuleContainer::tryGet<RayLayoutRuntimeState>()) {
        runtime->validate(result->layout.semantic_mode);
    }
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
    const auto semantic_mode = layout.semantic_mode;
    const auto as_defaults = layout.acceleration_structure
                                 ? layout.acceleration_structure->defaults
                                 : rayAccelerationStructureCanonicalDefaults;
    const std::span<const RayAsGeometryRule> as_rules =
        layout.acceleration_structure
            ? std::span<const RayAsGeometryRule>{
                  layout.acceleration_structure->rules}
            : std::span<const RayAsGeometryRule>{};
    std::vector<RaySceneRow> resolved;
    resolved.reserve(inputs.size());
    std::vector<bool> binding_matched(bindings ? bindings->bindings.size() : 0, false);
    for (const auto &input : inputs) {
        const auto identity = materialRowIdentity(input);
        if (input.geometry_kind == RayGeometryKind::aabbs &&
            semantic_mode != RayLayoutSemanticMode::LayoutV3) {
            throw std::runtime_error(
                "pelican.ray_group_type_requires_layout_v3@1: procedural rows require ray_group_layout version 3");
        }
        if (!requiresRayLayoutV2(semantic_mode) &&
            input.ray_instance) {
            throw std::runtime_error(
                "pelican.ray_instance_requires_layout_v2@1: pelican_ray_instance is only available with ray_group_layout version 2");
        }
        const RayMaterialBinding *chosen = nullptr;
        if (bindings != nullptr) {
            for (std::size_t index = 0; index < bindings->bindings.size(); ++index) {
                if (!selectorMatches(bindings->bindings[index].selector,
                                     identity, input.procedural_asset))
                    continue;
                binding_matched[index] = true;
                if (chosen != nullptr) {
                    throw std::runtime_error(
                        "pelican.ray_binding_selector_duplicate@1: row '" +
                        (input.geometry_kind == RayGeometryKind::aabbs
                             ? input.procedural_asset
                             : materialReference(identity,
                                                 layout.usesRayTables())) +
                        "' resolves more than once");
                }
                chosen = &bindings->bindings[index];
            }
        }
        auto instance_mask = as_defaults.mask;
        RaySceneSettingSources sources;
        if (layout.acceleration_structure) {
            sources.mask = sources.instance_flags = sources.custom_index = "layout_default";
            sources.geometry_flags = input.geometry_kind == RayGeometryKind::aabbs
                ? "procedural_default" : "layout_default";
        }
        auto instance_flags = as_defaults.instance_flags;
        auto instance_custom_index = as_defaults.custom_index;
        auto geometry_flags =
            input.geometry_kind == RayGeometryKind::aabbs &&
                    layout.acceleration_structure
                ? layout.acceleration_structure->procedural_defaults
                      .geometry_flags
                : as_defaults.geometry_flags;
        if (input.geometry_kind == RayGeometryKind::triangles &&
            requiresRayLayoutV2(semantic_mode) &&
            !as_rules.empty()) {
            if (std::holds_alternative<MaterialRowIdentityUnavailable>(identity)) {
                throw std::runtime_error(
                    "pelican.ray_as_row_material_identity_unavailable@1: AS rules require a document or asset/source_material_index row identity");
            }
            const RayAsGeometryRule *matched = nullptr;
            for (const auto &rule : as_rules) {
                if (!rayAsSelectorMatches(rule.material, identity)) continue;
                if (matched != nullptr) {
                    throw std::runtime_error(
                        "pelican.ray_as_rule_ambiguous@1: row material identity matches more than one AS rule");
                }
                matched = &rule;
            }
            if (matched != nullptr) {
                geometry_flags = matched->geometry_flags;
                sources.geometry_flags = "geometry_rule";
                sources.geometry_rule_index = static_cast<std::size_t>(matched - as_rules.data());
            }
        }
        if (requiresRayLayoutV2(semantic_mode) &&
            input.ray_instance) {
            if (input.ray_instance->mask) {
                instance_mask = *input.ray_instance->mask;
                sources.mask = "component";
            }
            if (input.ray_instance->instance_flags) {
                instance_flags = *input.ray_instance->instance_flags;
                sources.instance_flags = "component";
            }
            if (input.ray_instance->custom_index) {
                if (*input.ray_instance->custom_index > rayCustomIndexMaximum) {
                    throw std::runtime_error(
                        "pelican.ray_instance_field_range@1: custom_index exceeds 24 bits");
                }
                instance_custom_index =
                    *input.ray_instance->custom_index;
                sources.custom_index = "component";
            }
        }
        if ((instance_flags & rayInstanceForceOpaque) != 0 &&
            (instance_flags & rayInstanceForceNoOpaque) != 0) {
            throw std::runtime_error(
                "pelican.ray_as_conflicting_instance_flags@1: force_opaque and force_no_opaque are mutually exclusive");
        }
        if (layout.usesMaterialAlphaProfile() &&
            (instance_flags & rayInstanceForceOpaque) != 0) {
            throw std::runtime_error(
                "pelican.ray_profile_opaque_as@1: portable alpha profile forbids force_opaque AS instance flags");
        }
        const auto group_index =
            chosen ? chosen->group_index
                   : layout.groupIndex(layout.default_group);
        const auto required_group_type =
            input.geometry_kind == RayGeometryKind::aabbs
                ? RayGroupType::procedural
                : RayGroupType::triangles;
        if (group_index >= layout.group_types.size() ||
            layout.group_types[group_index] != required_group_type) {
            if (input.geometry_kind == RayGeometryKind::aabbs &&
                chosen == nullptr) {
                throw std::runtime_error(
                    "pelican.procedural_group_unresolved@1: procedural asset '" +
                    input.procedural_asset +
                    "' has no procedural selector or procedural default group");
            }
            throw std::runtime_error(
                "pelican.procedural_group_kind_conflict@1: row geometry kind does not match resolved layout group type");
        }
        resolved.push_back(
            {.setting_sources = sources,
             .binding_index = chosen ? std::optional<std::size_t>(chosen - bindings->bindings.data()) : std::nullopt,
             .semantic_mode = semantic_mode,
             .geometry_kind = input.geometry_kind,
             .source_index = input.source_index,
             .group_index = group_index,
             .record_data =
                 input.geometry_kind == RayGeometryKind::aabbs
                     ? encodeRecord(input.procedural_record, layout,
                                    input.procedural_asset)
                     : chosen
                           ? chosen->record_data
                           : std::vector<std::byte>(layout.data_size,
                                                   std::byte{0}),
             .scene_identity = input.scene_identity,
              .geometry_identity = input.geometry_identity,
              .geometry_match_identity = input.geometry_match_identity,
              .material_reference =
                  input.geometry_kind == RayGeometryKind::aabbs
                      ? std::string{}
                      : materialReference(identity,
                                          layout.usesRayTables()),
              .material_name = input.material_name,
              .resolved_material_handle =
                  input.resolved_material_handle,
              .resolved_material_generation =
                  input.resolved_material_generation,
              .resolved_material_lease =
                  input.resolved_material_lease,
              .geometry_allocation_id =
                  input.geometry_allocation_id,
              .authoring_object_id = input.authoring_object_id,
              .instance_mask = instance_mask,
              .instance_flags = instance_flags,
              .instance_custom_index = instance_custom_index,
              .geometry_flags = geometry_flags,
              .alpha_override_mask =
                  input.alpha_override_mask,
              .alpha_override_base_color_factor =
                  input.alpha_override_base_color_factor,
              .alpha_override_uv_offset_scale =
                  input.alpha_override_uv_offset_scale,
              .alpha_override_uv_rotation =
                  input.alpha_override_uv_rotation});
        if (const auto offset = layout.engineRecordOffset(RayEngineRecordField::procedural_transform)) {
            const auto forward = input.geometry_kind == RayGeometryKind::aabbs ? input.world_transform : glm::mat4{1.0F};
            for (unsigned column = 0; column < 4; ++column)
                for (unsigned row = 0; row < 4; ++row)
                    if (!std::isfinite(forward[column][row]))
                        throw std::runtime_error("pelican.procedural_transform_invalid@1: transform must be finite");
            if (forward[0][3] != 0.0F || forward[1][3] != 0.0F || forward[2][3] != 0.0F || forward[3][3] != 1.0F)
                throw std::runtime_error("pelican.procedural_transform_invalid@1: transform must be affine");
            const auto determinant = glm::determinant(forward);
            if (!std::isfinite(determinant) || determinant == 0.0F)
                throw std::runtime_error("pelican.procedural_transform_invalid@1: transform must be invertible");
            const auto inverse = glm::inverse(forward);
            std::array<float, 24> encoded{};
            unsigned index = 0;
            for (const auto &matrix : {forward, inverse})
                for (unsigned row = 0; row < 3; ++row)
                    for (unsigned column = 0; column < 4; ++column) {
                        const auto value = matrix[column][row];
                        if (!std::isfinite(value))
                            throw std::runtime_error("pelican.procedural_transform_invalid@1: inverse transform must be finite");
                        encoded[index++] = value == 0.0F ? 0.0F : value;
                    }
            std::memcpy(resolved.back().record_data.data() + *offset, encoded.data(), sizeof(encoded));
        }
    }
    if (bindings != nullptr) {
        for (std::size_t index = 0; index < binding_matched.size(); ++index) {
            if (!binding_matched[index]) {
                const auto &selector = bindings->bindings[index].selector;
                if (selector.kind ==
                    RayBindingSelector::Kind::procedural) {
                    throw std::runtime_error(
                        "pelican.procedural_group_unresolved@1: procedural selector '" +
                        selector.reference +
                        "' did not resolve to a scene row");
                }
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
        if (!left.geometry_match_identity.empty() &&
            left.geometry_match_identity == right.geometry_match_identity &&
            (left.group_index != right.group_index ||
             left.record_data != right.record_data)) {
            throw std::runtime_error(
                "pelican.ray_row_binding_ambiguous@1: geometric row '" +
                left.geometry_match_identity +
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
        if (semantic_mode == RayLayoutSemanticMode::LayoutV1Q) {
            resolved[index].instance_custom_index = resolved[index].q;
            resolved[index].setting_sources.custom_index = "layout_v1_q";
        }
        identity << resolved[index].scene_identity << '\0'
                 << resolved[index].geometry_identity << '\0'
                 << resolved[index].geometry_match_identity << '\0'
                 << resolved[index].material_reference << '\0'
                 << static_cast<unsigned>(resolved[index].geometry_kind)
                 << '\0'
                 << resolved[index].group_index << '\n';
        for (const auto byte : resolved[index].record_data)
            identity << static_cast<char>(byte);
    }
    RaySceneRowSnapshot result;
    result.record_members = layout.record;
    result.layout_reference = layout.source_reference;
    result.bindings_reference = bindings ? bindings->source_reference : std::string{};
    result.group_names = layout.groups;
    result.semantic_mode = semantic_mode;
    const auto encoded = identity.str();
    result.identity_digest_sha256 =
        picosha2::hash256_hex_string(encoded.begin(), encoded.end());
    result.rows = std::move(resolved);
    return result;
}

RayTracingTableBuildResult buildRayTracingTableSnapshot(
    const RaySceneRowSnapshot &immutable_scene_rows,
    std::span<const RayMaterialTableSource> materials,
    const RayTracingGeometrySnapshot &geometry,
    const RayGroupLayout *layout) {
    const auto profile_active =
        layout != nullptr && layout->usesMaterialAlphaProfile();
    constexpr std::uint32_t mask_alpha_mode = 1u;
    constexpr std::uint32_t uv_transform_absent = 0u;
    constexpr std::uint32_t uv_transform_identity = 1u;
    const auto hash_bytes = [](std::string_view prefix,
                               const void *data, std::size_t size) {
        std::string encoded{prefix};
        encoded.append(static_cast<const char *>(data), size);
        return picosha2::hash256_hex_string(
            encoded.begin(), encoded.end());
    };
    const auto same_material_source = [profile_active](
                                          const RayMaterialTableSource &left,
                                          const RayMaterialTableSource &right) {
        const auto ordinary_equal =
               left.base_color_factor == right.base_color_factor &&
               left.alpha_cutoff == right.alpha_cutoff &&
               left.alpha_mode == right.alpha_mode &&
               left.base_color_texture_identity ==
                   right.base_color_texture_identity &&
               left.base_color_srgb_view == right.base_color_srgb_view &&
               left.default_sampler == right.default_sampler;
        if (!profile_active) {
            return left.material_handle == right.material_handle &&
                   left.material_generation ==
                       right.material_generation &&
                   ordinary_equal;
        }
        if (!ordinary_equal) return false;
        return left.has_normalized_alpha_ir ==
                   right.has_normalized_alpha_ir &&
               left.alpha_source_kind == right.alpha_source_kind &&
               left.alpha_material_identity ==
                   right.alpha_material_identity &&
               left.alpha_texture_identity ==
                   right.alpha_texture_identity &&
               left.normalized_alpha_mode ==
                   right.normalized_alpha_mode &&
               left.normalized_alpha_cutoff ==
                   right.normalized_alpha_cutoff &&
               left.normalized_base_factor_a ==
                   right.normalized_base_factor_a &&
               left.normalized_uv_offset_scale ==
                   right.normalized_uv_offset_scale &&
               left.normalized_uv_rotation ==
                   right.normalized_uv_rotation &&
               left.normalized_uv_transform_state ==
                   right.normalized_uv_transform_state &&
               left.normalized_alpha_digest_sha256 ==
                   right.normalized_alpha_digest_sha256;
    };

    std::map<std::tuple<int, std::uint64_t, std::string>,
             const RayMaterialTableSource *>
        source_by_handle;
    std::map<std::string, const RayMaterialTableSource *, std::less<>>
        source_by_key;
    for (const auto &source : materials) {
        if (source.material_handle < 0 || source.material_generation == 0 ||
            source.canonical_key.empty() || !source.material_lease) {
            throw std::runtime_error(
                "pelican.ray_material_snapshot_invalid@1: resolved material handle, generation, lease, and canonical key are required");
        }
        const bool profile_mask =
            profile_active && source.alpha_mode == mask_alpha_mode;
        if (profile_mask &&
            (!source.has_normalized_alpha_ir ||
             source.alpha_source_kind != "gltf_base_color" ||
             source.normalized_alpha_digest_sha256.empty())) {
            throw std::runtime_error(
                "pelican.ray_profile_alpha_ir_missing@1: material '" +
                source.canonical_key +
                "' requires project-lowered gltf_base_color NormalizedAlphaIr");
        }
        if (profile_mask &&
            (source.alpha_material_identity != source.canonical_key ||
             source.alpha_texture_identity !=
                 source.base_color_texture_identity)) {
            throw std::runtime_error(
                "pelican.ray_profile_alpha_identity_mismatch@1: material '" +
                source.canonical_key +
                "' NormalizedAlphaIr is not derived from its WP368a authored references");
        }
        if (profile_mask &&
            source.normalized_uv_transform_state !=
                uv_transform_absent &&
            source.normalized_uv_transform_state !=
                uv_transform_identity) {
            const auto authored_state =
                source.normalized_uv_transform_state == 3u
                    ? "present-invalid"
                    : "non-identity";
            throw std::runtime_error(
                "pelican.ray_profile_uv_transform_unsupported@1: material '" +
                source.canonical_key + "' has " + authored_state +
                " KHR_texture_transform data; raster alpha does not consume this extension");
        }
        const auto handle_key = std::tuple{
            source.material_handle, source.material_generation,
            source.canonical_key};
        const auto [handle_found, handle_inserted] =
            source_by_handle.emplace(handle_key, &source);
        if (!handle_inserted &&
            !same_material_source(*handle_found->second, source)) {
            throw std::runtime_error(
                "pelican.ray_material_snapshot_invalid@1: one resolved material generation has inconsistent table data");
        }
        const auto [key_found, key_inserted] =
            source_by_key.emplace(source.canonical_key, &source);
        if (!key_inserted &&
            !same_material_source(*key_found->second, source)) {
            throw std::runtime_error(
                "pelican.ray_material_key_collision@1: canonical material key '" +
                source.canonical_key + "' resolves to inconsistent material generations");
        }
    }

    const auto texture_identity = [profile_active](
                                      const RayMaterialTableSource &source)
        -> std::string_view {
        if (profile_active) {
            return source.alpha_mode == mask_alpha_mode
                       ? std::string_view{source.alpha_texture_identity}
                       : std::string_view{};
        }
        return source.base_color_texture_identity;
    };
    std::map<std::string, RayTextureTableEntry, std::less<>> texture_by_key;
    for (const auto &[_, source] : source_by_key) {
        const auto &identity = texture_identity(*source);
        if (identity.empty()) continue;
        if (!source->base_color_srgb_view || !source->default_sampler) {
            throw std::runtime_error(
                "pelican.ray_material_texture_invalid@1: canonical texture '" +
                std::string{identity} +
                "' requires an sRGB view and default sampler");
        }
        RayTextureTableEntry entry{
            .canonical_identity = std::string{identity},
            .srgb_view = source->base_color_srgb_view,
            .sampler = source->default_sampler,
        };
        const auto [found, inserted] = texture_by_key.emplace(
            entry.canonical_identity, entry);
        if (!inserted &&
            (found->second.srgb_view != entry.srgb_view ||
             found->second.sampler != entry.sampler)) {
            throw std::runtime_error(
                "pelican.ray_material_texture_identity_collision@1: canonical texture identity maps to inconsistent image/sampler resources");
        }
    }
    if (texture_by_key.size() > rayBindlessTextureCapacity) {
        throw std::runtime_error(
            "pelican.ray_material_texture_capacity@1: canonical texture count exceeds 1024");
    }

    RayTracingTableBuildResult build;
    build.scene_rows = immutable_scene_rows;
    auto &scene_rows = build.scene_rows;
    auto &result = build.tables;
    result.textures.reserve(texture_by_key.size());
    for (const auto &[_, texture] : texture_by_key) {
        result.textures.push_back(texture);
    }
    const auto material_row_count =
        profile_active ? scene_rows.rows.size() : source_by_key.size();
    result.material_keys.reserve(material_row_count);
    result.material_row_digests.reserve(material_row_count);
    result.material_leases.reserve(material_row_count);
    if (!profile_active) {
        result.material_rows.reserve(source_by_key.size());
        for (const auto &[key, source] : source_by_key) {
            RayMaterialTableRow row{
                .base_color_factor = source->base_color_factor,
                .alpha_cutoff = source->alpha_cutoff,
                .alpha_mode = source->alpha_mode,
            };
            if (!source->base_color_texture_identity.empty()) {
                const auto texture = std::lower_bound(
                    result.textures.begin(), result.textures.end(),
                    source->base_color_texture_identity,
                    [](const auto &entry, std::string_view identity) {
                        return entry.canonical_identity < identity;
                    });
                row.base_color_texture_index =
                    static_cast<std::uint32_t>(
                        texture - result.textures.begin());
            }
            result.material_keys.push_back(key);
            result.material_rows.push_back(row);
            result.material_leases.push_back(source->material_lease);
            std::ostringstream prefix;
            prefix << key << '\0' << source->material_handle << ':'
                   << source->material_generation << '\0';
            result.material_row_digests.push_back(hash_bytes(
                prefix.str(), &result.material_rows.back(), sizeof(row)));
        }
        result.material_bytes.resize(
            result.material_rows.size() * sizeof(RayMaterialTableRow));
        if (!result.material_bytes.empty()) {
            std::memcpy(result.material_bytes.data(),
                        result.material_rows.data(),
                        result.material_bytes.size());
        }
    } else {
        result.profile_material_rows.reserve(scene_rows.rows.size());
        for (const auto &scene_row : scene_rows.rows) {
            if (scene_row.geometry_kind == RayGeometryKind::aabbs) {
                const auto key =
                    "procedural:" + std::to_string(scene_row.q);
                result.material_keys.push_back(key);
                result.profile_material_rows.emplace_back();
                result.material_leases.emplace_back();
                result.material_row_digests.push_back(hash_bytes(
                    key, &result.profile_material_rows.back(),
                    sizeof(RayProfileMaterialTableRow)));
                continue;
            }
            const auto source = source_by_handle.find(
                {scene_row.resolved_material_handle,
                 scene_row.resolved_material_generation,
                 scene_row.material_reference});
            if (source == source_by_handle.end() ||
                source->second->material_lease !=
                    scene_row.resolved_material_lease) {
                throw std::runtime_error(
                    "pelican.ray_material_snapshot_invalid@1: selector row does not match its resolved material handle generation/lease");
            }
            const auto geometry_row = std::lower_bound(
                geometry.rows.begin(), geometry.rows.end(),
                scene_row.geometry_allocation_id,
                [](const auto &candidate, std::uint64_t identity) {
                    return candidate.geometry_allocation_id < identity;
                });
            if (geometry_row == geometry.rows.end() ||
                geometry_row->geometry_allocation_id !=
                    scene_row.geometry_allocation_id) {
                throw std::runtime_error(
                    "pelican.ray_geometry_snapshot_invalid@1: row allocation ID is absent from the geometry table");
            }
            const auto &material = *source->second;
            const bool mask =
                material.alpha_mode == mask_alpha_mode;
            RayProfileMaterialTableRow row{
                .alpha_mode = material.alpha_mode,
                .cutoff = mask ? material.normalized_alpha_cutoff
                               : material.alpha_cutoff,
                .base_factor_a =
                    mask ? material.normalized_base_factor_a
                         : material.base_color_factor[3],
                .uv_offset_scale =
                    mask ? material.normalized_uv_offset_scale
                         : std::array<float, 4>{
                               0.0F, 0.0F, 1.0F, 1.0F},
                .uv_rotation =
                    mask ? material.normalized_uv_rotation : 0.0F,
                .uses_vertex_color =
                    mask &&
                            (geometry_row->gpu.attribute_presence &
                             rayGeometryAttributeColor0) != 0
                        ? 1u
                        : 0u,
            };
            if (mask && !material.alpha_texture_identity.empty()) {
                const auto texture = std::lower_bound(
                    result.textures.begin(), result.textures.end(),
                    material.alpha_texture_identity,
                    [](const auto &entry, std::string_view identity) {
                        return entry.canonical_identity < identity;
                    });
                if (texture == result.textures.end() ||
                    texture->canonical_identity !=
                        material.alpha_texture_identity) {
                    throw std::logic_error(
                        "profile texture identity was not lowered into the table");
                }
                row.texture_index = static_cast<std::uint32_t>(
                    texture - result.textures.begin());
            }
            if (mask && (scene_row.alpha_override_mask &
                 rayAlphaOverrideBaseFactor) != 0) {
                row.base_factor_a =
                    scene_row.alpha_override_base_color_factor[3];
            }
            if (mask && (scene_row.alpha_override_mask &
                 rayAlphaOverrideUvTransform) != 0) {
                row.uv_offset_scale =
                    scene_row.alpha_override_uv_offset_scale;
                row.uv_rotation =
                    scene_row.alpha_override_uv_rotation;
            }
            if (mask) {
                validateRayProfileGeometryAttributes(
                    row, geometry_row->gpu, scene_row.q,
                    scene_row.material_reference);
            }
            const auto key =
                "row:" + std::to_string(scene_row.q) + ":" +
                scene_row.material_reference;
            result.material_keys.push_back(key);
            result.profile_material_rows.push_back(row);
            result.material_leases.push_back(material.material_lease);
            std::string prefix = key;
            prefix.push_back('\0');
            prefix += material.normalized_alpha_digest_sha256;
            prefix.push_back('\0');
            prefix += scene_row.scene_identity;
            prefix.push_back('\0');
            result.material_row_digests.push_back(hash_bytes(
                prefix, &result.profile_material_rows.back(),
                sizeof(row)));
        }
        result.material_bytes.resize(
            result.profile_material_rows.size() *
            sizeof(RayProfileMaterialTableRow));
        if (!result.material_bytes.empty()) {
            std::memcpy(result.material_bytes.data(),
                        result.profile_material_rows.data(),
                        result.material_bytes.size());
        }
    }
    std::string material_identity;
    for (std::size_t index = 0; index < result.material_keys.size(); ++index) {
        material_identity += result.material_keys[index];
        material_identity.push_back('\0');
        material_identity += result.material_row_digests[index];
        material_identity.push_back('\n');
    }
    for (const auto &texture : result.textures) {
        material_identity += texture.canonical_identity;
        material_identity.push_back('\n');
    }
    if (!result.material_bytes.empty()) {
        material_identity.append(
            reinterpret_cast<const char *>(result.material_bytes.data()),
            result.material_bytes.size());
    }
    result.material_digest_sha256 = picosha2::hash256_hex_string(
        material_identity.begin(), material_identity.end());

    result.geometry_allocation_ids.reserve(geometry.rows.size());
    result.geometry_rows.reserve(geometry.rows.size());
    result.geometry_row_digests.reserve(geometry.rows.size());
    std::uint64_t previous_id = 0;
    for (const auto &source : geometry.rows) {
        if (source.geometry_allocation_id == 0 ||
            (!result.geometry_allocation_ids.empty() &&
             source.geometry_allocation_id <= previous_id)) {
            throw std::runtime_error(
                "pelican.ray_geometry_snapshot_invalid@1: geometry rows must be unique non-zero allocation IDs in ascending order");
        }
        previous_id = source.geometry_allocation_id;
        result.geometry_allocation_ids.push_back(
            source.geometry_allocation_id);
        result.geometry_rows.push_back(source.gpu);
        const auto prefix = std::to_string(source.geometry_allocation_id) + "\0";
        result.geometry_row_digests.push_back(hash_bytes(
            prefix, &result.geometry_rows.back(), sizeof(source.gpu)));
    }
    result.geometry_bytes.resize(
        result.geometry_rows.size() * sizeof(RayGeometryTableRow));
    if (!result.geometry_bytes.empty()) {
        std::memcpy(result.geometry_bytes.data(), result.geometry_rows.data(),
                    result.geometry_bytes.size());
    }
    std::string geometry_identity;
    for (std::size_t index = 0;
         index < result.geometry_allocation_ids.size(); ++index) {
        geometry_identity +=
            std::to_string(result.geometry_allocation_ids[index]);
        geometry_identity.push_back('\0');
        geometry_identity += result.geometry_row_digests[index];
        geometry_identity.push_back('\n');
    }
    if (!result.geometry_bytes.empty()) {
        geometry_identity.append(
            reinterpret_cast<const char *>(result.geometry_bytes.data()),
            result.geometry_bytes.size());
    }
    result.geometry_digest_sha256 = picosha2::hash256_hex_string(
        geometry_identity.begin(), geometry_identity.end());

    for (auto &scene_row : scene_rows.rows) {
        if (scene_row.geometry_kind == RayGeometryKind::aabbs) {
            scene_row.material_index =
                std::numeric_limits<std::uint32_t>::max();
        } else {
        const auto material_source = source_by_handle.find(
            {scene_row.resolved_material_handle,
             scene_row.resolved_material_generation,
             scene_row.material_reference});
        if (material_source == source_by_handle.end() ||
            material_source->second->material_lease !=
                scene_row.resolved_material_lease) {
            throw std::runtime_error(
                "pelican.ray_material_snapshot_invalid@1: selector row does not match its resolved material handle generation/lease");
        }
        if (profile_active) {
            if (scene_row.q >= result.profile_material_rows.size() ||
                result.material_keys[scene_row.q] !=
                    "row:" + std::to_string(scene_row.q) + ":" +
                        scene_row.material_reference) {
                throw std::runtime_error(
                    "pelican.ray_material_snapshot_invalid@1: resolved profile row is absent from the per-instance table");
            }
            scene_row.material_index = scene_row.q;
        } else {
            const auto material = std::lower_bound(
                result.material_keys.begin(), result.material_keys.end(),
                scene_row.material_reference);
            if (material == result.material_keys.end() ||
                *material != scene_row.material_reference) {
                throw std::runtime_error(
                    "pelican.ray_material_snapshot_invalid@1: row canonical material key is absent from the table");
            }
            scene_row.material_index = static_cast<std::uint32_t>(
                material - result.material_keys.begin());
        }
        }

        const auto geometry_row = std::lower_bound(
            result.geometry_allocation_ids.begin(),
            result.geometry_allocation_ids.end(),
            scene_row.geometry_allocation_id);
        if (geometry_row == result.geometry_allocation_ids.end() ||
            *geometry_row != scene_row.geometry_allocation_id) {
            throw std::runtime_error(
                "pelican.ray_geometry_snapshot_invalid@1: row allocation ID is absent from the geometry table");
        }
        scene_row.geometry_index = static_cast<std::uint32_t>(
            geometry_row - result.geometry_allocation_ids.begin());
    }
    scene_rows.material_table_digest_sha256 =
        result.material_digest_sha256;
    scene_rows.geometry_table_digest_sha256 =
        result.geometry_digest_sha256;
    std::string final_identity = scene_rows.identity_digest_sha256 + '\n' +
        result.material_digest_sha256 + '\n' +
        result.geometry_digest_sha256 + '\n';
    for (const auto &row : scene_rows.rows) {
        final_identity += std::to_string(row.q) + ':' +
                          std::to_string(row.material_index) + ':' +
                          std::to_string(row.geometry_index) + ':' +
                          std::to_string(row.resolved_material_generation) +
                          '\n';
    }
    scene_rows.identity_digest_sha256 = picosha2::hash256_hex_string(
        final_identity.begin(), final_identity.end());
    return build;
}

void validateRayProfileGeometryAttributes(
    const RayProfileMaterialTableRow &row,
    const RayGeometryTableRow &geometry,
    std::uint32_t q, std::string_view material_reference) {
    const auto fail = [&](std::string_view attribute) {
        throw std::runtime_error(
            "pelican.ray_geometry_attribute_missing@1: row=" +
            std::to_string(q) + " material='" +
            std::string{material_reference} + "' missing='" +
            std::string{attribute} + "'");
    };
    if (row.texture_index != rayMissingTextureIndex &&
        (geometry.attribute_presence & rayGeometryAttributeUv0) == 0) {
        fail("TEXCOORD_0");
    }
    if (row.uses_vertex_color != 0 &&
        (geometry.attribute_presence & rayGeometryAttributeColor0) == 0) {
        fail("COLOR_0");
    }
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
                const auto physical = std::span<std::byte>{bytes}.subspan(
                    destination + data_start, layout.data_size);
                if (const auto offset = layout.engineRecordOffset(
                        RayEngineRecordField::material_index)) {
                    if (row.material_index == rayMissingTextureIndex &&
                        row.geometry_kind != RayGeometryKind::aabbs) {
                        throw std::runtime_error(
                            "pelican.sbt.row_snapshot_invalid@1: material_index was not finalized");
                    }
                    writeU32(physical.subspan(*offset, 4),
                             row.material_index);
                }
                if (const auto offset = layout.engineRecordOffset(
                        RayEngineRecordField::geometry_index)) {
                    if (row.geometry_index == rayMissingTextureIndex) {
                        throw std::runtime_error(
                            "pelican.sbt.row_snapshot_invalid@1: geometry_index was not finalized");
                    }
                    writeU32(physical.subspan(*offset, 4),
                             row.geometry_index);
                }
                if (const auto offset = layout.engineRecordOffset(
                        RayEngineRecordField::geometry_kind)) {
                    writeU32(physical.subspan(*offset, 4),
                             static_cast<std::uint32_t>(
                                 row.geometry_kind));
                }
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

bool rayTracingTlasRecordOffsetOverrideActiveForTesting() noexcept {
    return static_cast<bool>(tlas_record_offset_override);
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
