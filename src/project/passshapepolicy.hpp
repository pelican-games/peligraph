#pragma once

#include "passfieldownership.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::size_t passShapeTypeCount = 14;

struct PassShapeTypePolicy {
    std::size_t minimum_color_outputs;
    std::size_t maximum_color_outputs;
    bool depth_output_required;
    bool depth_output_allowed;
    bool any_output_required;
    bool inputs_allowed;
    std::size_t minimum_inputs;

    bool operator==(const PassShapeTypePolicy &) const = default;
};

struct PassShapePolicy {
    std::array<PassShapeTypePolicy, passShapeTypeCount> types;
    bool swapchain_input_allowed;
    bool swapchain_color_output_allowed;
    bool swapchain_depth_output_allowed;
    bool duplicate_color_outputs_allowed;
    bool duplicate_inputs_allowed;
    bool current_frame_color_feedback_allowed;
    bool current_frame_depth_feedback_allowed;

    bool operator==(const PassShapePolicy &) const = default;
};

struct PassShapeInputObservation {
    std::string name;
    bool history;
    bool image;

    bool operator==(const PassShapeInputObservation &) const = default;
};

struct PassShapeObservation {
    RenderPassType type;
    std::vector<std::string> color_outputs;
    std::optional<std::string> depth_output;
    std::vector<PassShapeInputObservation> inputs;

    bool operator==(const PassShapeObservation &) const = default;
};

enum class PassShapeViolationKind {
    missing_output,
    color_output_count,
    depth_output_required,
    depth_output_unsupported,
    input_unsupported,
    input_count,
    swapchain_input,
    swapchain_color_output,
    swapchain_depth_output,
    duplicate_color_output,
    duplicate_input,
    current_frame_color_feedback,
    current_frame_depth_feedback,
};

struct PassShapeViolation {
    PassShapeViolationKind kind;
    std::string target;
    std::size_t observed_count;
    std::size_t minimum_count;
    std::size_t maximum_count;

    bool operator==(const PassShapeViolation &) const = default;
};

// The immutable production policy. Callers inject this reference explicitly;
// tests may instead inject a different immutable value.
const PassShapePolicy &defaultPassShapePolicy();

const PassShapeTypePolicy &passShapeTypePolicy(
    const PassShapePolicy &policy, RenderPassType type);

std::string_view passShapeViolationName(PassShapeViolationKind kind);
std::string passShapeViolationDescription(const PassShapeViolation &violation);

struct PassShapeInputReference {
    std::string name;
    bool history;

    bool operator==(const PassShapeInputReference &) const = default;
};

PassShapeInputReference parsePassShapeInputReference(
    std::string_view authored);

// Builds the core/studio-neutral observation from an authored pass object.
// Names listed in non_image_inputs still count as inputs, but are excluded
// from render-target duplicate and feedback checks. Reserved-name role checks
// apply regardless of the resolved resource kind.
PassShapeObservation passShapeObservationFromJson(
    RenderPassType type, const nlohmann::json &pass,
    std::span<const std::string> non_image_inputs = {});

std::vector<PassShapeViolation> evaluatePassShape(
    const PassShapePolicy &policy,
    const PassShapeObservation &observation);

// Converts evaluator output into the one named exception contract shared by
// every engine parser. Keeping this formatting beside the evaluator prevents
// parser-specific interpretations of the same violation.
void validatePassShapeObservation(
    const PassShapePolicy &policy,
    const PassShapeObservation &observation,
    std::string_view pass_name);

// Shared pre-validation boundary for independently-built views of authored
// pass JSON (notably PassDefinition and frameplanner). It preserves the exact
// authored RenderPassType while applying the injected immutable policy.
RenderPassType validateAuthoredPassShape(
    const PassShapePolicy &policy,
    const nlohmann::json &pass,
    PassFieldOwnershipCapabilities capabilities,
    std::string_view pass_name,
    std::span<const std::string> non_image_inputs = {},
    std::string_view context = "pass");

} // namespace Pelican
