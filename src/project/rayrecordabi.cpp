#include "rayrecordabi.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace Pelican {

namespace {

using json = nlohmann::json;

RayRecordValue rayRecordValueOf(const json &value) {
    RayRecordValue result;
    if (value.is_number_unsigned()) {
        result.kind = RayRecordValue::Kind::unsigned_integer;
        result.unsigned_integer = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        result.kind = RayRecordValue::Kind::signed_integer;
        result.signed_integer = value.get<std::int64_t>();
    } else if (value.is_number_float()) {
        result.kind = RayRecordValue::Kind::floating;
        result.floating = value.get<double>();
    } else if (value.is_array()) {
        result.kind = RayRecordValue::Kind::list;
        for (const auto &entry : value) result.list.push_back(rayRecordValueOf(entry));
    }
    return result;
}

bool isNumber(const RayRecordValue &value) {
    return value.kind == RayRecordValue::Kind::signed_integer ||
           value.kind == RayRecordValue::Kind::unsigned_integer ||
           value.kind == RayRecordValue::Kind::floating;
}

double numberOf(const RayRecordValue &value) {
    switch (value.kind) {
    case RayRecordValue::Kind::signed_integer:
        return static_cast<double>(value.signed_integer);
    case RayRecordValue::Kind::unsigned_integer:
        return static_cast<double>(value.unsigned_integer);
    default:
        return value.floating;
    }
}

float checkedFloat(const RayRecordValue &value, std::string_view member,
                   std::string_view source) {
    if (!isNumber(value)) {
        rayBindingRecordError(source, "member '" + std::string{member} +
                                          "' requires an f32 number");
    }
    const auto encoded = numberOf(value);
    const auto converted = static_cast<float>(encoded);
    if (!std::isfinite(encoded) || !std::isfinite(converted)) {
        rayBindingRecordError(source, "member '" + std::string{member} +
                                          "' is outside finite f32");
    }
    return converted;
}

} // namespace

RayRecordValues rayRecordValuesOf(const nlohmann::json &record) {
    if (!record.is_object()) return std::nullopt;
    std::vector<std::pair<std::string, RayRecordValue>> members;
    for (auto field = record.begin(); field != record.end(); ++field) {
        members.emplace_back(field.key(), rayRecordValueOf(field.value()));
    }
    return members;
}

RayCallableRecordValues rayCallableRecordValuesOf(const nlohmann::json &records) {
    if (!records.is_array()) return std::nullopt;
    std::vector<RayRecordValues> result;
    for (const auto &record : records) result.push_back(rayRecordValuesOf(record));
    return result;
}

[[noreturn]] void rayGroupLayoutError(std::string_view source, std::string message) {
    throw std::runtime_error(
        "pelican.ray_group_layout_invalid@1: " + message +
        (source.empty() ? std::string{} : " in '" + std::string{source} + "'"));
}

[[noreturn]] void rayBindingRecordError(std::string_view source, std::string message) {
    throw std::runtime_error(
        "pelican.ray_binding_record_invalid@1: " + message +
        (source.empty() ? std::string{} : " in '" + std::string{source} + "'"));
}

bool isRayRecordName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    });
}

void requireRayLayoutObjectFields(const nlohmann::json &value,
                                  std::initializer_list<std::string_view> allowed,
                                  std::string_view domain, std::string_view source) {
    if (!value.is_object()) {
        rayGroupLayoutError(source, std::string{domain} + " must be an object");
    }
    for (auto field = value.begin(); field != value.end(); ++field) {
        const auto known = std::find(allowed.begin(), allowed.end(),
                                     field.key()) != allowed.end();
        if (!known) {
            rayGroupLayoutError(source, std::string{domain} +
                                            " has unknown field '" + field.key() + "'");
        }
    }
}

std::uint32_t alignRayRecordOffset(std::uint32_t value, std::uint32_t alignment,
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

RayRecordTypeFacts rayRecordTypeFacts(std::string_view encoded, std::string_view source) {
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

RayRecordMemberLayout appendRayRecordMember(std::string name, const RayRecordTypeFacts &facts,
                                            std::uint32_t &cursor) {
    cursor = alignRayRecordOffset(cursor, facts.alignment,
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

std::vector<RayRecordMemberLayout> parseRayRecordMembers(const nlohmann::json &record,
                                                         std::uint32_t &cursor,
                                                         bool reserve_engine_names,
                                                         std::string_view source) {
    if (!record.is_array()) {
        rayGroupLayoutError(source, "record must be an array");
    }
    std::vector<RayRecordMemberLayout> members;
    std::set<std::string> member_names;
    for (const auto &encoded : record) {
        requireRayLayoutObjectFields(encoded, {"name", "type"}, "record member", source);
        if (!encoded.contains("name") || !encoded.at("name").is_string() ||
            !isRayRecordName(encoded.at("name").get_ref<const std::string &>()) ||
            !encoded.contains("type") || !encoded.at("type").is_string()) {
            rayGroupLayoutError(source,
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
            rayGroupLayoutError(source,
                                "record contains duplicate member '" + name + "'");
        }
        const auto facts = rayRecordTypeFacts(encoded.at("type").get_ref<const std::string &>(),
                                              source);
        auto member = appendRayRecordMember(std::move(name), facts, cursor);
        members.push_back(std::move(member));
    }
    return members;
}

void writeRayRecordU32(std::span<std::byte> destination, std::uint32_t value) {
    destination[0] = static_cast<std::byte>(value & 0xffu);
    destination[1] = static_cast<std::byte>((value >> 8u) & 0xffu);
    destination[2] = static_cast<std::byte>((value >> 16u) & 0xffu);
    destination[3] = static_cast<std::byte>((value >> 24u) & 0xffu);
}

std::vector<std::byte> encodeRayRecord(const RayRecordValues &record,
                                       std::span<const RayRecordMemberLayout> members,
                                       std::uint32_t data_size, std::string_view source) {
    if (!record || record->size() != members.size()) {
        rayBindingRecordError(source,
                              "record member set does not match layout");
    }
    for (const auto &[name, value] : *record) {
        (void)value;
        if (std::none_of(members.begin(), members.end(),
                         [&](const auto &member) { return member.name == name; })) {
            rayBindingRecordError(source, "unknown record member '" + name + "'");
        }
    }
    std::vector<std::byte> result(data_size, std::byte{0});
    for (const auto &member : members) {
        const auto found = std::find_if(record->begin(), record->end(),
                                        [&](const auto &entry) { return entry.first == member.name; });
        if (found == record->end()) {
            rayBindingRecordError(source, "missing record member '" + member.name + "'");
        }
        const auto &value = found->second;
        const auto write_scalar = [&](std::uint32_t component, std::uint32_t bits) {
            writeRayRecordU32(std::span{result}.subspan(member.offset + component * 4u, 4), bits);
        };
        if (member.type == RayRecordType::i32) {
            if (value.kind != RayRecordValue::Kind::signed_integer) {
                rayBindingRecordError(source, "member '" + member.name + "' requires i32");
            }
            const auto number = value.signed_integer;
            if (number < std::numeric_limits<std::int32_t>::min() ||
                number > std::numeric_limits<std::int32_t>::max()) {
                rayBindingRecordError(source, "member '" + member.name + "' is outside i32");
            }
            write_scalar(0, static_cast<std::uint32_t>(static_cast<std::int32_t>(number)));
        } else if (member.type == RayRecordType::u32) {
            std::uint64_t number = 0;
            if (value.kind == RayRecordValue::Kind::unsigned_integer) {
                number = value.unsigned_integer;
            } else if (value.kind == RayRecordValue::Kind::signed_integer && value.signed_integer >= 0) {
                number = static_cast<std::uint64_t>(value.signed_integer);
            } else {
                rayBindingRecordError(source, "member '" + member.name + "' requires u32");
            }
            if (number > std::numeric_limits<std::uint32_t>::max()) {
                rayBindingRecordError(source, "member '" + member.name + "' is outside u32");
            }
            write_scalar(0, static_cast<std::uint32_t>(number));
        } else if (member.type == RayRecordType::f32) {
            write_scalar(0, std::bit_cast<std::uint32_t>(
                                checkedFloat(value, member.name, source)));
        } else {
            const auto components = member.size / 4u;
            if (value.kind != RayRecordValue::Kind::list || value.list.size() != components) {
                rayBindingRecordError(source, "member '" + member.name +
                                                  "' has wrong vector length");
            }
            for (std::uint32_t component = 0; component < components; ++component) {
                write_scalar(component, std::bit_cast<std::uint32_t>(
                                            checkedFloat(value.list.at(component), member.name, source)));
            }
        }
    }
    return result;
}

RayCallableRecordLayout parseRayCallableRecordLayout(
    const nlohmann::json &members, std::string_view diagnostic) {
    std::uint32_t cursor = 0;
    RayCallableRecordLayout result;
    result.members = parseRayRecordMembers(members, cursor, false, diagnostic);
    std::uint32_t alignment = 1;
    for (const auto &member : result.members)
        alignment = std::max(alignment, member.alignment);
    result.data_size = alignRayRecordOffset(cursor, alignment, "pelican.ray_group_layout_invalid@1");
    return result;
}

std::vector<std::byte> encodeRayCallableRecord(
    const RayRecordValues &values, const RayCallableRecordLayout &layout,
    std::string_view diagnostic) {
    return encodeRayRecord(values, layout.members, layout.data_size, diagnostic);
}

std::vector<std::byte> encodeRayCallableRecord(
    const nlohmann::json &values, const RayCallableRecordLayout &layout,
    std::string_view diagnostic) {
    return encodeRayCallableRecord(rayRecordValuesOf(values), layout, diagnostic);
}

std::vector<std::vector<std::byte>> encodeRayCallableRecords(
    const RayCallableRecordValues &records, const RayCallableRecordLayout &layout,
    std::size_t callable_count, std::string_view diagnostic) {
    if (!records || records->size() != callable_count)
        throw std::runtime_error("pelican.ray_tracing.callable_records_invalid@1: task '" +
            std::string{diagnostic} + "' callable_records must contain one record per callable");
    std::vector<std::vector<std::byte>> result;
    result.reserve(records->size());
    for (const auto &record : *records)
        result.push_back(encodeRayCallableRecord(record, layout, diagnostic));
    return result;
}

std::vector<std::vector<std::byte>> encodeRayCallableRecords(
    const nlohmann::json &records, const RayCallableRecordLayout &layout,
    std::size_t callable_count, std::string_view diagnostic) {
    return encodeRayCallableRecords(rayCallableRecordValuesOf(records), layout, callable_count,
                                    diagnostic);
}

} // namespace Pelican
