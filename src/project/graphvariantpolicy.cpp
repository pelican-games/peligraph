#include "graphvariantpolicy.hpp"

#include "featurecompose.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

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

// G-6: the words of a name. Words are split at every character that is not a
// letter or a digit, where a lower-case letter meets an upper-case one, and
// where letters meet digits; they come back in lower case. "fluid_sim" is
// {fluid, sim}, "debugUI" is {debug, ui}, "taa2" is {taa, 2}.
std::vector<std::string> nameWords(std::string_view value) {
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) words.push_back(std::move(word));
        word.clear();
    };
    char previous = '\0';
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) == 0) {
            flush();
            previous = '\0';
            continue;
        }
        const auto prev = static_cast<unsigned char>(previous);
        const bool case_break =
            previous != '\0' && std::islower(prev) != 0 &&
            std::isupper(ch) != 0;
        const bool digit_break =
            previous != '\0' &&
            (std::isdigit(prev) != 0) != (std::isdigit(ch) != 0);
        if (case_break || digit_break) flush();
        word.push_back(static_cast<char>(std::tolower(ch)));
        previous = raw;
    }
    flush();
    return words;
}

// G-6: a marker matches whole words, never a piece of one. A marker of
// several words ("motion_vector") matches that run of words; its last word
// may carry a plural "s" ("motion_vectors"). So "ui" is found in "pelican_ui"
// and "ui_overlay" and not in "fluid_sim" or "build_grid", and "present" is
// found in "final_present" and not in "presentation".
bool containsMarkerWord(std::string_view value,
                        std::span<const std::string_view> markers) {
    const auto words = nameWords(value);
    return std::any_of(
        markers.begin(), markers.end(),
        [&](const std::string_view marker) {
            const auto wanted = nameWords(marker);
            if (wanted.empty() || wanted.size() > words.size()) {
                return false;
            }
            for (std::size_t start = 0;
                 start + wanted.size() <= words.size(); ++start) {
                bool match = true;
                for (std::size_t index = 0;
                     match && index < wanted.size(); ++index) {
                    const auto &word = words[start + index];
                    const auto &want = wanted[index];
                    match = word == want ||
                            (index + 1 == wanted.size() &&
                             word == want + "s");
                }
                if (match) return true;
            }
            return false;
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
    return containsMarkerWord(value, markers);
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
    return containsMarkerWord(value, markers);
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

// A pass placed on the UI anchor draws over the UI: the one placement a
// preview must never take, whatever the pass calls itself. Exactly the two
// spellings of that anchor; an insert that merely mentions "ui" is not it.
bool declaresUiAnchor(const nlohmann::json &feature) {
    if (!feature.contains("passes") ||
        !feature.at("passes").is_array()) {
        return false;
    }
    return std::any_of(
        feature.at("passes").begin(), feature.at("passes").end(),
        [](const auto &entry) {
            if (!entry.is_object()) return false;
            const auto insert = entry.value("insert", std::string{});
            return insert == "after:pelican_ui" ||
                   insert == "before:pelican_ui";
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
        // G-6: judged by the pass itself. Where it is inserted is not a
        // property of the pass: "before:present" is just a place, and a pass
        // anchored on an excluded feature's pass is excluded by
        // GraphVariantFeatureSelection, which knows that feature.
        if (unsafePreviewName(pass.value("name", std::string{})) ||
            unsafePreviewName(pass.value("type", std::string{}))) {
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
            constexpr std::array overlay_words{
                std::string_view{"mirror"}, std::string_view{"ui"},
            };
            const bool retained_terminal =
                outputsRequestLocalTerminal(pass) &&
                lower_name.ends_with("present") &&
                !containsMarkerWord(pass_name, overlay_words);
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
    case GraphVariantFeatureReason::dependency:
        return "dependency";
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
        if (feature.contains("lighting_data") &&
            feature.at("lighting_data").is_object() &&
            feature.at("lighting_data").value(
                "xr_path", std::string{}) ==
                "small_light_v1") {
            result.disposition =
                GraphVariantFeatureDisposition::exclude;
            result.reason =
                GraphVariantFeatureReason::known_incompatible;
        } else if (knownXrExcludedFeature(feature_name)) {
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

namespace {

// The names a JSON value lists: a string, or the strings of an array.
void forEachListedName(const nlohmann::json &value,
                       const std::function<void(const std::string &)> &visit) {
    if (value.is_string()) {
        visit(value.get_ref<const std::string &>());
    } else if (value.is_array()) {
        for (const auto &entry : value) {
            if (entry.is_string()) {
                visit(entry.get_ref<const std::string &>());
            }
        }
    }
}

// Every resource a pass's `input` / `output` names, as a string anywhere
// under them. A history read ("name@history") names the resource before '@'.
void forEachResourceName(const nlohmann::json &value,
                         const std::function<void(const std::string &)> &visit) {
    if (value.is_string()) {
        const auto &name = value.get_ref<const std::string &>();
        const auto at = name.find('@');
        visit(at == std::string::npos ? name : name.substr(0, at));
        return;
    }
    if (value.is_array() || value.is_object()) {
        for (const auto &entry : value) {
            forEachResourceName(entry, visit);
        }
    }
}

// The pass of a feature `passes[]` entry ({insert, pass}); a bare pass object
// is accepted the same way decideGraphVariantFeature accepts it.
const nlohmann::json *featurePassOf(const nlohmann::json &entry) {
    if (!entry.is_object()) return nullptr;
    const auto &pass = entry.contains("pass") ? entry.at("pass") : entry;
    return pass.is_object() ? &pass : nullptr;
}

const nlohmann::json *arrayField(const nlohmann::json &object,
                                 std::string_view field) {
    const auto found = object.find(std::string{field});
    return found != object.end() && found->is_array() ? &*found : nullptr;
}

const nlohmann::json *objectField(const nlohmann::json &object,
                                  std::string_view field) {
    const auto found = object.find(std::string{field});
    return found != object.end() && found->is_object() ? &*found : nullptr;
}

std::string insertAnchor(const std::string &insert) {
    for (const std::string_view prefix : {std::string_view{"before:"},
                                          std::string_view{"after:"}}) {
        if (insert.starts_with(prefix)) {
            return insert.substr(prefix.size());
        }
    }
    return {};
}

} // namespace

GraphVariantFeatureSelection::GraphVariantFeatureSelection(
    CompiledGraphVariantPolicy policy)
    : policy_{std::move(policy)} {}

GraphVariantFeatureDecision GraphVariantFeatureSelection::decide(
    std::string_view feature_name, const nlohmann::json &feature) {
    auto decision =
        decideGraphVariantFeature(policy_, feature_name, feature);
    if (decision.disposition ==
        GraphVariantFeatureDisposition::include) {
        if (const auto *owner = usedExcludedFeature(feature)) {
            decision.disposition =
                GraphVariantFeatureDisposition::exclude;
            decision.reason = GraphVariantFeatureReason::dependency;
            decision.depends_on = *owner;
        }
    }
    if (decision.disposition ==
        GraphVariantFeatureDisposition::exclude) {
        carryExcluded(decision.feature_name, feature);
    }
    return decision;
}

void GraphVariantFeatureSelection::carryExcluded(
    const std::string &feature_name, const nlohmann::json &feature) {
    if (!feature.is_object()) return;
    const auto keep = [&feature_name](auto &names,
                                      const std::string &name) {
        if (!name.empty()) names.try_emplace(name, feature_name);
    };
    if (const auto *passes = arrayField(feature, "passes")) {
        for (const auto &entry : *passes) {
            if (const auto *pass = featurePassOf(entry)) {
                keep(excluded_passes_,
                     pass->value("name", std::string{}));
            }
        }
    }
    if (const auto *tasks = arrayField(feature, "compute_tasks")) {
        for (const auto &task : *tasks) {
            if (task.is_object()) {
                keep(excluded_passes_,
                     task.value("name", std::string{}));
            }
        }
    }
    for (const std::string_view field :
         {std::string_view{"render_targets"},
          std::string_view{"buffers"}}) {
        if (const auto *resources = arrayField(feature, field)) {
            for (const auto &resource : *resources) {
                if (resource.is_object()) {
                    keep(excluded_resources_,
                         resource.value("name", std::string{}));
                }
            }
        }
    }
}

const std::string *GraphVariantFeatureSelection::usedExcludedFeature(
    const nlohmann::json &feature) const {
    if (!feature.is_object() ||
        (excluded_passes_.empty() && excluded_resources_.empty())) {
        return nullptr;
    }
    const std::string *owner = nullptr;
    const auto pass_named = [&](const std::string &name) {
        if (owner != nullptr) return;
        if (const auto found = excluded_passes_.find(name);
            found != excluded_passes_.end()) {
            owner = &found->second;
        }
    };
    const auto resource_named = [&](const std::string &name) {
        if (owner != nullptr) return;
        if (const auto found = excluded_resources_.find(name);
            found != excluded_resources_.end()) {
            owner = &found->second;
        }
    };
    // A graph node of this feature: its explicit edges and what it reads and
    // writes.
    const auto node = [&](const nlohmann::json &pass) {
        for (const std::string_view edge :
             {std::string_view{"after"}, std::string_view{"before"}}) {
            if (const auto found = pass.find(std::string{edge});
                found != pass.end()) {
                forEachListedName(*found, pass_named);
            }
        }
        for (const std::string_view field :
             {std::string_view{"input"}, std::string_view{"output"}}) {
            if (const auto found = pass.find(std::string{field});
                found != pass.end()) {
                forEachResourceName(*found, resource_named);
            }
        }
    };
    if (const auto *passes = arrayField(feature, "passes")) {
        for (const auto &entry : *passes) {
            if (!entry.is_object()) continue;
            // (a) The anchor an insert names. A canonical anchor is the
            // pipeline's own node, never a feature's pass of the same name.
            const auto anchor =
                insertAnchor(entry.value("insert", std::string{}));
            if (!anchor.empty() &&
                !isCanonicalRenderPassAnchor(anchor)) {
                pass_named(anchor);
            }
            if (const auto *pass = featurePassOf(entry)) node(*pass);
        }
    }
    if (const auto *tasks = arrayField(feature, "compute_tasks")) {
        for (const auto &task : *tasks) {
            if (task.is_object()) node(task);
        }
    }
    if (const auto *overrides = objectField(feature, "pass_overrides")) {
        for (auto entry = overrides->begin(); entry != overrides->end();
             ++entry) {
            pass_named(entry.key());
        }
    }
    if (const auto *overrides =
            objectField(feature, "render_target_overrides")) {
        for (auto entry = overrides->begin(); entry != overrides->end();
             ++entry) {
            resource_named(entry.key());
        }
    }
    return owner;
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
