#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

// RC-3c2b1: the std430 record ABI of ray tracing records -- a ray group
// layout's hit-group record (core's RayGroupLayout reads it) and a task's
// callable record -- as neutral values: the member types and offsets, the
// pure type checks and the packing of a record's values. The render graph
// IR holds a task's callable record schema and its packed records with
// these; nothing here depends on Vulkan.

enum class RayRecordType : std::uint8_t {
    i32,
    u32,
    f32,
    vec2,
    vec3,
    vec4,
};

struct RayRecordMemberLayout {
    std::string name;
    RayRecordType type = RayRecordType::u32;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t alignment = 0;

    bool operator==(const RayRecordMemberLayout &) const = default;
};

// Callable records have their own std430 schema, without hit-group engine fields.
struct RayCallableRecordLayout {
    std::vector<RayRecordMemberLayout> members;
    std::uint32_t data_size = 0;

    bool operator==(const RayCallableRecordLayout &) const = default;
};

// A record member's value as written. The packing reads the kind: an i32
// takes a signed integer (a JSON literal without a sign is unsigned), a u32
// a non-negative integer, an f32 any number, a vector a list of numbers.
struct RayRecordValue {
    enum class Kind : std::uint8_t {
        other,
        signed_integer,
        unsigned_integer,
        floating,
        list,
    };
    Kind kind = Kind::other;
    std::int64_t signed_integer = 0;
    std::uint64_t unsigned_integer = 0;
    double floating = 0.0;
    std::vector<RayRecordValue> list;

    bool operator==(const RayRecordValue &) const = default;
};
// A record as written: its members by name, in the written (sorted) order;
// nullopt when it is not an object.
using RayRecordValues =
    std::optional<std::vector<std::pair<std::string, RayRecordValue>>>;
// A task's callable records as written: nullopt when not an array.
using RayCallableRecordValues = std::optional<std::vector<RayRecordValues>>;

RayRecordValues rayRecordValuesOf(const nlohmann::json &record);
RayCallableRecordValues rayCallableRecordValuesOf(const nlohmann::json &records);

// pelican.ray_group_layout_invalid@1: <message>[ in '<source>'].
[[noreturn]] void rayGroupLayoutError(std::string_view source, std::string message);
// pelican.ray_binding_record_invalid@1: <message>[ in '<source>'].
[[noreturn]] void rayBindingRecordError(std::string_view source, std::string message);
// ^[A-Za-z0-9_]{1,64}$
bool isRayRecordName(std::string_view name);
// `value` is an object with no field outside `allowed`
// (rayGroupLayoutError otherwise).
void requireRayLayoutObjectFields(const nlohmann::json &value,
                                  std::initializer_list<std::string_view> allowed,
                                  std::string_view domain, std::string_view source);
std::uint32_t alignRayRecordOffset(std::uint32_t value, std::uint32_t alignment,
                                   std::string_view diagnostic);

struct RayRecordTypeFacts {
    RayRecordType type;
    std::uint32_t size;
    std::uint32_t alignment;
    std::uint32_t components;
};
// pelican.ray_record_type_unsupported@1 for a name outside i32..vec4.
RayRecordTypeFacts rayRecordTypeFacts(std::string_view encoded, std::string_view source);
RayRecordMemberLayout appendRayRecordMember(std::string name, const RayRecordTypeFacts &facts,
                                            std::uint32_t &cursor);
// A record's members ([{name, type}]) laid out from `cursor`.
std::vector<RayRecordMemberLayout> parseRayRecordMembers(const nlohmann::json &record,
                                                         std::uint32_t &cursor,
                                                         bool reserve_engine_names,
                                                         std::string_view source);
void writeRayRecordU32(std::span<std::byte> destination, std::uint32_t value);

// One record's values packed by `members` into `data_size` bytes
// (pelican.ray_binding_record_invalid@1 for a member set or a value the
// schema does not take).
std::vector<std::byte> encodeRayRecord(const RayRecordValues &record,
                                       std::span<const RayRecordMemberLayout> members,
                                       std::uint32_t data_size, std::string_view source);

RayCallableRecordLayout parseRayCallableRecordLayout(
    const nlohmann::json &members, std::string_view diagnostic = {});
std::vector<std::byte> encodeRayCallableRecord(
    const RayRecordValues &values, const RayCallableRecordLayout &layout,
    std::string_view diagnostic = {});
std::vector<std::byte> encodeRayCallableRecord(
    const nlohmann::json &values, const RayCallableRecordLayout &layout,
    std::string_view diagnostic = {});
// One record per callable (pelican.ray_tracing.callable_records_invalid@1
// otherwise), each packed by the schema.
std::vector<std::vector<std::byte>> encodeRayCallableRecords(
    const RayCallableRecordValues &records, const RayCallableRecordLayout &layout,
    std::size_t callable_count, std::string_view diagnostic = {});
std::vector<std::vector<std::byte>> encodeRayCallableRecords(
    const nlohmann::json &records, const RayCallableRecordLayout &layout,
    std::size_t callable_count, std::string_view diagnostic = {});

} // namespace Pelican
