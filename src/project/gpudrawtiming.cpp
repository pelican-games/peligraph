#include "gpudrawtiming.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

void requireOnlyKeys(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> allowed,
    std::string_view subject) {
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (std::find(allowed.begin(), allowed.end(), field.key()) ==
            allowed.end()) {
            throw std::runtime_error(
                std::string{subject} + " has unknown key '" +
                field.key() + "'");
        }
    }
}

const nlohmann::json &requiredObject(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_object()) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{key} +
            " must be an object");
    }
    return *found;
}

std::string requiredNonEmptyString(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{key} +
            " must be a non-empty string");
    }
    return found->get<std::string>();
}

std::uint32_t requiredUnsigned(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_number_integer()) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{key} +
            " must be an unsigned 32-bit integer");
    }
    std::uint64_t value = 0;
    if (found->is_number_unsigned()) {
        value = found->get<std::uint64_t>();
    } else {
        const auto signed_value = found->get<std::int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(
                std::string{subject} + " " + std::string{key} +
                " must be an unsigned 32-bit integer");
        }
        value = static_cast<std::uint64_t>(signed_value);
    }
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{key} +
            " must be an unsigned 32-bit integer");
    }
    return static_cast<std::uint32_t>(value);
}

double requiredFiniteNonNegative(
    const nlohmann::json &object,
    std::string_view key,
    std::string_view subject) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_number()) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{key} +
            " must be a finite non-negative number");
    }
    const auto value = found->get<double>();
    if (!std::isfinite(value) || value < 0.0) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{key} +
            " must be a finite non-negative number");
    }
    return value;
}

GpuDrawTimingDeviceIdentity parseDevice(
    const nlohmann::json &value) {
    static constexpr std::string_view subject =
        "GPU draw timing device";
    requireOnlyKeys(
        value,
        {"vendor_id", "device_id", "driver_version", "device_name"},
        subject);
    return {
        .vendor_id = requiredUnsigned(value, "vendor_id", subject),
        .device_id = requiredUnsigned(value, "device_id", subject),
        .driver_version =
            requiredUnsigned(value, "driver_version", subject),
        .device_name =
            requiredNonEmptyString(value, "device_name", subject),
    };
}

GpuDrawTimingWorkload parseWorkload(
    const nlohmann::json &value) {
    static constexpr std::string_view subject =
        "GPU draw timing workload";
    requireOnlyKeys(
        value,
        {"candidate_records", "visible_records", "segment_records",
         "view_count", "output_capacity_records"},
        subject);
    return {
        .candidate_records =
            requiredUnsigned(value, "candidate_records", subject),
        .visible_records =
            requiredUnsigned(value, "visible_records", subject),
        .segment_records =
            requiredUnsigned(value, "segment_records", subject),
        .view_count =
            requiredUnsigned(value, "view_count", subject),
        .output_capacity_records =
            requiredUnsigned(value, "output_capacity_records", subject),
    };
}

GpuDrawGpuPathTiming parseGpuPath(
    const nlohmann::json &value) {
    static constexpr std::string_view subject =
        "GPU draw timing gpu_path";
    requireOnlyKeys(
        value,
        {"frame_gpu_ms", "culling_gpu_ms", "material_draw_gpu_ms",
         "host_frame_ms", "sample_count"},
        subject);
    return {
        .frame_gpu_ms =
            requiredFiniteNonNegative(value, "frame_gpu_ms", subject),
        .culling_gpu_ms =
            requiredFiniteNonNegative(value, "culling_gpu_ms", subject),
        .material_draw_gpu_ms =
            requiredFiniteNonNegative(
                value, "material_draw_gpu_ms", subject),
        .host_frame_ms =
            requiredFiniteNonNegative(value, "host_frame_ms", subject),
        .sample_count =
            requiredUnsigned(value, "sample_count", subject),
    };
}

GpuDrawCpuPathTiming parseCpuPath(
    const nlohmann::json &value) {
    static constexpr std::string_view subject =
        "GPU draw timing cpu_path";
    requireOnlyKeys(
        value,
        {"frame_gpu_ms", "material_draw_gpu_ms", "host_frame_ms",
         "sample_count"},
        subject);
    return {
        .frame_gpu_ms =
            requiredFiniteNonNegative(value, "frame_gpu_ms", subject),
        .material_draw_gpu_ms =
            requiredFiniteNonNegative(
                value, "material_draw_gpu_ms", subject),
        .host_frame_ms =
            requiredFiniteNonNegative(value, "host_frame_ms", subject),
        .sample_count =
            requiredUnsigned(value, "sample_count", subject),
    };
}

void validateFiniteNonNegative(double value, std::string_view field) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::runtime_error(
            "GPU draw timing " + std::string{field} +
            " must be finite and non-negative");
    }
}

nlohmann::ordered_json deviceToJson(
    const GpuDrawTimingDeviceIdentity &device) {
    return {
        {"vendor_id", device.vendor_id},
        {"device_id", device.device_id},
        {"driver_version", device.driver_version},
        {"device_name", device.device_name},
    };
}

nlohmann::ordered_json workloadToJson(
    const GpuDrawTimingWorkload &workload) {
    return {
        {"candidate_records", workload.candidate_records},
        {"visible_records", workload.visible_records},
        {"segment_records", workload.segment_records},
        {"view_count", workload.view_count},
        {"output_capacity_records",
         workload.output_capacity_records},
    };
}

nlohmann::ordered_json gpuPathToJson(
    const GpuDrawGpuPathTiming &timing) {
    return {
        {"frame_gpu_ms", timing.frame_gpu_ms},
        {"culling_gpu_ms", timing.culling_gpu_ms},
        {"material_draw_gpu_ms", timing.material_draw_gpu_ms},
        {"host_frame_ms", timing.host_frame_ms},
        {"sample_count", timing.sample_count},
    };
}

nlohmann::ordered_json cpuPathToJson(
    const GpuDrawCpuPathTiming &timing) {
    return {
        {"frame_gpu_ms", timing.frame_gpu_ms},
        {"material_draw_gpu_ms", timing.material_draw_gpu_ms},
        {"host_frame_ms", timing.host_frame_ms},
        {"sample_count", timing.sample_count},
    };
}

} // namespace

std::string_view gpuDrawTimingSelectionName(
    GpuDrawTimingSelection selection) noexcept {
    switch (selection) {
    case GpuDrawTimingSelection::gpu_culling:
        return "gpu_culling";
    case GpuDrawTimingSelection::cpu_draw_queue:
        return "cpu_draw_queue";
    case GpuDrawTimingSelection::inconclusive:
        return "inconclusive";
    }
    return "unknown";
}

void validateGpuDrawTimingObservation(
    const GpuDrawTimingObservation &observation) {
    if (observation.device.device_name.empty()) {
        throw std::runtime_error(
            "GPU draw timing device_name must be non-empty");
    }
    if (observation.graph_variant.empty()) {
        throw std::runtime_error(
            "GPU draw timing graph_variant must be non-empty");
    }
    if (observation.source.empty()) {
        throw std::runtime_error(
            "GPU draw timing source must be non-empty");
    }
    if (observation.workload.candidate_records == 0 ||
        observation.workload.segment_records == 0 ||
        observation.workload.view_count == 0 ||
        observation.workload.output_capacity_records == 0) {
        throw std::runtime_error(
            "GPU draw timing workload counts must be non-zero except "
            "visible_records");
    }
    if (observation.workload.visible_records >
        observation.workload.output_capacity_records) {
        throw std::runtime_error(
            "GPU draw timing visible_records exceeds output capacity");
    }
    validateFiniteNonNegative(
        observation.gpu_path.frame_gpu_ms,
        "gpu_path.frame_gpu_ms");
    validateFiniteNonNegative(
        observation.gpu_path.culling_gpu_ms,
        "gpu_path.culling_gpu_ms");
    validateFiniteNonNegative(
        observation.gpu_path.material_draw_gpu_ms,
        "gpu_path.material_draw_gpu_ms");
    validateFiniteNonNegative(
        observation.gpu_path.host_frame_ms,
        "gpu_path.host_frame_ms");
    validateFiniteNonNegative(
        observation.cpu_path.frame_gpu_ms,
        "cpu_path.frame_gpu_ms");
    validateFiniteNonNegative(
        observation.cpu_path.material_draw_gpu_ms,
        "cpu_path.material_draw_gpu_ms");
    validateFiniteNonNegative(
        observation.cpu_path.host_frame_ms,
        "cpu_path.host_frame_ms");
    if (observation.gpu_path.frame_gpu_ms <= 0.0 ||
        observation.cpu_path.frame_gpu_ms <= 0.0) {
        throw std::runtime_error(
            "GPU draw timing frame_gpu_ms must be greater than zero");
    }
    if (observation.gpu_path.sample_count == 0 ||
        observation.cpu_path.sample_count == 0) {
        throw std::runtime_error(
            "GPU draw timing sample_count must be greater than zero");
    }
}

GpuDrawTimingObservation compileGpuDrawTimingObservation(
    const nlohmann::json &declaration) {
    static constexpr std::string_view subject =
        "GPU draw timing observation";
    if (!declaration.is_object()) {
        throw std::runtime_error(
            std::string{subject} + " must be an object");
    }
    requireOnlyKeys(
        declaration,
        {"schema", "version", "device", "graph_variant", "workload",
         "gpu_path", "cpu_path", "source"},
        subject);
    if (requiredNonEmptyString(
            declaration, "schema", subject) !=
        "pelican.gpu_draw_timing") {
        throw std::runtime_error(
            "GPU draw timing observation has unsupported schema");
    }
    if (requiredUnsigned(declaration, "version", subject) != 1) {
        throw std::runtime_error(
            "GPU draw timing observation has unsupported version");
    }
    GpuDrawTimingObservation result{
        .device = parseDevice(
            requiredObject(declaration, "device", subject)),
        .graph_variant =
            requiredNonEmptyString(
                declaration, "graph_variant", subject),
        .workload = parseWorkload(
            requiredObject(declaration, "workload", subject)),
        .gpu_path = parseGpuPath(
            requiredObject(declaration, "gpu_path", subject)),
        .cpu_path = parseCpuPath(
            requiredObject(declaration, "cpu_path", subject)),
        .source =
            requiredNonEmptyString(declaration, "source", subject),
    };
    validateGpuDrawTimingObservation(result);
    return result;
}

GpuDrawBreakEvenDecision evaluateGpuDrawBreakEven(
    const GpuDrawTimingObservation &observation,
    const GpuDrawBreakEvenPolicy &policy) {
    validateGpuDrawTimingObservation(observation);
    if (!std::isfinite(policy.minimum_gain_percent) ||
        policy.minimum_gain_percent < 0.0 ||
        policy.minimum_gain_percent >= 100.0) {
        throw std::runtime_error(
            "GPU draw break-even minimum_gain_percent must be finite "
            "and in [0, 100)");
    }
    if (policy.minimum_sample_count == 0) {
        throw std::runtime_error(
            "GPU draw break-even minimum_sample_count must be greater "
            "than zero");
    }

    const auto gain =
        (observation.cpu_path.frame_gpu_ms -
         observation.gpu_path.frame_gpu_ms) /
        observation.cpu_path.frame_gpu_ms * 100.0;
    const auto available_samples = std::min(
        observation.gpu_path.sample_count,
        observation.cpu_path.sample_count);

    GpuDrawBreakEvenDecision result;
    result.measured_gain_percent = gain;
    if (available_samples < policy.minimum_sample_count) {
        result.reason =
            "insufficient samples: available=" +
            std::to_string(available_samples) +
            ", required=" +
            std::to_string(policy.minimum_sample_count);
        return result;
    }

    if (observation.gpu_path.frame_gpu_ms <
            observation.cpu_path.frame_gpu_ms &&
        gain >= policy.minimum_gain_percent) {
        result.selection =
            GpuDrawTimingSelection::gpu_culling;
    } else {
        result.selection =
            GpuDrawTimingSelection::cpu_draw_queue;
    }
    result.reason =
        "measured frame_gpu_ms selects " +
        std::string{gpuDrawTimingSelectionName(result.selection)} +
        " (gain=" + std::to_string(gain) +
        "%, required=" +
        std::to_string(policy.minimum_gain_percent) +
        "%, samples=" +
        std::to_string(available_samples) + ")";
    return result;
}

nlohmann::ordered_json gpuDrawTimingObservationToJson(
    const GpuDrawTimingObservation &observation) {
    validateGpuDrawTimingObservation(observation);
    return {
        {"schema", "pelican.gpu_draw_timing"},
        {"version", 1},
        {"device", deviceToJson(observation.device)},
        {"graph_variant", observation.graph_variant},
        {"workload", workloadToJson(observation.workload)},
        {"gpu_path", gpuPathToJson(observation.gpu_path)},
        {"cpu_path", cpuPathToJson(observation.cpu_path)},
        {"source", observation.source},
    };
}

nlohmann::ordered_json gpuDrawBreakEvenPolicyToJson(
    const GpuDrawBreakEvenPolicy &policy) {
    if (!std::isfinite(policy.minimum_gain_percent) ||
        policy.minimum_gain_percent < 0.0 ||
        policy.minimum_gain_percent >= 100.0 ||
        policy.minimum_sample_count == 0) {
        throw std::runtime_error(
            "GPU draw break-even policy is invalid");
    }
    return {
        {"minimum_gain_percent", policy.minimum_gain_percent},
        {"minimum_sample_count", policy.minimum_sample_count},
        {"metric", "frame_gpu_ms"},
    };
}

nlohmann::ordered_json gpuDrawBreakEvenDecisionToJson(
    const GpuDrawBreakEvenDecision &decision) {
    if (!std::isfinite(decision.measured_gain_percent) ||
        decision.metric.empty() || decision.reason.empty()) {
        throw std::runtime_error(
            "GPU draw break-even decision is invalid");
    }
    return {
        {"selection",
         gpuDrawTimingSelectionName(decision.selection)},
        {"metric", decision.metric},
        {"measured_gain_percent",
         decision.measured_gain_percent},
        {"reason", decision.reason},
    };
}

} // namespace Pelican
