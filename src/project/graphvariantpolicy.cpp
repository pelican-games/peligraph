#include "graphvariantpolicy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <stdexcept>
#include <string>

#ifndef PELICAN_WITH_OPENXR
#define PELICAN_WITH_OPENXR 0
#endif

namespace Pelican {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

bool containsMarker(std::string_view value,
                    std::span<const std::string_view> markers) {
    const auto name = lower(std::string{value});
    return std::any_of(markers.begin(), markers.end(),
                       [&](const std::string_view marker) {
                           return name.find(marker) != std::string::npos;
                       });
}

bool unsafePreviewName(std::string_view value) {
    constexpr std::array markers{
        std::string_view{"taa"}, std::string_view{"velocity"},
        std::string_view{"motion_vector"},
        std::string_view{"projection_jitter"}, std::string_view{"ui"},
        std::string_view{"imgui"}, std::string_view{"mirror"},
        std::string_view{"present"},
    };
    return containsMarker(value, markers);
}

bool unsafePreviewDirectPassSurface(std::string_view value) {
    // A base graph's conventional terminal may be named "present". It is
    // retained only after its swapchain identity has been rewritten to the
    // request-local capture.
    constexpr std::array markers{
        std::string_view{"taa"}, std::string_view{"velocity"},
        std::string_view{"motion_vector"},
        std::string_view{"projection_jitter"}, std::string_view{"ui"},
        std::string_view{"imgui"}, std::string_view{"mirror"},
    };
    return containsMarker(value, markers);
}

bool looksLikeVelocityName(std::string_view name) {
    constexpr std::array markers{
        std::string_view{"velocity"},
        std::string_view{"motion_vector"},
    };
    return containsMarker(name, markers);
}

bool declaresHistory(const nlohmann::json &feature) {
    if (!feature.contains("render_targets") ||
        !feature.at("render_targets").is_array()) {
        return false;
    }
    return std::any_of(
        feature.at("render_targets").begin(),
        feature.at("render_targets").end(),
        [](const auto &target) {
            return target.is_object() &&
                   target.value("history", false);
        });
}

bool declaresPassType(const nlohmann::json &feature,
                      std::string_view type) {
    if (!feature.contains("passes") ||
        !feature.at("passes").is_array()) {
        return false;
    }
    for (const auto &entry : feature.at("passes")) {
        if (!entry.is_object()) continue;
        const auto &pass =
            entry.contains("pass") ? entry.at("pass") : entry;
        if (pass.is_object() &&
            pass.value("type", std::string{}) == type) {
            return true;
        }
    }
    return false;
}

bool declaresVelocityTarget(const nlohmann::json &feature) {
    if (!feature.contains("render_targets") ||
        !feature.at("render_targets").is_array()) {
        return false;
    }
    return std::any_of(
        feature.at("render_targets").begin(),
        feature.at("render_targets").end(),
        [](const auto &target) {
            return target.is_object() &&
                   looksLikeVelocityName(
                       target.value("name", std::string{}));
        });
}

bool declaresUiAnchor(const nlohmann::json &feature) {
    if (!feature.contains("passes") ||
        !feature.at("passes").is_array()) {
        return false;
    }
    return std::any_of(
        feature.at("passes").begin(), feature.at("passes").end(),
        [](const auto &entry) {
            return entry.is_object() &&
                   entry.value("insert", std::string{})
                           .find("pelican_ui") != std::string::npos;
        });
}

bool declaresUnsafePreviewTarget(const nlohmann::json &feature) {
    if (!feature.contains("render_targets") ||
        !feature.at("render_targets").is_array()) {
        return false;
    }
    return std::any_of(
        feature.at("render_targets").begin(),
        feature.at("render_targets").end(),
        [](const auto &target) {
            return target.is_object() &&
                   unsafePreviewName(
                       target.value("name", std::string{}));
        });
}

bool declaresUnsafePreviewPass(const nlohmann::json &feature) {
    if (!feature.contains("passes") ||
        !feature.at("passes").is_array()) {
        return false;
    }
    for (const auto &entry : feature.at("passes")) {
        if (!entry.is_object()) continue;
        const auto &pass =
            entry.contains("pass") ? entry.at("pass") : entry;
        if (!pass.is_object()) continue;
        if (unsafePreviewName(pass.value("name", std::string{})) ||
            unsafePreviewName(pass.value("type", std::string{})) ||
            unsafePreviewName(
                entry.value("insert", std::string{}))) {
            return true;
        }
    }
    return false;
}

#if PELICAN_WITH_OPENXR
bool knownXrExcludedFeature(std::string_view name) {
    constexpr std::array names{
        std::string_view{"taa"},
        std::string_view{"velocity"},
        std::string_view{"ui"},
    };
    return std::find(names.begin(), names.end(), name) != names.end();
}
#endif

bool hasHistoryInput(const nlohmann::json &pass) {
    if (!pass.contains("input")) return false;
    const auto is_history = [](const nlohmann::json &value) {
        return value.is_string() &&
               value.get_ref<const std::string &>().ends_with(
                   "@history");
    };
    const auto &inputs = pass.at("input");
    return is_history(inputs) ||
           (inputs.is_array() &&
            std::any_of(inputs.begin(), inputs.end(), is_history));
}

bool outputsRequestLocalTerminal(const nlohmann::json &pass) {
    if (!pass.contains("output") ||
        !pass.at("output").is_object() ||
        !pass.at("output").contains("color")) {
        return false;
    }
    const auto is_request_local =
        [](const nlohmann::json &value) {
            if (!value.is_string()) return false;
            const auto &name =
                value.get_ref<const std::string &>();
            return name == "preview_capture" || name == "display";
        };
    const auto &color = pass.at("output").at("color");
    return is_request_local(color) ||
           (color.is_array() &&
            std::any_of(color.begin(), color.end(),
                        is_request_local));
}

[[noreturn]] void rejectPreviewPass(
    std::string_view graph, std::string_view pass,
    std::string_view reason) {
    throw std::runtime_error(
        "preview graph '" + std::string{graph} +
        "' rejects authored pass '" + std::string{pass} +
        "': " + std::string{reason});
}

#if PELICAN_WITH_OPENXR
[[noreturn]] void rejectXrHistory(
    std::string_view feature_name) {
    throw std::runtime_error(
        "OpenXR activation rejected history feature '" +
        std::string{feature_name} + "'");
}
#endif

void redirectSwapchainToRequestLocalCapture(
    nlohmann::json &value) {
    if (value.is_string() &&
        value.get_ref<const std::string &>() == "swapchain") {
        value = "preview_capture";
        return;
    }
    if (value.is_array() || value.is_object()) {
        for (auto &entry : value) {
            redirectSwapchainToRequestLocalCapture(entry);
        }
    }
}

void validatePreviewConfig(const nlohmann::json &config) {
    if (config.contains("projection_jitter")) {
        throw std::runtime_error(
            "preview graph '<config>' rejects authored projection_jitter");
    }
    if (config.contains("render_targets") &&
        config.at("render_targets").is_array()) {
        for (const auto &target : config.at("render_targets")) {
            if (!target.is_object()) continue;
            const auto name =
                target.value("name", std::string{"unnamed"});
            if (target.value("history", false)) {
                throw std::runtime_error(
                    "preview graph '<config>' rejects authored history target '" +
                    name + "'");
            }
            if (unsafePreviewName(name)) {
                throw std::runtime_error(
                    "preview graph '<config>' rejects authored unsafe target '" +
                    name + "'");
            }
        }
    }
    if (!config.contains("rendering_passes") ||
        !config.at("rendering_passes").is_array()) {
        throw std::runtime_error(
            "preview graph config requires rendering_passes");
    }
    for (const auto &graph : config.at("rendering_passes")) {
        if (!graph.is_object()) continue;
        const auto graph_name =
            graph.value("name", std::string{"unnamed"});
        if (!graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            continue;
        }
        for (const auto &pass : graph.at("passes")) {
            if (!pass.is_object()) continue;
            const auto pass_name =
                pass.value("name", std::string{"unnamed"});
            const auto type =
                pass.value("type", std::string{});
            if (type == "canonical_anchor") continue;
            const auto lower_name = lower(pass_name);
            const bool retained_terminal =
                outputsRequestLocalTerminal(pass) &&
                lower_name.ends_with("present") &&
                lower_name.find("mirror") == std::string::npos &&
                lower_name.find("ui") == std::string::npos;
            if ((unsafePreviewDirectPassSurface(pass_name) &&
                 !retained_terminal) ||
                unsafePreviewDirectPassSurface(type)) {
                rejectPreviewPass(
                    graph_name, pass_name,
                    "unsafe pass type/name " + type);
            }
            if (hasHistoryInput(pass)) {
                rejectPreviewPass(
                    graph_name, pass_name, "history read");
            }
            if (pass.value("writes_history", false)) {
                rejectPreviewPass(
                    graph_name, pass_name, "history write");
            }
        }
    }
}

#if PELICAN_WITH_OPENXR
void validateXrConfig(const nlohmann::json &config) {
    if (config.contains("projection_jitter")) {
        throw std::runtime_error(
            "OpenXR activation cannot include authored projection jitter");
    }
    if (config.contains("render_targets") &&
        config.at("render_targets").is_array()) {
        for (const auto &target : config.at("render_targets")) {
            if (!target.is_object()) continue;
            if (target.value("history", false)) {
                rejectXrHistory(
                    "<base rendering config>:" +
                    target.value("name", std::string{"unnamed"}));
            }
            if (looksLikeVelocityName(
                    target.value("name", std::string{}))) {
                throw std::runtime_error(
                    "OpenXR activation cannot include velocity target '" +
                    target.value("name", std::string{"unnamed"}) +
                    "'");
            }
        }
    }
    if (!config.contains("rendering_passes") ||
        !config.at("rendering_passes").is_array()) {
        return;
    }
    for (const auto &graph : config.at("rendering_passes")) {
        if (!graph.is_object() || !graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            continue;
        }
        for (const auto &pass : graph.at("passes")) {
            if (!pass.is_object()) continue;
            const auto type =
                pass.value("type", std::string{});
            if (type == "ui" || type == "velocity") {
                throw std::runtime_error(
                    "OpenXR activation cannot exclude direct " +
                    type + " pass '" +
                    pass.value("name", std::string{"unnamed"}) +
                    "'");
            }
            if (hasHistoryInput(pass)) {
                rejectXrHistory(
                    "<base rendering config>:" +
                    pass.value("name", std::string{"unnamed"}));
            }
        }
    }
}
#endif

} // namespace

std::string_view renderPipelineGraphVariantName(
    RenderPipelineGraphVariant variant) {
    switch (variant) {
    case RenderPipelineGraphVariant::flat: return "flat";
    case RenderPipelineGraphVariant::preview: return "preview";
    case RenderPipelineGraphVariant::xr: return "xr";
    }
    return "unknown";
}

std::string_view graphVariantHistoryPolicyName(
    GraphVariantHistoryPolicy policy) {
    switch (policy) {
    case GraphVariantHistoryPolicy::preserve: return "preserve";
    case GraphVariantHistoryPolicy::forbid: return "forbid";
    }
    return "unknown";
}

std::string_view graphVariantProjectionJitterPolicyName(
    GraphVariantProjectionJitterPolicy policy) {
    switch (policy) {
    case GraphVariantProjectionJitterPolicy::preserve:
        return "preserve";
    case GraphVariantProjectionJitterPolicy::forbid:
        return "forbid";
    }
    return "unknown";
}

std::string_view graphVariantViewFamilyName(
    GraphVariantViewFamily family) {
    switch (family) {
    case GraphVariantViewFamily::caller_defined:
        return "caller_defined";
    case GraphVariantViewFamily::mono: return "mono";
    case GraphVariantViewFamily::stereo: return "stereo";
    }
    return "unknown";
}

std::string_view graphVariantViewExecutionName(
    GraphVariantViewExecution execution) {
    switch (execution) {
    case GraphVariantViewExecution::caller_defined:
        return "caller_defined";
    case GraphVariantViewExecution::single_view:
        return "single_view";
    case GraphVariantViewExecution::sequential:
        return "sequential";
    }
    return "unknown";
}

std::string_view graphVariantResourceLayoutName(
    GraphVariantResourceLayout layout) {
    switch (layout) {
    case GraphVariantResourceLayout::shared_2d:
        return "shared_2d";
    case GraphVariantResourceLayout::sequential_2d:
        return "sequential_2d";
    }
    return "unknown";
}

std::string_view graphVariantTerminalName(
    GraphVariantTerminal terminal) {
    switch (terminal) {
    case GraphVariantTerminal::presentation:
        return "presentation";
    case GraphVariantTerminal::request_local_capture:
        return "request_local_capture";
    case GraphVariantTerminal::external_view:
        return "external_view";
    }
    return "unknown";
}

std::string_view graphVariantMirrorOutputName(
    GraphVariantMirrorOutput output) {
    switch (output) {
    case GraphVariantMirrorOutput::none: return "none";
    case GraphVariantMirrorOutput::left_eye: return "left_eye";
    }
    return "unknown";
}

CompiledGraphVariantPolicy compileGraphVariantPolicy(
    const GraphVariantPolicyRequest &request,
    const GraphVariantPolicyCapabilities &capabilities) {
    CompiledGraphVariantPolicy result;
    result.variant = request.variant;
    switch (request.variant) {
    case RenderPipelineGraphVariant::flat:
        return result;
    case RenderPipelineGraphVariant::preview:
        if (!capabilities.request_local_capture) {
            throw std::runtime_error(
                "preview graph variant requires request-local capture support");
        }
        result.history = GraphVariantHistoryPolicy::forbid;
        result.projection_jitter =
            GraphVariantProjectionJitterPolicy::forbid;
        result.view_family = GraphVariantViewFamily::mono;
        result.view_execution =
            GraphVariantViewExecution::single_view;
        result.terminal =
            GraphVariantTerminal::request_local_capture;
        result.view_count = 1;
        return result;
    case RenderPipelineGraphVariant::xr:
#if PELICAN_WITH_OPENXR
        if (!capabilities.sequential_stereo) {
            throw std::runtime_error(
                "XR graph variant requires sequential stereo support");
        }
        if (!capabilities.left_eye_mirror) {
            throw std::runtime_error(
                "XR graph variant requires left-eye mirror support");
        }
        result.history = GraphVariantHistoryPolicy::forbid;
        result.projection_jitter =
            GraphVariantProjectionJitterPolicy::forbid;
        result.view_family = GraphVariantViewFamily::stereo;
        result.view_execution =
            GraphVariantViewExecution::sequential;
        result.resource_layout =
            GraphVariantResourceLayout::sequential_2d;
        result.terminal = GraphVariantTerminal::external_view;
        result.mirror_output =
            GraphVariantMirrorOutput::left_eye;
        result.view_count = 2;
        result.rendering_pass_name_suffix = "#xr";
        return result;
#else
        throw std::runtime_error(
            "XR graph variant is unavailable in this build");
#endif
    }
    throw std::runtime_error("unknown render-pipeline graph variant");
}

std::string_view graphVariantFeatureDispositionName(
    GraphVariantFeatureDisposition disposition) {
    switch (disposition) {
    case GraphVariantFeatureDisposition::include: return "include";
    case GraphVariantFeatureDisposition::exclude: return "exclude";
    case GraphVariantFeatureDisposition::reject: return "reject";
    }
    return "unknown";
}

std::string_view graphVariantFeatureReasonName(
    GraphVariantFeatureReason reason) {
    switch (reason) {
    case GraphVariantFeatureReason::compatible:
        return "compatible";
    case GraphVariantFeatureReason::known_incompatible:
        return "known_incompatible";
    case GraphVariantFeatureReason::unsafe_name:
        return "unsafe_name";
    case GraphVariantFeatureReason::history: return "history";
    case GraphVariantFeatureReason::projection_jitter:
        return "projection_jitter";
    case GraphVariantFeatureReason::velocity: return "velocity";
    case GraphVariantFeatureReason::user_interface:
        return "user_interface";
    case GraphVariantFeatureReason::unsafe_surface:
        return "unsafe_surface";
    }
    return "unknown";
}

GraphVariantFeatureDecision decideGraphVariantFeature(
    const CompiledGraphVariantPolicy &policy,
    std::string_view feature_name,
    const nlohmann::json &feature) {
    GraphVariantFeatureDecision result{
        .feature_name = std::string{feature_name},
    };
    switch (policy.variant) {
    case RenderPipelineGraphVariant::flat:
        return result;
    case RenderPipelineGraphVariant::preview:
        result.disposition =
            GraphVariantFeatureDisposition::exclude;
        if (unsafePreviewName(feature_name)) {
            result.reason = GraphVariantFeatureReason::unsafe_name;
        } else if (declaresHistory(feature)) {
            result.reason = GraphVariantFeatureReason::history;
        } else if (feature.contains("projection_jitter")) {
            result.reason =
                GraphVariantFeatureReason::projection_jitter;
        } else if (declaresVelocityTarget(feature) ||
                   declaresPassType(feature, "velocity")) {
            result.reason = GraphVariantFeatureReason::velocity;
        } else if (declaresPassType(feature, "ui") ||
                   declaresUiAnchor(feature)) {
            result.reason =
                GraphVariantFeatureReason::user_interface;
        } else if (declaresUnsafePreviewTarget(feature) ||
                   declaresUnsafePreviewPass(feature)) {
            result.reason =
                GraphVariantFeatureReason::unsafe_surface;
        } else {
            result.disposition =
                GraphVariantFeatureDisposition::include;
            result.reason = GraphVariantFeatureReason::compatible;
        }
        return result;
    case RenderPipelineGraphVariant::xr:
#if PELICAN_WITH_OPENXR
        if (knownXrExcludedFeature(feature_name)) {
            result.disposition =
                GraphVariantFeatureDisposition::exclude;
            result.reason =
                GraphVariantFeatureReason::known_incompatible;
        } else if (declaresHistory(feature)) {
            result.disposition =
                GraphVariantFeatureDisposition::reject;
            result.reason = GraphVariantFeatureReason::history;
        } else if (feature.contains("projection_jitter")) {
            result.disposition =
                GraphVariantFeatureDisposition::exclude;
            result.reason =
                GraphVariantFeatureReason::projection_jitter;
        } else if (declaresPassType(feature, "velocity") ||
                   declaresVelocityTarget(feature)) {
            result.disposition =
                GraphVariantFeatureDisposition::exclude;
            result.reason = GraphVariantFeatureReason::velocity;
        } else if (declaresPassType(feature, "ui") ||
                   declaresUiAnchor(feature)) {
            result.disposition =
                GraphVariantFeatureDisposition::exclude;
            result.reason =
                GraphVariantFeatureReason::user_interface;
        }
        return result;
#else
        throw std::runtime_error(
            "XR graph variant is unavailable in this build");
#endif
    }
    throw std::runtime_error("unknown graph variant feature policy");
}

std::string graphVariantFeatureRejectionMessage(
    const CompiledGraphVariantPolicy &policy,
    const GraphVariantFeatureDecision &decision) {
    if (decision.disposition !=
        GraphVariantFeatureDisposition::reject) {
        throw std::logic_error(
            "graph variant feature rejection message requires reject disposition");
    }
    if (policy.variant == RenderPipelineGraphVariant::xr &&
        decision.reason == GraphVariantFeatureReason::history) {
        return "OpenXR activation rejected history feature '" +
               decision.feature_name + "'";
    }
    return "graph variant '" +
           std::string{renderPipelineGraphVariantName(policy.variant)} +
           "' rejected feature '" + decision.feature_name +
           "' because " +
           std::string{graphVariantFeatureReasonName(decision.reason)};
}

void transformGraphVariantConfig(
    const CompiledGraphVariantPolicy &policy,
    nlohmann::json &config) {
    if (policy.terminal ==
        GraphVariantTerminal::request_local_capture) {
        redirectSwapchainToRequestLocalCapture(config);
    }
}

void validateGraphVariantConfig(
    const CompiledGraphVariantPolicy &policy,
    const nlohmann::json &config) {
    switch (policy.variant) {
    case RenderPipelineGraphVariant::flat: return;
    case RenderPipelineGraphVariant::preview:
        validatePreviewConfig(config);
        return;
    case RenderPipelineGraphVariant::xr:
#if PELICAN_WITH_OPENXR
        validateXrConfig(config);
        return;
#else
        throw std::runtime_error(
            "XR graph variant is unavailable in this build");
#endif
    }
    throw std::runtime_error("unknown graph variant config policy");
}

} // namespace Pelican
