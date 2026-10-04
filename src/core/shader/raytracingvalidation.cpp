#include "raytracingvalidation.hpp"
#include "raytracingrefusal.hpp"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

thread_local RayTracingValidationObserver validation_observer;
thread_local RayTracingPipelineCreateObserver create_observer;
thread_local std::array<bool, 4> validation_delegate_enabled{
    true, true, true, true};
thread_local bool record_addressing_validation_enabled = true;

std::size_t siteIndex(RayTracingValidationSite site) {
    const auto index = static_cast<std::size_t>(site);
    if (index >= validation_delegate_enabled.size()) {
        throw std::logic_error("unknown ray tracing validation site");
    }
    return index;
}

std::string deviceName(const RayTracingRuntimeFacts &facts) {
    return facts.device_name.empty() ? "<unknown>" : facts.device_name;
}

[[noreturn]] void failDimension(
    std::size_t dimension, std::uint64_t requested,
    std::uint64_t actual, const RayTracingRuntimeFacts &facts) {
    static constexpr std::array names{"width", "height", "depth"};
    throw std::runtime_error(
        "pelican.ray_tracing.dispatch_dimension_limit@1: property " +
        std::string{names.at(dimension)} + " requested=" +
        std::to_string(requested) + " actual_limit=" +
        std::to_string(actual) + " device='" + deviceName(facts) + "'");
}

const RayTracingInterfaceVariable *findInterface(
    const RayTracingEntryPointReflection &entry,
    RayTracingInterfaceStorage storage) {
    for (const auto &interface : entry.interfaces) {
        if (interface.storage == storage) {
            return &interface;
        }
    }
    return nullptr;
}

std::size_t interfaceCount(
    const RayTracingEntryPointReflection &entry,
    RayTracingInterfaceStorage storage) {
    std::size_t count = 0;
    for (const auto &interface : entry.interfaces) {
        if (interface.storage == storage) ++count;
    }
    return count;
}

std::string occurrenceName(
    const RayTracingShaderOccurrence &occurrence) {
    return "shader '" + std::string{occurrence.diagnostic_uri} +
           "' entry '" +
           (occurrence.entry ? occurrence.entry->key.name : "<missing>") +
           "' group=" + std::to_string(occurrence.group_index);
}

// EB-7: a hit slot's ray type and group (slot = group * ray_types + ray) in
// a layout pipeline; nothing without a layout.
void addHitSlotFields(nlohmann::json &fields, const RayGroupLayout *layout,
                      std::size_t hit_slot) {
    if (layout == nullptr || layout->ray_types.empty()) return;
    const auto rays = layout->ray_types.size();
    fields["ray_type"] = layout->ray_types[hit_slot % rays];
    if (hit_slot / rays < layout->groups.size())
        fields["group"] = layout->groups[hit_slot / rays];
}

// EB-3: where a refused occurrence sits, as RayTracingRefusal fields: its
// stage and shader, and in a layout pipeline the ray type and group of its
// hit slot (slot = group * ray_types + ray) or the name of its miss. The
// compute task registration adds the task (it alone knows it).
nlohmann::json occurrenceFields(
    const RayTracingShaderOccurrence &occurrence,
    const RayGroupLayout *layout,
    std::optional<std::size_t> hit_slot,
    std::optional<std::size_t> miss = std::nullopt) {
    nlohmann::json fields{
        {"stage", shaderStageName(occurrence.stage)},
        {"shader", std::string{occurrence.diagnostic_uri}}};
    if (layout == nullptr) return fields;
    if (hit_slot) addHitSlotFields(fields, layout, *hit_slot);
    if (miss && *miss < layout->misses.size())
        fields["miss"] = layout->misses[*miss];
    return fields;
}

// EB-7: the "<code>" of a "<code>: <words>" message, when the words before
// the first ": " are a versioned code (pelican.x@1); empty otherwise.
std::string messageCode(std::string_view message) {
    const auto colon = message.find(": ");
    if (colon == std::string_view::npos) return {};
    const auto code = message.substr(0, colon);
    if (!code.starts_with("pelican.") || code.find('@') == std::string_view::npos)
        return {};
    return std::string{code};
}

// EB-7: a caller's call site, as a refusal of the caller names it.
nlohmann::json callerFields(const RayTracingShaderOccurrence &caller,
                            std::uint32_t instruction) {
    auto fields = occurrenceFields(caller, nullptr, std::nullopt);
    fields["instruction"] = instruction;
    return fields;
}

nlohmann::json describedType(const RayTracingCanonicalType *type) {
    return type ? nlohmann::json(describeRayTracingCanonicalType(*type))
                : nlohmann::json(nullptr);
}

// EB-3: fields name the shader that receives the payload (stage, shader and
// its slot; with no shader in the slot, stage alone and shader null), the
// caller, expected = the caller's outgoing type, actual = the receiver's
// incoming type (null when it declares none).
[[noreturn]] void failPayload(
    const RayTracingShaderOccurrence &caller,
    const RayTracingTraceCall &trace, std::string_view domain,
    std::uint32_t target_index,
    const RayTracingCanonicalType *outgoing,
    const RayTracingCanonicalType *incoming,
    nlohmann::json target_fields) {
    target_fields["caller"] = {
        {"stage", shaderStageName(caller.stage)},
        {"shader", std::string{caller.diagnostic_uri}},
        {"instruction", trace.instruction_number}};
    target_fields["expected"] = describedType(outgoing);
    target_fields["actual"] = describedType(incoming);
    throw RayTracingRefusal(
        "pelican.ray_tracing.payload_mismatch@1", std::move(target_fields),
        "pelican.ray_tracing.payload_mismatch@1: caller " +
        occurrenceName(caller) + " instruction=" +
        std::to_string(trace.instruction_number) + " " +
        std::string{domain} + "_group=" +
        std::to_string(target_index) + " outgoing=" +
        (outgoing ? describeRayTracingCanonicalType(*outgoing)
                  : "<missing>") +
        " incoming=" +
        (incoming ? describeRayTracingCanonicalType(*incoming)
                  : "<missing>"));
}

// EB-3: the anyhit is the refused shader; expected is its closest hit's
// incoming type, which is the slot's payload.
[[noreturn]] void failHitPairPayload(
    std::size_t hit_slot,
    const RayTracingShaderOccurrence &closest_hit,
    const RayTracingShaderOccurrence &any_hit,
    const RayTracingCanonicalType *closest_incoming,
    const RayTracingCanonicalType *any_incoming,
    const RayGroupLayout *layout) {
    auto fields = occurrenceFields(any_hit, layout, hit_slot);
    fields["expected"] = describedType(closest_incoming);
    fields["actual"] = describedType(any_incoming);
    fields["closesthit"] = std::string{closest_hit.diagnostic_uri};
    throw RayTracingRefusal(
        "pelican.ray_tracing.payload_mismatch@1", std::move(fields),
        "pelican.ray_tracing.payload_mismatch@1: physical hit_slot=" +
        std::to_string(hit_slot) + " closesthit " +
        occurrenceName(closest_hit) + " incoming=" +
        (closest_incoming
             ? describeRayTracingCanonicalType(*closest_incoming)
             : "<missing>") +
        " anyhit " + occurrenceName(any_hit) + " incoming=" +
        (any_incoming
             ? describeRayTracingCanonicalType(*any_incoming)
             : "<missing>"));
}

bool isTriangleHitAttribute(
    const RayTracingCanonicalType &type) {
    return type.kind == RayTracingCanonicalTypeKind::vector &&
           type.element_count == 2 && type.elements.size() == 1 &&
           type.elements.front().kind ==
               RayTracingCanonicalTypeKind::floating &&
           type.elements.front().bit_width == 32;
}

RayTracingCanonicalType canonicalRecordType(RayRecordType type) {
    RayTracingCanonicalType scalar;
    scalar.bit_width = 32;
    switch (type) {
    case RayRecordType::i32:
        scalar.kind = RayTracingCanonicalTypeKind::signed_integer;
        return scalar;
    case RayRecordType::u32:
        scalar.kind = RayTracingCanonicalTypeKind::unsigned_integer;
        return scalar;
    case RayRecordType::f32:
        scalar.kind = RayTracingCanonicalTypeKind::floating;
        return scalar;
    case RayRecordType::vec2:
    case RayRecordType::vec3:
    case RayRecordType::vec4: {
        scalar.kind = RayTracingCanonicalTypeKind::floating;
        RayTracingCanonicalType vector;
        vector.kind = RayTracingCanonicalTypeKind::vector;
        vector.element_count = type == RayRecordType::vec2
                                   ? 2u
                                   : type == RayRecordType::vec3 ? 3u : 4u;
        vector.elements.push_back(std::move(scalar));
        return vector;
    }
    }
    throw std::logic_error("unknown ray record type");
}

// EB-3: expected = the record the occurrence may declare, in order,
// [{name, offset, type}] (null: it may declare none, as no record bytes are
// written for it); actual = what it declares, [{offset, type}]. Types are
// spelled canonically on both sides (u32, f32, vec3<f32>).
[[noreturn]] void failShaderRecord(
    const RayTracingShaderOccurrence &occurrence,
    nlohmann::json fields,
    const RayTracingShaderRecordReflection &actual,
    const std::span<const RayRecordMemberLayout> *expected,
    std::string message) {
    auto expected_json = nlohmann::json(nullptr);
    if (expected != nullptr) {
        expected_json = nlohmann::json::array();
        for (const auto &member : *expected)
            expected_json.push_back(
                {{"name", member.name},
                 {"offset", member.offset},
                 {"type", describeRayTracingCanonicalType(
                              canonicalRecordType(member.type))}});
    }
    auto actual_json = nlohmann::json::array();
    for (const auto &member : actual.members)
        actual_json.push_back(
            {{"offset", member.offset},
             {"type", describeRayTracingCanonicalType(member.type)}});
    fields["expected"] = std::move(expected_json);
    fields["actual"] = std::move(actual_json);
    throw RayTracingRefusal(
        "pelican.ray_tracing.shader_record_abi_mismatch@1", std::move(fields),
        "pelican.ray_tracing.shader_record_abi_mismatch@1: " +
            occurrenceName(occurrence) + " " + std::move(message));
}

void validateRecordMembers(
    const RayTracingShaderOccurrence &occurrence,
    const RayTracingShaderRecordReflection &actual,
    std::span<const RayRecordMemberLayout> expected,
    nlohmann::json fields) {
    if (actual.members.size() != expected.size()) {
        failShaderRecord(
            occurrence, std::move(fields), actual, &expected, "member_count=" +
                            std::to_string(actual.members.size()) +
                            " expected=" +
                            std::to_string(expected.size()));
    }
    for (std::size_t member = 0;
         member < expected.size(); ++member) {
        const auto expected_type =
            canonicalRecordType(expected[member].type);
        if (actual.members[member].offset !=
                expected[member].offset ||
            actual.members[member].type != expected_type) {
            failShaderRecord(
                occurrence, std::move(fields), actual, &expected,
                "member=" + std::to_string(member) +
                                " offset=" +
                                std::to_string(actual.members[member].offset) +
                                " expected_offset=" +
                                std::to_string(expected[member].offset) +
                                " type=" +
                                describeRayTracingCanonicalType(
                                    actual.members[member].type) +
                                " expected_type=" +
                                describeRayTracingCanonicalType(
                                    expected_type));
        }
    }
}

void hashAppend(std::uint64_t &hash, std::string_view text) {
    for (const auto byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
}

} // namespace

void validateRayTracingRequest(
    std::uint32_t requested_depth,
    const RayTracingRuntimeFacts &facts) {
    if (requested_depth == 0) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.invalid_recursion@1",
            {{"requested", requested_depth}, {"limit", facts.max_ray_recursion_depth}},
            "pelican.ray_tracing.invalid_recursion@1: "
            "max_recursion requested=0; valid domain is [1, " +
            std::to_string(facts.max_ray_recursion_depth) + "]");
    }
    if (requested_depth > facts.max_ray_recursion_depth) {
        throw RayTracingRefusal(
            "pelican.ray_tracing.recursion_limit@1",
            {{"requested", requested_depth}, {"limit", facts.max_ray_recursion_depth}},
            "pelican.ray_tracing.recursion_limit@1: property "
            "maxRayRecursionDepth requested=" +
            std::to_string(requested_depth) + " actual=" +
            std::to_string(facts.max_ray_recursion_depth) +
            " device='" + deviceName(facts) + "'");
    }
}

void validateTraceExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts) {
    for (std::size_t dimension = 0; dimension < extent.size();
         ++dimension) {
        if (extent[dimension] == 0 ||
            extent[dimension] >
                facts.max_dispatch_dimensions[dimension]) {
            failDimension(dimension, extent[dimension],
                          facts.max_dispatch_dimensions[dimension], facts);
        }
    }

    std::uint64_t product = 1;
    for (const auto dimension : extent) {
        if (product >
            std::numeric_limits<std::uint64_t>::max() / dimension) {
            throw std::runtime_error(
                "pelican.ray_tracing.dispatch_invocation_limit@1: "
                "trace extent product overflow for " +
                std::to_string(extent[0]) + "x" +
                std::to_string(extent[1]) + "x" +
                std::to_string(extent[2]) + " device='" +
                deviceName(facts) + "'");
        }
        product *= dimension;
    }
    if (product > facts.max_ray_dispatch_invocation_count) {
        throw std::runtime_error(
            "pelican.ray_tracing.dispatch_invocation_limit@1: property "
            "maxRayDispatchInvocationCount requested=" +
            std::to_string(product) + " actual=" +
            std::to_string(
                facts.max_ray_dispatch_invocation_count) +
            " extent=" + std::to_string(extent[0]) + "x" +
            std::to_string(extent[1]) + "x" +
            std::to_string(extent[2]) + " device='" +
            deviceName(facts) + "'");
    }
}

std::string validateRayTracingPipelineInterfaces(
    const RayTracingPipelineInterfaceRequest &request,
    const RayTracingRuntimeFacts &facts) {
    if (!request.hit_groups.empty() &&
        !request.hit_group_occurrences.empty()) {
        throw std::logic_error(
            "ray tracing validation received both legacy and paired hit groups");
    }
    std::vector<RayTracingShaderOccurrence> hit_groups;
    std::vector<std::optional<RayTracingShaderOccurrence>> any_hits;
    std::vector<std::optional<RayTracingShaderOccurrence>> intersections;
    if (!request.hit_group_occurrences.empty()) {
        hit_groups.reserve(request.hit_group_occurrences.size());
        any_hits.reserve(request.hit_group_occurrences.size());
        intersections.reserve(request.hit_group_occurrences.size());
        for (const auto &pair : request.hit_group_occurrences) {
            hit_groups.push_back(pair.closest_hit);
            any_hits.push_back(pair.any_hit);
            intersections.push_back(pair.intersection);
        }
    } else {
        hit_groups.assign(request.hit_groups.begin(), request.hit_groups.end());
        any_hits.resize(hit_groups.size());
        intersections.resize(hit_groups.size());
    }
    // EB-7: every refusal below is a RayTracingRefusal with where it is
    // (occurrenceFields), as EB-3's record and payload refusals; the
    // messages are the ones they always had.
    const auto validate_occurrence = [](const auto &occurrence,
                                        nlohmann::json where) {
        if (occurrence.entry == nullptr) {
            throw RayTracingRefusal(
                "pelican.ray_tracing.interface_type_unsupported@1", std::move(where),
                "pelican.ray_tracing.interface_type_unsupported@1: " +
                occurrenceName(occurrence) +
                " has no selected (expected stage, 'main') entry");
        }
        for (const auto storage : {
                 RayTracingInterfaceStorage::incoming_ray_payload,
                 RayTracingInterfaceStorage::incoming_callable_data,
                 RayTracingInterfaceStorage::hit_attribute}) {
            if (interfaceCount(*occurrence.entry, storage) > 1) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.interface_count@1", std::move(where),
                    "pelican.ray_tracing.interface_count@1: " +
                    occurrenceName(occurrence) +
                    " declares more than one incoming interface of kind " +
                    std::to_string(static_cast<unsigned>(storage)));
            }
        }
    };
    validate_occurrence(request.raygen,
                        occurrenceFields(request.raygen, request.group_layout, std::nullopt));
    for (std::size_t index = 0; index < request.misses.size(); ++index)
        validate_occurrence(request.misses[index],
                            occurrenceFields(request.misses[index], request.group_layout,
                                             std::nullopt, index));
    for (const auto &occurrence : request.callables) {
        validate_occurrence(occurrence, occurrenceFields(occurrence, nullptr, std::nullopt));
        const auto &record = occurrence.entry->shader_record;
        if (record) {
            if (request.callable_record_layout) {
                validateRecordMembers(occurrence, *record,
                                      request.callable_record_layout->members,
                                      occurrenceFields(occurrence, nullptr, std::nullopt));
            } else if (!record->members.empty()) {
                throw RayTracingRefusal(
                    "pelican.ray_tracing.callable_record_unsupported@1",
                    occurrenceFields(occurrence, nullptr, std::nullopt),
                    "pelican.ray_tracing.callable_record_unsupported@1: " +
                    occurrenceName(occurrence) +
                    " declares shader-record data without a callable_record schema");
            }
        }
    }
    const auto validate_triangle_hit_stage = [&](const auto &occurrence,
                                                 std::size_t hit_slot) {
        auto where = occurrenceFields(occurrence, request.group_layout, hit_slot);
        validate_occurrence(occurrence, where);
        for (const auto &interface : occurrence.entry->interfaces) {
            if (interface.storage !=
                RayTracingInterfaceStorage::hit_attribute) {
                continue;
            }
            if (!isTriangleHitAttribute(interface.type)) {
                where["expected"] = "vec2<f32>";
                where["actual"] = describeRayTracingCanonicalType(interface.type);
                throw RayTracingRefusal(
                    "pelican.ray_tracing.hit_attribute_forbidden@1", std::move(where),
                    "pelican.ray_tracing.hit_attribute_forbidden@1: " +
                    occurrenceName(occurrence) +
                    " triangle hit attribute must be vec2<f32>; actual=" +
                    describeRayTracingCanonicalType(interface.type));
            }
            if (interface.byte_size >
                facts.max_ray_hit_attribute_size) {
                where["bytes"] = interface.byte_size;
                where["limit"] = facts.max_ray_hit_attribute_size;
                throw RayTracingRefusal(
                    "pelican.ray_tracing.hit_attribute_size@1", std::move(where),
                    "pelican.ray_tracing.hit_attribute_size@1: " +
                    occurrenceName(occurrence) +
                    " hit attribute bytes=" +
                    std::to_string(interface.byte_size) +
                    " exceeds maxRayHitAttributeSize=" +
                    std::to_string(
                        facts.max_ray_hit_attribute_size) +
                    " device='" + deviceName(facts) + "'");
            }
        }
    };
    for (std::size_t index = 0; index < hit_groups.size(); ++index) {
        bool procedural = false;
        if (request.group_layout != nullptr &&
            !request.group_layout->ray_types.empty() &&
            index / request.group_layout->ray_types.size() <
                request.group_layout->group_types.size()) {
            procedural = request.group_layout->group_types[
                             index /
                             request.group_layout->ray_types.size()] ==
                         RayGroupType::procedural;
        }
        const auto where = [&](const RayTracingShaderOccurrence &occurrence) {
            return occurrenceFields(occurrence, request.group_layout, index);
        };
        if (procedural) {
            validate_occurrence(hit_groups[index], where(hit_groups[index]));
            if (any_hits[index]) validate_occurrence(*any_hits[index], where(*any_hits[index]));
            if (!intersections[index]) {
                // As the task parser names the same code: the slot.
                auto slot = nlohmann::json::object();
                addHitSlotFields(slot, request.group_layout, index);
                throw RayTracingRefusal(
                    "pelican.procedural_group_requires_intersection@1", std::move(slot),
                    "pelican.procedural_group_requires_intersection@1: procedural hit group has no intersection stage");
            }
            validate_occurrence(*intersections[index], where(*intersections[index]));
            const auto *intersection_attribute = findInterface(
                *intersections[index]->entry,
                RayTracingInterfaceStorage::hit_attribute);
            const auto validate_procedural_attribute = [&](const auto &stage) {
                const auto *attribute = findInterface(
                    *stage.entry, RayTracingInterfaceStorage::hit_attribute);
                if (!intersection_attribute || !attribute ||
                    attribute->type != intersection_attribute->type) {
                    // EB-7: the refused stage; expected = the intersection's
                    // hit attribute, actual = the stage's (null: none
                    // declared), and the intersection that sets it.
                    auto fields = where(stage);
                    fields["expected"] = intersection_attribute
                                             ? nlohmann::json(describeRayTracingCanonicalType(
                                                   intersection_attribute->type))
                                             : nlohmann::json(nullptr);
                    fields["actual"] = attribute ? nlohmann::json(describeRayTracingCanonicalType(attribute->type))
                                                 : nlohmann::json(nullptr);
                    fields["intersection"] = std::string{intersections[index]->diagnostic_uri};
                    throw RayTracingRefusal(
                        "pelican.procedural_hit_attribute_mismatch@1", std::move(fields),
                        "pelican.procedural_hit_attribute_mismatch@1: " +
                        occurrenceName(stage) + " must declare the same hit attribute as " +
                        occurrenceName(*intersections[index]));
                }
                if (attribute->byte_size > facts.max_ray_hit_attribute_size) {
                    auto fields = where(stage);
                    fields["bytes"] = attribute->byte_size;
                    fields["limit"] = facts.max_ray_hit_attribute_size;
                    throw RayTracingRefusal(
                        "pelican.procedural_hit_attribute_too_large@1", std::move(fields),
                        "pelican.procedural_hit_attribute_too_large@1: " +
                        occurrenceName(stage) + " hit attribute bytes=" +
                        std::to_string(attribute->byte_size) +
                        " exceeds maxRayHitAttributeSize=" +
                        std::to_string(facts.max_ray_hit_attribute_size) +
                        " device='" + deviceName(facts) + "'");
                }
            };
            validate_procedural_attribute(hit_groups[index]);
            if (any_hits[index]) validate_procedural_attribute(*any_hits[index]);
        } else {
            if (intersections[index]) {
                // As the task parser names the same code: the slot and the
                // stage, here with the shader too.
                throw RayTracingRefusal(
                    "pelican.ray_tracing.unsupported_shader_stage@1", where(*intersections[index]),
                    "pelican.ray_tracing.unsupported_shader_stage@1: triangles hit group has an intersection stage");
            }
            validate_triangle_hit_stage(hit_groups[index], index);
            if (any_hits[index]) {
                validate_triangle_hit_stage(*any_hits[index], index);
            }
        }
    }

    if (request.group_layout != nullptr &&
        request.group_layout->usesMaterialAlphaProfile()) {
        constexpr std::uint32_t opaque_ray_flag = 0x1u;
        for (const auto &trace : request.raygen.entry->trace_calls) {
            if (!trace.ray_flags ||
                ((*trace.ray_flags & opaque_ray_flag) != 0)) {
                auto fields = callerFields(request.raygen, trace.instruction_number);
                fields["ray_flags"] = trace.ray_flags ? nlohmann::json(*trace.ray_flags)
                                                      : nlohmann::json(nullptr);
                throw RayTracingRefusal(
                    "pelican.ray_profile_opaque_trace@1", std::move(fields),
                    "pelican.ray_profile_opaque_trace@1: shader '" +
                    std::string{request.raygen.diagnostic_uri} +
                    "' instruction=" +
                    std::to_string(trace.instruction_number) +
                    " rayFlags=" +
                    (trace.ray_flags
                         ? std::to_string(*trace.ray_flags)
                         : std::string{"<non-constant>"}) +
                    " requires constant non-Opaque flags for portable_profile 'material_alpha_v1'");
            }
        }
    }

    // A physical hit-group pair has one canonical incoming payload type even
    // when no OpTraceRayKHR currently reaches that ray slot.  Establish that
    // invariant before trace-call routing; the latter then compares outgoing
    // payloads with this already-validated canonical type.
    std::vector<const RayTracingCanonicalType *> hit_payload_types;
    hit_payload_types.reserve(hit_groups.size());
    for (std::size_t index = 0; index < hit_groups.size(); ++index) {
        const auto *closest_incoming = findInterface(
            *hit_groups[index].entry,
            RayTracingInterfaceStorage::incoming_ray_payload);
        if (any_hits[index]) {
            const auto *any_incoming = findInterface(
                *any_hits[index]->entry,
                RayTracingInterfaceStorage::incoming_ray_payload);
            const auto generated_omission =
                any_hits[index]->engine_generated &&
                any_incoming == nullptr;
            if (!generated_omission &&
                ((closest_incoming == nullptr) != (any_incoming == nullptr) ||
                (closest_incoming != nullptr &&
                 closest_incoming->type != any_incoming->type))) {
                failHitPairPayload(
                    index, hit_groups[index], *any_hits[index],
                    closest_incoming
                        ? &closest_incoming->type
                        : nullptr,
                    any_incoming ? &any_incoming->type : nullptr,
                    request.group_layout);
            }
        }
        hit_payload_types.push_back(
            closest_incoming ? &closest_incoming->type : nullptr);
    }

    if (request.group_layout != nullptr) {
        const auto &layout = *request.group_layout;
        const auto ray_count = layout.ray_types.size();
        if (hit_groups.size() !=
            layout.groups.size() * ray_count) {
            throw std::runtime_error(
                "pelican.ray_tracing.hit_grid_invalid@1: hit group count does not equal groups*ray_types");
        }
        // EB-3: a stage that declares no shader record reads none of it, so
        // leaving it undeclared is always safe (as for an intersection or a
        // callable): a traced ray type's hit shader no longer has to declare
        // a record it does not use. Where record bytes are written (a traced
        // hit slot), a declared record must be the layout's physical record
        // exactly. Raygen, miss and untraced slots get no record bytes
        // (packRayTracingHitTable), so storage there is refused. A task
        // cannot name an untraced row (hit_shader_untraced), so in production
        // only the engine's no-op closest hit and no-report intersection sit
        // there.
        const auto validate_record =
            [&](const RayTracingShaderOccurrence &occurrence,
                bool allowed, nlohmann::json fields) {
                const auto &actual = occurrence.entry->shader_record;
                if (!actual) return;
                if (!allowed) {
                    failShaderRecord(
                        occurrence, std::move(fields), *actual, nullptr,
                        "declares shader-record storage where it is forbidden");
                }
                validateRecordMembers(occurrence, *actual,
                                      layout.physical_record, std::move(fields));
            };
        validate_record(request.raygen, false,
                        occurrenceFields(request.raygen, &layout, std::nullopt));
        for (std::size_t index = 0; index < request.misses.size(); ++index)
            validate_record(request.misses[index], false,
                            occurrenceFields(request.misses[index], &layout,
                                             std::nullopt, index));
        for (std::size_t index = 0; index < hit_groups.size(); ++index) {
            const auto ray = static_cast<std::uint32_t>(index % ray_count);
            const auto traced =
                std::find(request.traced_ray_type_indices.begin(),
                          request.traced_ray_type_indices.end(), ray) !=
                request.traced_ray_type_indices.end();
            validate_record(hit_groups[index], traced,
                            occurrenceFields(hit_groups[index], &layout, index));
            if (any_hits[index]) {
                validate_record(*any_hits[index], traced,
                                occurrenceFields(*any_hits[index], &layout, index));
            }
            if (intersections[index]) {
                validate_record(*intersections[index], traced,
                                occurrenceFields(*intersections[index], &layout, index));
            }
        }
    }

    const auto validate_trace = [&](const auto &caller,
                                    const auto &trace) {
        const auto *outgoing = &trace.payload_type;
        // EB-3: a slot no shader fills is named by its stage alone.
        const auto empty_slot = [](ShaderStage stage) {
            return nlohmann::json{{"stage", shaderStageName(stage)},
                                  {"shader", nullptr}};
        };
        const auto compare_target = [&](std::string_view domain,
                                        std::uint32_t index,
                                        auto targets) {
            if (index >= targets.size()) {
                failPayload(caller, trace, domain, index,
                            outgoing, nullptr, empty_slot(ShaderStage::miss));
            }
            const auto &target = targets[index];
            const auto *incoming = findInterface(
                *target.entry,
                RayTracingInterfaceStorage::incoming_ray_payload);
            if (incoming == nullptr || *outgoing != incoming->type) {
                failPayload(caller, trace, domain, index,
                            outgoing,
                            incoming ? &incoming->type : nullptr,
                            occurrenceFields(target, request.group_layout,
                                             std::nullopt, index));
            }
        };
        const auto compare_hit_target = [&](std::uint32_t index) {
            if (index >= hit_groups.size()) {
                failPayload(caller, trace, "hit", index,
                            outgoing, nullptr, empty_slot(ShaderStage::closesthit));
            }
            const auto *incoming = hit_payload_types[index];
            if (incoming == nullptr || *outgoing != *incoming) {
                failPayload(caller, trace, "hit", index, outgoing,
                            incoming,
                            occurrenceFields(hit_groups[index],
                                             request.group_layout, index));
            }
        };
        if (request.group_layout == nullptr) {
            // RT1a legacy: instance SBT offset and geometry index are zero.
            compare_target("miss", trace.miss_index,
                           request.misses);
            compare_hit_target(trace.sbt_record_offset);
            return;
        }
        if (!record_addressing_validation_enabled) return;
        RayTracingRecordAddressing addressing{
            .record_index_stride = static_cast<std::uint32_t>(
                request.group_layout->ray_types.size()),
            .traced_ray_type_indices = {
                request.traced_ray_type_indices.begin(),
                request.traced_ray_type_indices.end()},
        };
        try {
            validateRayTracingRecordAddressing(
                trace.sbt_record_offset, trace.sbt_record_stride,
                trace.miss_index, *request.group_layout, addressing);
        } catch (const RayTracingRefusal &) {
            throw;
        } catch (const std::runtime_error &error) {
            // EB-7: the layout's addressing refusal names the trace call
            // that made it; the words stay the layout's.
            const auto code = messageCode(error.what());
            if (code.empty()) throw;
            auto fields = callerFields(caller, trace.instruction_number);
            fields["sbt_record_offset"] = trace.sbt_record_offset;
            fields["sbt_record_stride"] = trace.sbt_record_stride;
            fields["miss_index"] = trace.miss_index;
            throw RayTracingRefusal(code, std::move(fields), error.what());
        }
        compare_target("miss", trace.miss_index,
                       request.misses);
        const auto ray_count = request.group_layout->ray_types.size();
        for (std::size_t group = 0;
             group < request.group_layout->groups.size(); ++group) {
            compare_hit_target(static_cast<std::uint32_t>(
                group * ray_count + trace.sbt_record_offset));
        }
    };

    const auto validate_traces = [&](const auto &caller) {
        for (const auto &trace : caller.entry->trace_calls) {
            validate_trace(caller, trace);
        }
        for (const auto &call : caller.entry->callable_calls) {
            const auto compare = [&](std::size_t index) {
                if (index >= request.callables.size()) {
                    auto fields = callerFields(caller, call.instruction_number);
                    fields["callable_index"] = index;
                    fields["callables"] = request.callables.size();
                    throw RayTracingRefusal(
                        "pelican.ray_tracing.callable_index_out_of_range@1", std::move(fields),
                        "pelican.ray_tracing.callable_index_out_of_range@1: " +
                        occurrenceName(caller) + " instruction " +
                        std::to_string(call.instruction_number) + " callable index=" +
                        std::to_string(index) + " table size=" +
                        std::to_string(request.callables.size()));
                }
                const auto &target = request.callables[index];
                const auto *incoming = findInterface(*target.entry,
                    RayTracingInterfaceStorage::incoming_callable_data);
                if (!incoming || incoming->type != call.data_type) {
                    // As payload_mismatch: the callable that receives it,
                    // expected = the caller's data, actual = its incoming.
                    auto fields = occurrenceFields(target, nullptr, std::nullopt);
                    fields["callable_index"] = index;
                    fields["expected"] = describeRayTracingCanonicalType(call.data_type);
                    fields["actual"] = incoming ? nlohmann::json(describeRayTracingCanonicalType(incoming->type))
                                                : nlohmann::json(nullptr);
                    fields["caller"] = callerFields(caller, call.instruction_number);
                    throw RayTracingRefusal(
                        "pelican.ray_tracing.callable_data_mismatch@1", std::move(fields),
                        "pelican.ray_tracing.callable_data_mismatch@1: " +
                        occurrenceName(caller) + " instruction " +
                        std::to_string(call.instruction_number) + " calls " +
                        occurrenceName(target) + " with " +
                        describeRayTracingCanonicalType(call.data_type) +
                        "; incoming=" + (incoming ?
                            describeRayTracingCanonicalType(incoming->type) : "<missing>"));
                }
            };
            if (call.callable_index) {
                compare(*call.callable_index);
            } else {
                // The shader owns dynamic index bounds, as for other shader
                // array accesses. Validate every possible table interface.
                if (request.callables.empty()) compare(0);
                for (std::size_t index = 0; index < request.callables.size(); ++index)
                    compare(index);
            }
        }
    };
    validate_traces(request.raygen);
    for (const auto &occurrence : request.misses)
        validate_traces(occurrence);
    for (const auto &occurrence : request.callables)
        validate_traces(occurrence);
    for (std::size_t index = 0; index < hit_groups.size(); ++index) {
        validate_traces(hit_groups[index]);
        if (any_hits[index]) validate_traces(*any_hits[index]);
        if (intersections[index]) validate_traces(*intersections[index]);
    }

    std::uint64_t hash = 14695981039346656037ull;
    const auto digest_occurrence = [&](const auto &occurrence) {
        hashAppend(hash, shaderStageName(occurrence.stage));
        hashAppend(hash, occurrence.diagnostic_uri);
        hashAppend(hash, std::to_string(occurrence.group_index));
        for (const auto &interface : occurrence.entry->interfaces) {
            hashAppend(hash,
                       std::to_string(static_cast<unsigned>(
                           interface.storage)));
            hashAppend(hash, interface.location
                                 ? std::to_string(*interface.location)
                                 : "<none>");
            hashAppend(hash,
                       describeRayTracingCanonicalType(interface.type));
        }
    };
    digest_occurrence(request.raygen);
    for (const auto &occurrence : request.misses)
        digest_occurrence(occurrence);
    for (const auto &occurrence : request.callables)
        digest_occurrence(occurrence);
    for (std::size_t index = 0; index < hit_groups.size(); ++index) {
        digest_occurrence(hit_groups[index]);
        if (any_hits[index]) digest_occurrence(*any_hits[index]);
        if (intersections[index]) digest_occurrence(*intersections[index]);
    }

    const auto digest_target = [&](std::string_view domain,
                                   std::uint32_t resolved_index,
                                   const auto &target) {
        hashAppend(hash, domain);
        hashAppend(hash, std::to_string(resolved_index));
        hashAppend(hash, shaderStageName(target.stage));
        hashAppend(hash, target.diagnostic_uri);
        hashAppend(hash, std::to_string(target.group_index));
    };
    const auto digest_indexed_target =
        [&](std::string_view domain,
            std::uint32_t resolved_index,
            const auto &targets) {
            if (resolved_index >= targets.size()) {
                // The production validator rejects this before digesting.
                // The mutation seam deliberately bypasses that gate, so it
                // still needs a deterministic, bounds-safe digest on the
                // way to the real PipelineFactory create attempt.
                hashAppend(hash, domain);
                hashAppend(hash,
                           std::to_string(resolved_index));
                hashAppend(hash,
                           "<unvalidated-out-of-range>");
                return;
            }
            digest_target(domain, resolved_index,
                          targets[resolved_index]);
        };
    const auto digest_indexed_hit_target =
        [&](std::uint32_t resolved_index) {
            digest_indexed_target("hitTarget", resolved_index, hit_groups);
            if (resolved_index < any_hits.size() && any_hits[resolved_index]) {
                digest_target("anyHitTarget", resolved_index,
                              *any_hits[resolved_index]);
            }
        };
    const auto digest_traces = [&](const auto &caller) {
        for (const auto &call : caller.entry->callable_calls) {
            hashAppend(hash, "executeCallable");
            hashAppend(hash, std::to_string(call.instruction_number));
            hashAppend(hash, describeRayTracingCanonicalType(call.data_type));
            hashAppend(hash, call.callable_index ?
                std::to_string(*call.callable_index) : "<dynamic>");
            if (call.callable_index) {
                digest_indexed_target("callableTarget", *call.callable_index, request.callables);
            } else {
                for (std::uint32_t index = 0; index < request.callables.size(); ++index)
                    digest_indexed_target("callableTarget", index, request.callables);
            }
        }
        for (const auto &trace : caller.entry->trace_calls) {
            hashAppend(hash, "trace");
            hashAppend(hash,
                       std::to_string(trace.instruction_number));
            if (request.group_layout != nullptr &&
                request.group_layout->usesMaterialAlphaProfile()) {
                hashAppend(hash, "rayFlags");
                hashAppend(hash,
                           trace.ray_flags
                               ? std::to_string(*trace.ray_flags)
                               : "<non-constant>");
            }
            hashAppend(hash, "missIndex");
            hashAppend(hash, std::to_string(trace.miss_index));
            digest_indexed_target(
                "missTarget", trace.miss_index,
                request.misses);
            hashAppend(hash, "sbtRecordOffset");
            hashAppend(hash,
                       std::to_string(trace.sbt_record_offset));
            hashAppend(hash, "sbtRecordStride");
            hashAppend(hash,
                       std::to_string(trace.sbt_record_stride));
            if (request.group_layout == nullptr) {
                digest_indexed_hit_target(trace.sbt_record_offset);
            } else {
                const auto ray_count =
                    request.group_layout->ray_types.size();
                for (std::size_t group = 0;
                     group < request.group_layout->groups.size(); ++group) {
                    const auto resolved = static_cast<std::uint32_t>(
                        group * ray_count + trace.sbt_record_offset);
                    digest_indexed_hit_target(resolved);
                }
            }
            hashAppend(hash, "tracePayloadType");
            hashAppend(hash,
                       describeRayTracingCanonicalType(
                           trace.payload_type));
        }
    };
    digest_traces(request.raygen);
    for (const auto &occurrence : request.misses)
        digest_traces(occurrence);
    for (const auto &occurrence : request.callables)
        digest_traces(occurrence);
    for (std::size_t index = 0; index < hit_groups.size(); ++index) {
        digest_traces(hit_groups[index]);
        if (any_hits[index]) digest_traces(*any_hits[index]);
        if (intersections[index]) digest_traces(*intersections[index]);
    }
    std::ostringstream result;
    result << (request.group_layout ? "rt1b-fnv1a64:"
                                    : "rt1a-fnv1a64:")
           << std::hex << std::setw(16)
           << std::setfill('0') << hash;
    return result.str();
}

ScopedRayTracingValidationObserver::
    ScopedRayTracingValidationObserver(
        RayTracingValidationObserver observer)
    : previous_{std::move(validation_observer)} {
    validation_observer = std::move(observer);
}

ScopedRayTracingValidationObserver::~ScopedRayTracingValidationObserver() {
    validation_observer = std::move(previous_);
}

ScopedRayTracingValidationDelegateOverrideForTesting::
    ScopedRayTracingValidationDelegateOverrideForTesting(
        RayTracingValidationSite site, bool enabled)
    : site_{site},
      previous_{validation_delegate_enabled[siteIndex(site)]} {
    validation_delegate_enabled[siteIndex(site)] = enabled;
}

ScopedRayTracingValidationDelegateOverrideForTesting::
    ~ScopedRayTracingValidationDelegateOverrideForTesting() {
    validation_delegate_enabled[siteIndex(site_)] = previous_;
}

ScopedRayTracingRecordAddressingValidationOverrideForTesting::
    ScopedRayTracingRecordAddressingValidationOverrideForTesting(
        bool enabled)
    : previous_{record_addressing_validation_enabled} {
    record_addressing_validation_enabled = enabled;
}

ScopedRayTracingRecordAddressingValidationOverrideForTesting::
    ~ScopedRayTracingRecordAddressingValidationOverrideForTesting() {
    record_addressing_validation_enabled = previous_;
}

void validateProductionRayTracingRequest(
    std::uint32_t requested_depth,
    const RayTracingRuntimeFacts &facts) {
    if (validation_observer) {
        validation_observer({
            .site = RayTracingValidationSite::pipeline_build,
            .facts = facts,
            .requested_depth = requested_depth,
        });
    }
    validateRayTracingRequest(requested_depth, facts);
}

void validateProductionTraceExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts,
    RayTracingValidationSite site) {
    if (site == RayTracingValidationSite::pipeline_build) {
        throw std::logic_error(
            "pipeline_build is not a trace-extent validation site");
    }
    if (validation_observer) {
        validation_observer({
            .site = site,
            .facts = facts,
            .extent = extent,
        });
    }
    validateTraceExtent(extent, facts);
}

void validateRayTracingPipelineBuild(
    std::uint32_t requested_depth,
    const RayTracingRuntimeFacts &facts) {
    if (!validation_delegate_enabled[siteIndex(
            RayTracingValidationSite::pipeline_build)]) {
        return;
    }
    validateProductionRayTracingRequest(requested_depth, facts);
}

void validateRayTracingInitialExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts) {
    if (!validation_delegate_enabled[siteIndex(
            RayTracingValidationSite::initial_extent)]) {
        return;
    }
    validateProductionTraceExtent(
        extent, facts, RayTracingValidationSite::initial_extent);
}

void validateRayTracingRebindExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts) {
    if (!validation_delegate_enabled[siteIndex(
            RayTracingValidationSite::rebind)]) {
        return;
    }
    validateProductionTraceExtent(
        extent, facts, RayTracingValidationSite::rebind);
}

void validateRayTracingFinalCommandExtent(
    std::array<std::uint32_t, 3> extent,
    const RayTracingRuntimeFacts &facts) {
    if (!validation_delegate_enabled[siteIndex(
            RayTracingValidationSite::final_command)]) {
        return;
    }
    validateProductionTraceExtent(
        extent, facts, RayTracingValidationSite::final_command);
}

ScopedRayTracingPipelineCreateObserver::
    ScopedRayTracingPipelineCreateObserver(
        RayTracingPipelineCreateObserver observer)
    : previous_{std::move(create_observer)} {
    create_observer = std::move(observer);
}

ScopedRayTracingPipelineCreateObserver::~
    ScopedRayTracingPipelineCreateObserver() {
    create_observer = std::move(previous_);
}

void observeRayTracingPipelineCreate(
    std::uint32_t resolved_depth,
    std::string_view validator_digest,
    std::span<const std::uint32_t> hit_group_any_hit_shaders) {
    if (create_observer) {
        create_observer({
            .max_pipeline_ray_recursion_depth = resolved_depth,
            .validator_digest = std::string{validator_digest},
            .hit_group_any_hit_shaders = {
                hit_group_any_hit_shaders.begin(),
                hit_group_any_hit_shaders.end()},
        });
    }
}

} // namespace Pelican
