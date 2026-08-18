#include "passshapepolicy.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace Pelican {
namespace {

constexpr std::size_t typeIndex(RenderPassType type) {
    return static_cast<std::size_t>(type);
}

PassShapePolicy makeDefaultPolicy() {
    constexpr auto unlimited = std::numeric_limits<std::size_t>::max();
    PassShapePolicy policy{
        .types = {},
        .swapchain_input_allowed = false,
        .swapchain_color_output_allowed = true,
        .swapchain_depth_output_allowed = false,
        .duplicate_color_outputs_allowed = false,
        .duplicate_inputs_allowed = false,
        .current_frame_color_feedback_allowed = false,
        .current_frame_depth_feedback_allowed = false,
    };

    for (auto &rule : policy.types) {
        rule = PassShapeTypePolicy{
            .minimum_color_outputs = 0,
            .maximum_color_outputs = unlimited,
            .depth_output_required = false,
            .depth_output_allowed = true,
            .any_output_required = true,
            .inputs_allowed = false,
            .minimum_inputs = 0,
        };
    }

    for (const auto type : {
             RenderPassType::material,
             RenderPassType::fullscreen,
             RenderPassType::raster,
             RenderPassType::output_transform,
         }) {
        policy.types[typeIndex(type)].inputs_allowed = true;
    }

    for (const auto type : {
             RenderPassType::fullscreen,
             RenderPassType::output_transform,
         }) {
        auto &rule = policy.types[typeIndex(type)];
        rule.minimum_color_outputs = 1;
        rule.maximum_color_outputs = 1;
        rule.depth_output_allowed = false;
    }

    {
        auto &rule = policy.types[typeIndex(RenderPassType::shadow_depth)];
        rule.maximum_color_outputs = 0;
        rule.depth_output_required = true;
    }
    for (const auto type : {
             RenderPassType::velocity,
             RenderPassType::picking,
         }) {
        auto &rule = policy.types[typeIndex(type)];
        rule.minimum_color_outputs = 1;
        rule.maximum_color_outputs = 1;
        rule.depth_output_required = true;
    }
    for (const auto type : {
             RenderPassType::debug_draw,
             RenderPassType::gizmo,
             RenderPassType::debug_text,
             RenderPassType::ui,
             RenderPassType::imgui,
         }) {
        auto &rule = policy.types[typeIndex(type)];
        rule.minimum_color_outputs = 1;
        rule.maximum_color_outputs = 1;
        rule.depth_output_allowed = false;
    }
    for (const auto type : {
             RenderPassType::canonical_anchor,
             RenderPassType::snapshot_copy,
         }) {
        policy.types[typeIndex(type)].any_output_required = false;
    }
    return policy;
}

std::string targetName(const nlohmann::json &encoded,
                       std::string_view role) {
    if (encoded.is_string()) {
        return encoded.get<std::string>();
    }
    if (encoded.is_object() && encoded.contains("target") &&
        encoded.at("target").is_string()) {
        return encoded.at("target").get<std::string>();
    }
    throw std::runtime_error(std::string{role} +
                             " must name a render target");
}

template <class Callback>
void forEachScalarOrArray(const nlohmann::json &encoded,
                          Callback callback) {
    if (encoded.is_null()) {
        return;
    }
    if (encoded.is_array()) {
        for (const auto &value : encoded) {
            callback(value);
        }
        return;
    }
    callback(encoded);
}

PassShapeViolation countViolation(PassShapeViolationKind kind,
                                  std::size_t observed,
                                  std::size_t minimum,
                                  std::size_t maximum) {
    return PassShapeViolation{
        .kind = kind,
        .target = {},
        .observed_count = observed,
        .minimum_count = minimum,
        .maximum_count = maximum,
    };
}

PassShapeViolation targetViolation(PassShapeViolationKind kind,
                                   std::string target) {
    return PassShapeViolation{
        .kind = kind,
        .target = std::move(target),
        .observed_count = 0,
        .minimum_count = 0,
        .maximum_count = 0,
    };
}

} // namespace

const PassShapePolicy &defaultPassShapePolicy() {
    static const PassShapePolicy policy = makeDefaultPolicy();
    return policy;
}

const PassShapeTypePolicy &passShapeTypePolicy(
    const PassShapePolicy &policy, RenderPassType type) {
    const auto index = typeIndex(type);
    if (index >= policy.types.size()) {
        throw std::runtime_error("Unknown render pass type for shape policy");
    }
    return policy.types[index];
}

std::string_view passShapeViolationName(PassShapeViolationKind kind) {
    switch (kind) {
    case PassShapeViolationKind::missing_output:
        return "missing_output";
    case PassShapeViolationKind::color_output_count:
        return "color_output_count";
    case PassShapeViolationKind::depth_output_required:
        return "depth_output_required";
    case PassShapeViolationKind::depth_output_unsupported:
        return "depth_output_unsupported";
    case PassShapeViolationKind::input_unsupported:
        return "input_unsupported";
    case PassShapeViolationKind::input_count:
        return "input_count";
    case PassShapeViolationKind::swapchain_input:
        return "swapchain_input";
    case PassShapeViolationKind::swapchain_color_output:
        return "swapchain_color_output";
    case PassShapeViolationKind::swapchain_depth_output:
        return "swapchain_depth_output";
    case PassShapeViolationKind::duplicate_color_output:
        return "duplicate_color_output";
    case PassShapeViolationKind::duplicate_input:
        return "duplicate_input";
    case PassShapeViolationKind::current_frame_color_feedback:
        return "current_frame_color_feedback";
    case PassShapeViolationKind::current_frame_depth_feedback:
        return "current_frame_depth_feedback";
    }
    throw std::runtime_error("Unknown pass shape violation");
}

std::string passShapeViolationDescription(
    const PassShapeViolation &violation) {
    const std::string name{passShapeViolationName(violation.kind)};
    switch (violation.kind) {
    case PassShapeViolationKind::missing_output:
        return name + ": pass must output color or depth";
    case PassShapeViolationKind::color_output_count:
        return name + ": observed " +
               std::to_string(violation.observed_count) +
               " color outputs; expected " +
               std::to_string(violation.minimum_count) + ".." +
               (violation.maximum_count ==
                        std::numeric_limits<std::size_t>::max()
                    ? std::string{"unbounded"}
                    : std::to_string(violation.maximum_count));
    case PassShapeViolationKind::depth_output_required:
        return name + ": a depth output is required";
    case PassShapeViolationKind::depth_output_unsupported:
        return name + ": depth output is not supported";
    case PassShapeViolationKind::input_unsupported:
        return name + ": this pass type does not support inputs";
    case PassShapeViolationKind::input_count:
        return name + ": observed " +
               std::to_string(violation.observed_count) +
               " inputs; expected at least " +
               std::to_string(violation.minimum_count);
    case PassShapeViolationKind::swapchain_input:
    case PassShapeViolationKind::swapchain_color_output:
    case PassShapeViolationKind::swapchain_depth_output:
    case PassShapeViolationKind::duplicate_color_output:
    case PassShapeViolationKind::duplicate_input:
    case PassShapeViolationKind::current_frame_color_feedback:
    case PassShapeViolationKind::current_frame_depth_feedback:
        return name + ": " + violation.target;
    }
    throw std::runtime_error("Unknown pass shape violation");
}

PassShapeInputReference parsePassShapeInputReference(
    std::string_view authored) {
    constexpr std::string_view suffix = "@history";
    if (authored.ends_with(suffix)) {
        const auto name = authored.substr(0, authored.size() - suffix.size());
        if (name.empty() || name.find('@') != std::string_view::npos) {
            throw std::runtime_error("Invalid history input target: " +
                                     std::string{authored});
        }
        return {std::string{name}, true};
    }
    if (authored.find('@') != std::string_view::npos) {
        throw std::runtime_error("Unknown input target qualifier: " +
                                 std::string{authored});
    }
    return {std::string{authored}, false};
}

PassShapeObservation passShapeObservationFromJson(
    RenderPassType type, const nlohmann::json &pass,
    std::span<const std::string> non_image_inputs) {
    if (!pass.is_object()) {
        throw std::runtime_error("Pass shape observation requires an object");
    }
    const auto output = pass.find("output");
    if (output == pass.end() || !output->is_object()) {
        throw std::runtime_error(
            "Pass shape observation requires object field 'output'");
    }
    if (!output->contains("color") || !output->contains("depth")) {
        throw std::runtime_error(
            "Pass shape observation output requires color and depth fields");
    }

    PassShapeObservation observation{.type = type};
    forEachScalarOrArray(output->at("color"), [&](const auto &encoded) {
        observation.color_outputs.push_back(
            targetName(encoded, "Color output"));
    });
    if (!output->at("depth").is_null()) {
        observation.depth_output =
            targetName(output->at("depth"), "Depth output");
    }

    const auto input = pass.find("input");
    if (input == pass.end()) {
        return observation;
    }
    forEachScalarOrArray(*input, [&](const auto &encoded) {
        if (!encoded.is_string()) {
            throw std::runtime_error(
                "Input target must be a render target or buffer name");
        }
        auto reference = parsePassShapeInputReference(
            encoded.get_ref<const std::string &>());
        const bool image = std::find(non_image_inputs.begin(),
                                     non_image_inputs.end(),
                                     reference.name) ==
                           non_image_inputs.end();
        observation.inputs.push_back(PassShapeInputObservation{
            .name = std::move(reference.name),
            .history = reference.history,
            .image = image,
        });
    });
    return observation;
}

std::vector<PassShapeViolation> evaluatePassShape(
    const PassShapePolicy &policy,
    const PassShapeObservation &observation) {
    const auto &rule = passShapeTypePolicy(policy, observation.type);
    std::vector<PassShapeViolation> violations;

    if (rule.any_output_required && observation.color_outputs.empty() &&
        !observation.depth_output) {
        violations.push_back(targetViolation(
            PassShapeViolationKind::missing_output, {}));
    }
    if (observation.color_outputs.size() < rule.minimum_color_outputs ||
        observation.color_outputs.size() > rule.maximum_color_outputs) {
        violations.push_back(countViolation(
            PassShapeViolationKind::color_output_count,
            observation.color_outputs.size(), rule.minimum_color_outputs,
            rule.maximum_color_outputs));
    }
    if (rule.depth_output_required && !observation.depth_output) {
        violations.push_back(targetViolation(
            PassShapeViolationKind::depth_output_required, {}));
    }
    if (!rule.depth_output_allowed && observation.depth_output) {
        violations.push_back(targetViolation(
            PassShapeViolationKind::depth_output_unsupported,
            *observation.depth_output));
    }
    if (!rule.inputs_allowed && !observation.inputs.empty()) {
        violations.push_back(countViolation(
            PassShapeViolationKind::input_unsupported,
            observation.inputs.size(), 0, 0));
    }
    if (observation.inputs.size() < rule.minimum_inputs) {
        violations.push_back(countViolation(
            PassShapeViolationKind::input_count,
            observation.inputs.size(), rule.minimum_inputs,
            std::numeric_limits<std::size_t>::max()));
    }

    if (!policy.swapchain_input_allowed) {
        for (const auto &input : observation.inputs) {
            if (input.image && input.name == "swapchain") {
                violations.push_back(targetViolation(
                    PassShapeViolationKind::swapchain_input, input.name));
                break;
            }
        }
    }
    if (!policy.swapchain_color_output_allowed &&
        std::ranges::find(observation.color_outputs, "swapchain") !=
            observation.color_outputs.end()) {
        violations.push_back(targetViolation(
            PassShapeViolationKind::swapchain_color_output, "swapchain"));
    }
    if (!policy.swapchain_depth_output_allowed &&
        observation.depth_output == "swapchain") {
        violations.push_back(targetViolation(
            PassShapeViolationKind::swapchain_depth_output, "swapchain"));
    }

    if (!policy.duplicate_color_outputs_allowed) {
        std::unordered_set<std::string> seen;
        for (const auto &output : observation.color_outputs) {
            if (!seen.insert(output).second) {
                violations.push_back(targetViolation(
                    PassShapeViolationKind::duplicate_color_output,
                    output));
            }
        }
    }
    if (!policy.duplicate_inputs_allowed) {
        std::unordered_set<std::string> seen;
        for (const auto &input : observation.inputs) {
            if (input.image && !seen.insert(input.name).second) {
                violations.push_back(targetViolation(
                    PassShapeViolationKind::duplicate_input, input.name));
            }
        }
    }

    for (const auto &input : observation.inputs) {
        if (!input.image || input.history) {
            continue;
        }
        if (!policy.current_frame_color_feedback_allowed &&
            std::ranges::find(observation.color_outputs, input.name) !=
                observation.color_outputs.end()) {
            violations.push_back(targetViolation(
                PassShapeViolationKind::current_frame_color_feedback,
                input.name));
        }
        if (!policy.current_frame_depth_feedback_allowed &&
            observation.depth_output == input.name) {
            violations.push_back(targetViolation(
                PassShapeViolationKind::current_frame_depth_feedback,
                input.name));
        }
    }
    return violations;
}

} // namespace Pelican
