#include "raygrouplayout.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Pelican {
namespace {

static_assert(alignof(RayMaterialTableRow) == 16);
static_assert(offsetof(RayMaterialTableRow, base_color_factor) == 0);
static_assert(offsetof(RayMaterialTableRow, alpha_cutoff) == 16);
static_assert(offsetof(RayMaterialTableRow, alpha_mode) == 20);
static_assert(offsetof(RayMaterialTableRow, base_color_texture_index) == 24);
static_assert(offsetof(RayMaterialTableRow, reserved) == 28);
static_assert(sizeof(RayMaterialTableRow) == 32);
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
    }
    return "unknown";
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

std::string materialReference(
    const RaySceneRowInput &row, bool table_key) {
    if (!row.material_document.empty()) {
        auto result = "document:" + row.material_document;
        if (table_key) result += "#" + row.material_name;
        return result;
    }
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

std::optional<std::uint32_t> RayGroupLayout::engineRecordOffset(
    RayEngineRecordField field) const noexcept {
    const auto name = engineRecordFieldName(field);
    const auto found = std::find_if(
        physical_record.begin(), physical_record.end(),
        [name](const auto &member) { return member.name == name; });
    if (found == physical_record.end()) return std::nullopt;
    return found->offset;
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
                         "groups", "misses", "engine_record", "record"},
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
                             RayEngineRecordField::geometry_index}) {
        if (requested_engine_fields.contains(field)) {
            result.engine_record.push_back(field);
        }
    }

    const auto &record = root.at("record");
    if (!record.is_array()) {
        layoutError(result.source_reference, "record must be an array");
    }
    std::set<std::string> member_names;
    std::uint32_t cursor = 0;
    const TypeFacts engine_facts{RayRecordType::u32, 4, 4, 1};
    for (const auto field : result.engine_record) {
        result.physical_record.push_back(appendRecordMember(
            std::string{engineRecordFieldName(field)}, engine_facts,
            cursor));
    }
    for (const auto &encoded : record) {
        requireObjectFields(encoded, {"name", "type"}, "record member", result.source_reference);
        if (!encoded.contains("name") || !encoded.at("name").is_string() ||
            !validName(encoded.at("name").get_ref<const std::string &>()) ||
            !encoded.contains("type") || !encoded.at("type").is_string()) {
            layoutError(result.source_reference,
                        "record member requires valid name and string type");
        }
        auto name = encoded.at("name").get<std::string>();
        if (name == "material_index" || name == "geometry_index") {
            throw std::runtime_error(
                "pelican.ray_record_name_reserved@1: authored record member '" +
                name + "' is reserved for the engine");
        }
        if (!member_names.emplace(name).second) {
            layoutError(result.source_reference,
                        "record contains duplicate member '" + name + "'");
        }
        const auto facts = typeFacts(encoded.at("type").get_ref<const std::string &>(),
                                     result.source_reference);
        auto member = appendRecordMember(std::move(name), facts, cursor);
        result.record.push_back(member);
        result.physical_record.push_back(std::move(member));
    }
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
    for (std::size_t index = 0; index < layout.engine_record.size(); ++index) {
        source << "    uint "
               << engineRecordFieldName(layout.engine_record[index]) << ';';
        if (index + 1 != layout.engine_record.size()) source << " \\\n";
        else source << '\n';
    }
    source << "const uint pelican_ray_missing_texture = 0xffffffffu;\n";
    source << "const uint pelican_ray_attribute_normal = 1u;\n";
    source << "const uint pelican_ray_attribute_uv0 = 2u;\n";
    source << "const uint pelican_ray_attribute_color0 = 4u;\n";
    source << R"glsl(
struct PelicanRayMaterialRow {
    vec4 base_color_factor;
    float alpha_cutoff;
    uint alpha_mode;
    uint base_color_texture_index;
    uint reserved;
};
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
}
vec4 pelicanRaySampleBaseColor(uint material_index, vec2 uv) {
    PelicanRayMaterialRow row = pelicanRayMaterial(material_index);
    vec4 texel = row.base_color_texture_index == pelican_ray_missing_texture
        ? vec4(1.0)
        : texture(pelican_ray_material_textures[nonuniformEXT(row.base_color_texture_index)], uv);
    return row.base_color_factor * texel;
}
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
                        materialReference(input, layout.usesRayTables()) +
                        "' resolves more than once");
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
              .geometry_match_identity = input.geometry_match_identity,
              .material_reference = materialReference(
                  input, layout.usesRayTables()),
              .material_name = input.material_name,
              .resolved_material_handle =
                  input.resolved_material_handle,
              .resolved_material_generation =
                  input.resolved_material_generation,
              .resolved_material_lease =
                  input.resolved_material_lease,
              .geometry_allocation_id =
                  input.geometry_allocation_id});
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
        identity << resolved[index].scene_identity << '\0'
                 << resolved[index].geometry_identity << '\0'
                 << resolved[index].geometry_match_identity << '\0'
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

RayTracingTableBuildResult buildRayTracingTableSnapshot(
    const RaySceneRowSnapshot &immutable_scene_rows,
    std::span<const RayMaterialTableSource> materials,
    const RayTracingGeometrySnapshot &geometry) {
    const auto hash_bytes = [](std::string_view prefix,
                               const void *data, std::size_t size) {
        std::string encoded{prefix};
        encoded.append(static_cast<const char *>(data), size);
        return picosha2::hash256_hex_string(
            encoded.begin(), encoded.end());
    };
    const auto same_material_source = [](const RayMaterialTableSource &left,
                                         const RayMaterialTableSource &right) {
        return left.material_handle == right.material_handle &&
               left.material_generation == right.material_generation &&
               left.base_color_factor == right.base_color_factor &&
               left.alpha_cutoff == right.alpha_cutoff &&
               left.alpha_mode == right.alpha_mode &&
               left.base_color_texture_identity ==
                   right.base_color_texture_identity &&
               left.base_color_srgb_view == right.base_color_srgb_view &&
               left.default_sampler == right.default_sampler;
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

    std::map<std::string, RayTextureTableEntry, std::less<>> texture_by_key;
    for (const auto &[_, source] : source_by_key) {
        if (source->base_color_texture_identity.empty()) continue;
        if (!source->base_color_srgb_view || !source->default_sampler) {
            throw std::runtime_error(
                "pelican.ray_material_texture_invalid@1: canonical texture '" +
                source->base_color_texture_identity +
                "' requires an sRGB view and default sampler");
        }
        RayTextureTableEntry entry{
            .canonical_identity = source->base_color_texture_identity,
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
    result.material_keys.reserve(source_by_key.size());
    result.material_rows.reserve(source_by_key.size());
    result.material_row_digests.reserve(source_by_key.size());
    result.material_leases.reserve(source_by_key.size());
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
            row.base_color_texture_index = static_cast<std::uint32_t>(
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
        std::memcpy(result.material_bytes.data(), result.material_rows.data(),
                    result.material_bytes.size());
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
                    if (row.material_index == rayMissingTextureIndex) {
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
