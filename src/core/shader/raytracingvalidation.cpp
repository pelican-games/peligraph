#include "raytracingvalidation.hpp"

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

[[noreturn]] void failPayload(
    const RayTracingShaderOccurrence &caller,
    const RayTracingTraceCall &trace, std::string_view domain,
    std::uint32_t target_index,
    const RayTracingCanonicalType *outgoing,
    const RayTracingCanonicalType *incoming) {
    throw std::runtime_error(
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

[[noreturn]] void failHitPairPayload(
    std::size_t hit_slot,
    const RayTracingShaderOccurrence &closest_hit,
    const RayTracingShaderOccurrence &any_hit,
    const RayTracingCanonicalType *closest_incoming,
    const RayTracingCanonicalType *any_incoming) {
    throw std::runtime_error(
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

[[noreturn]] void failShaderRecord(
    const RayTracingShaderOccurrence &occurrence,
    std::string message) {
    throw std::runtime_error(
        "pelican.ray_tracing.shader_record_abi_mismatch@1: " +
        occurrenceName(occurrence) + " " + std::move(message));
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
        throw std::runtime_error(
            "pelican.ray_tracing.invalid_recursion@1: "
            "max_recursion requested=0; valid domain is [1, " +
            std::to_string(facts.max_ray_recursion_depth) + "]");
    }
    if (requested_depth > facts.max_ray_recursion_depth) {
        throw std::runtime_error(
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
    const auto validate_occurrence = [](const auto &occurrence) {
        if (occurrence.entry == nullptr) {
            throw std::runtime_error(
                "pelican.ray_tracing.interface_type_unsupported@1: " +
                occurrenceName(occurrence) +
                " has no selected (expected stage, 'main') entry");
        }
        for (const auto storage : {
                 RayTracingInterfaceStorage::incoming_ray_payload,
                 RayTracingInterfaceStorage::hit_attribute}) {
            if (interfaceCount(*occurrence.entry, storage) > 1) {
                throw std::runtime_error(
                    "pelican.ray_tracing.interface_count@1: " +
                    occurrenceName(occurrence) +
                    " declares more than one incoming interface of kind " +
                    std::to_string(static_cast<unsigned>(storage)));
            }
        }
    };
    validate_occurrence(request.raygen);
    for (const auto &occurrence : request.misses)
        validate_occurrence(occurrence);
    const auto validate_triangle_hit_stage = [&](const auto &occurrence) {
        validate_occurrence(occurrence);
        for (const auto &interface : occurrence.entry->interfaces) {
            if (interface.storage !=
                RayTracingInterfaceStorage::hit_attribute) {
                continue;
            }
            if (!isTriangleHitAttribute(interface.type)) {
                throw std::runtime_error(
                    "pelican.ray_tracing.hit_attribute_forbidden@1: " +
                    occurrenceName(occurrence) +
                    " triangle hit attribute must be vec2<f32>; actual=" +
                    describeRayTracingCanonicalType(interface.type));
            }
            if (interface.byte_size >
                facts.max_ray_hit_attribute_size) {
                throw std::runtime_error(
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
        if (procedural) {
            validate_occurrence(hit_groups[index]);
            if (any_hits[index]) validate_occurrence(*any_hits[index]);
            if (!intersections[index]) {
                throw std::runtime_error(
                    "pelican.procedural_group_requires_intersection@1: procedural hit group has no intersection stage");
            }
            validate_occurrence(*intersections[index]);
            const auto *intersection_attribute = findInterface(
                *intersections[index]->entry,
                RayTracingInterfaceStorage::hit_attribute);
            const auto validate_procedural_attribute = [&](const auto &stage) {
                const auto *attribute = findInterface(
                    *stage.entry, RayTracingInterfaceStorage::hit_attribute);
                if (!intersection_attribute || !attribute ||
                    attribute->type != intersection_attribute->type) {
                    throw std::runtime_error(
                        "pelican.procedural_hit_attribute_mismatch@1: " +
                        occurrenceName(stage) + " must declare the same hit attribute as " +
                        occurrenceName(*intersections[index]));
                }
                if (attribute->byte_size > facts.max_ray_hit_attribute_size) {
                    throw std::runtime_error(
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
                throw std::runtime_error(
                    "pelican.ray_tracing.unsupported_shader_stage@1: triangles hit group has an intersection stage");
            }
            validate_triangle_hit_stage(hit_groups[index]);
            if (any_hits[index]) {
                validate_triangle_hit_stage(*any_hits[index]);
            }
        }
    }

    if (request.group_layout != nullptr &&
        request.group_layout->usesMaterialAlphaProfile()) {
        constexpr std::uint32_t opaque_ray_flag = 0x1u;
        for (const auto &trace : request.raygen.entry->trace_calls) {
            if (!trace.ray_flags ||
                ((*trace.ray_flags & opaque_ray_flag) != 0)) {
                throw std::runtime_error(
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
                    any_incoming ? &any_incoming->type : nullptr);
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
        const auto validate_record =
            [&](const RayTracingShaderOccurrence &occurrence,
                bool allowed, bool required) {
                const auto &actual = occurrence.entry->shader_record;
                if (!actual) {
                    if (required && !layout.physical_record.empty()) {
                        failShaderRecord(
                            occurrence,
                            "has no shader-record storage; expected " +
                                std::to_string(layout.physical_record.size()) +
                                " members");
                    }
                    return;
                }
                if (!allowed) {
                    failShaderRecord(
                        occurrence,
                        "declares shader-record storage where it is forbidden");
                }
                if (actual->members.size() != layout.physical_record.size()) {
                    failShaderRecord(
                        occurrence, "member_count=" +
                                        std::to_string(actual->members.size()) +
                                        " expected=" +
                                        std::to_string(layout.physical_record.size()));
                }
                for (std::size_t member = 0;
                     member < layout.physical_record.size(); ++member) {
                    const auto expected_type =
                        canonicalRecordType(layout.physical_record[member].type);
                    if (actual->members[member].offset !=
                            layout.physical_record[member].offset ||
                        actual->members[member].type != expected_type) {
                        failShaderRecord(
                            occurrence, "member=" + std::to_string(member) +
                                            " offset=" +
                                            std::to_string(actual->members[member].offset) +
                                            " expected_offset=" +
                                            std::to_string(layout.physical_record[member].offset) +
                                            " type=" +
                                            describeRayTracingCanonicalType(
                                                actual->members[member].type) +
                                            " expected_type=" +
                                            describeRayTracingCanonicalType(
                                                expected_type));
                    }
                }
            };
        validate_record(request.raygen, false, false);
        for (const auto &miss : request.misses)
            validate_record(miss, false, false);
        for (std::size_t index = 0; index < hit_groups.size(); ++index) {
            const auto ray = static_cast<std::uint32_t>(index % ray_count);
            const auto traced =
                std::find(request.traced_ray_type_indices.begin(),
                          request.traced_ray_type_indices.end(), ray) !=
                request.traced_ray_type_indices.end();
            validate_record(hit_groups[index], traced, traced);
            if (any_hits[index]) {
                validate_record(*any_hits[index], traced, traced);
            }
            if (intersections[index]) {
                // An intersection shader may consume the hit-group record,
                // but a generic implementation (including the engine's
                // no-report procedural control shader) need not declare it.
                // If present, keep enforcing the same physical ABI.
                validate_record(*intersections[index], traced, false);
            }
        }
    }

    const auto validate_trace = [&](const auto &caller,
                                    const auto &trace) {
        const auto *outgoing = &trace.payload_type;
        const auto compare_target = [&](std::string_view domain,
                                        std::uint32_t index,
                                        auto targets) {
            if (index >= targets.size()) {
                failPayload(caller, trace, domain, index,
                            outgoing, nullptr);
            }
            const auto &target = targets[index];
            const auto *incoming = findInterface(
                *target.entry,
                RayTracingInterfaceStorage::incoming_ray_payload);
            if (incoming == nullptr || *outgoing != incoming->type) {
                failPayload(caller, trace, domain, index,
                            outgoing,
                            incoming ? &incoming->type : nullptr);
            }
        };
        const auto compare_hit_target = [&](std::uint32_t index) {
            if (index >= hit_groups.size()) {
                failPayload(caller, trace, "hit", index,
                            outgoing, nullptr);
            }
            const auto *incoming = hit_payload_types[index];
            if (incoming == nullptr || *outgoing != *incoming) {
                failPayload(caller, trace, "hit", index, outgoing,
                            incoming);
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
        validateRayTracingRecordAddressing(
            trace.sbt_record_offset, trace.sbt_record_stride,
            trace.miss_index, *request.group_layout, addressing);
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
    };
    validate_traces(request.raygen);
    for (const auto &occurrence : request.misses)
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
