#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace Pelican {

enum class GpuDrawTimingSelection : std::uint8_t {
    gpu_culling,
    cpu_draw_queue,
    inconclusive,
};

std::string_view gpuDrawTimingSelectionName(
    GpuDrawTimingSelection selection) noexcept;

struct GpuDrawTimingDeviceIdentity {
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t driver_version = 0;
    std::string device_name;

    bool operator==(const GpuDrawTimingDeviceIdentity &) const = default;
};

// Counts describe the published draw transport, not scene entities. One
// entity may contribute multiple records and XR may publish per-view records.
struct GpuDrawTimingWorkload {
    std::uint32_t candidate_records = 0;
    // Sum of compacted outputs produced across the measured segment set.
    // Overlapping view/filter segments may make this exceed candidates.
    std::uint32_t visible_records = 0;
    std::uint32_t segment_records = 0;
    std::uint32_t view_count = 0;
    std::uint32_t output_capacity_records = 0;

    bool operator==(const GpuDrawTimingWorkload &) const = default;
};

struct GpuDrawGpuPathTiming {
    // Complete graph GPU duration is the break-even metric. The node values
    // are attribution evidence and are intentionally not summed into another
    // competing metric.
    double frame_gpu_ms = 0.0;
    double culling_gpu_ms = 0.0;
    double material_draw_gpu_ms = 0.0;
    // Wall-clock Renderer::render duration. It may include queue/fence pacing
    // and therefore remains diagnostic rather than the v1 decision metric.
    double host_frame_ms = 0.0;
    std::uint32_t sample_count = 0;

    bool operator==(const GpuDrawGpuPathTiming &) const = default;
};

struct GpuDrawCpuPathTiming {
    double frame_gpu_ms = 0.0;
    double material_draw_gpu_ms = 0.0;
    double host_frame_ms = 0.0;
    std::uint32_t sample_count = 0;

    bool operator==(const GpuDrawCpuPathTiming &) const = default;
};

// Offline measurement evidence. Runtime timing must not mutate render policy;
// a reviewed observation may later be copied into a device profile.
struct GpuDrawTimingObservation {
    GpuDrawTimingDeviceIdentity device;
    std::string graph_variant;
    GpuDrawTimingWorkload workload;
    GpuDrawGpuPathTiming gpu_path;
    GpuDrawCpuPathTiming cpu_path;
    std::string source;

    bool operator==(const GpuDrawTimingObservation &) const = default;
};

struct GpuDrawBreakEvenPolicy {
    double minimum_gain_percent = 5.0;
    std::uint32_t minimum_sample_count = 16;

    bool operator==(const GpuDrawBreakEvenPolicy &) const = default;
};

struct GpuDrawBreakEvenDecision {
    GpuDrawTimingSelection selection =
        GpuDrawTimingSelection::inconclusive;
    std::string metric = "frame_gpu_ms";
    double measured_gain_percent = 0.0;
    std::string reason;

    bool operator==(const GpuDrawBreakEvenDecision &) const = default;
};

void validateGpuDrawTimingObservation(
    const GpuDrawTimingObservation &observation);

GpuDrawTimingObservation compileGpuDrawTimingObservation(
    const nlohmann::json &declaration);

GpuDrawBreakEvenDecision evaluateGpuDrawBreakEven(
    const GpuDrawTimingObservation &observation,
    const GpuDrawBreakEvenPolicy &policy = {});

nlohmann::ordered_json gpuDrawTimingObservationToJson(
    const GpuDrawTimingObservation &observation);

nlohmann::ordered_json gpuDrawBreakEvenPolicyToJson(
    const GpuDrawBreakEvenPolicy &policy);

nlohmann::ordered_json gpuDrawBreakEvenDecisionToJson(
    const GpuDrawBreakEvenDecision &decision);

} // namespace Pelican
