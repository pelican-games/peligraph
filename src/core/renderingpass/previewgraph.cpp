#include "previewgraph.hpp"

#include "renderingpassconfigloader.hpp"
#include "../../project/renderpipeline.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

namespace Pelican {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool unsafeName(std::string_view value) {
    const auto name = lower(std::string{value});
    constexpr std::array<std::string_view, 8> markers{
        "taa", "velocity", "motion_vector", "projection_jitter",
        "ui", "imgui", "mirror", "present"};
    return std::any_of(markers.begin(), markers.end(), [&](std::string_view marker) {
        return name.find(marker) != std::string::npos;
    });
}

bool unsafeDirectPassSurface(std::string_view value) {
    const auto name = lower(std::string{value});
    // A base graph's conventional terminal may be named "present".  It is
    // retained only after its swapchain identity has been rewritten to the
    // request-local capture.  Temporal/UI/mirror semantics remain rejectable.
    constexpr std::array<std::string_view, 7> markers{
        "taa", "velocity", "motion_vector", "projection_jitter",
        "ui", "imgui", "mirror"};
    return std::any_of(markers.begin(), markers.end(), [&](std::string_view marker) {
        return name.find(marker) != std::string::npos;
    });
}

bool declaresUnsafeFeatureSurface(const nlohmann::json &feature) {
    if (feature.contains("projection_jitter")) return true;
    if (feature.contains("render_targets") && feature.at("render_targets").is_array()) {
        for (const auto &target : feature.at("render_targets")) {
            if (!target.is_object()) continue;
            if (target.value("history", false) ||
                unsafeName(target.value("name", std::string{}))) {
                return true;
            }
        }
    }
    if (feature.contains("passes") && feature.at("passes").is_array()) {
        for (const auto &entry : feature.at("passes")) {
            if (!entry.is_object()) continue;
            const auto &pass = entry.contains("pass") ? entry.at("pass") : entry;
            if (!pass.is_object()) continue;
            if (unsafeName(pass.value("name", std::string{})) ||
                unsafeName(pass.value("type", std::string{})) ||
                unsafeName(entry.value("insert", std::string{}))) {
                return true;
            }
        }
    }
    return false;
}

bool hasHistoryInput(const nlohmann::json &pass) {
    if (!pass.contains("input")) return false;
    const auto is_history = [](const nlohmann::json &value) {
        return value.is_string() &&
               value.get_ref<const std::string &>().ends_with("@history");
    };
    const auto &inputs = pass.at("input");
    return is_history(inputs) ||
           (inputs.is_array() && std::any_of(inputs.begin(), inputs.end(), is_history));
}

bool outputsRequestLocalTerminal(const nlohmann::json &pass) {
    if (!pass.contains("output") || !pass.at("output").is_object() ||
        !pass.at("output").contains("color")) {
        return false;
    }
    const auto &color = pass.at("output").at("color");
    const auto is_request_local_terminal = [](const nlohmann::json &value) {
        if (!value.is_string()) return false;
        const auto &name = value.get_ref<const std::string &>();
        return name == "preview_capture" || name == "display";
    };
    return is_request_local_terminal(color) ||
           (color.is_array() &&
            std::any_of(color.begin(), color.end(), is_request_local_terminal));
}

[[noreturn]] void rejectPass(std::string_view graph, std::string_view pass,
                             std::string_view reason) {
    throw std::runtime_error("preview graph '" + std::string{graph} +
                             "' rejects authored pass '" + std::string{pass} +
                             "': " + std::string{reason});
}

std::uint64_t generationOf(const nlohmann::json &config) {
    // Keep the value exactly representable by JSON number consumers.
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : config.dump()) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    hash &= (1ull << 53u) - 1u;
    return hash == 0 ? 1 : hash;
}

void redirectSwapchainToRequestLocalCapture(nlohmann::json &value) {
    if (value.is_string() && value.get_ref<const std::string &>() == "swapchain") {
        value = "preview_capture";
        return;
    }
    if (value.is_array()) {
        for (auto &entry : value) redirectSwapchainToRequestLocalCapture(entry);
    } else if (value.is_object()) {
        for (auto &entry : value) redirectSwapchainToRequestLocalCapture(entry);
    }
}

} // namespace

bool includeFeatureInPreviewGraph(std::string_view feature_name,
                                  const nlohmann::json &feature) {
    // Unsafe feature envelopes are removed atomically.  This retains the
    // composer's dependency/anchor validation and records each exclusion.
    return !unsafeName(feature_name) && !declaresUnsafeFeatureSurface(feature);
}

void validatePreviewGraphConfig(const nlohmann::json &config) {
    if (config.contains("projection_jitter")) {
        throw std::runtime_error("preview graph '<config>' rejects authored projection_jitter");
    }
    if (config.contains("render_targets") && config.at("render_targets").is_array()) {
        for (const auto &target : config.at("render_targets")) {
            if (!target.is_object()) continue;
            const auto name = target.value("name", std::string{"unnamed"});
            if (target.value("history", false)) {
                throw std::runtime_error("preview graph '<config>' rejects authored history target '" +
                                         name + "'");
            }
            if (unsafeName(name)) {
                throw std::runtime_error("preview graph '<config>' rejects authored unsafe target '" +
                                         name + "'");
            }
        }
    }
    if (!config.contains("rendering_passes") ||
        !config.at("rendering_passes").is_array()) {
        throw std::runtime_error("preview graph config requires rendering_passes");
    }
    for (const auto &graph : config.at("rendering_passes")) {
        if (!graph.is_object()) continue;
        const auto graph_name = graph.value("name", std::string{"unnamed"});
        if (!graph.contains("passes") || !graph.at("passes").is_array()) continue;
        for (const auto &pass : graph.at("passes")) {
            if (!pass.is_object()) continue;
            const auto pass_name = pass.value("name", std::string{"unnamed"});
            const auto type = pass.value("type", std::string{});
            if (type == "canonical_anchor") continue;
            const auto lower_name = lower(pass_name);
            const bool retained_terminal = outputsRequestLocalTerminal(pass) &&
                lower_name.ends_with("present") &&
                lower_name.find("mirror") == std::string::npos &&
                lower_name.find("ui") == std::string::npos;
            if ((unsafeDirectPassSurface(pass_name) && !retained_terminal) ||
                unsafeDirectPassSurface(type)) {
                rejectPass(graph_name, pass_name, "unsafe pass type/name " + type);
            }
            if (hasHistoryInput(pass)) rejectPass(graph_name, pass_name, "history read");
            if (pass.value("writes_history", false)) {
                rejectPass(graph_name, pass_name, "history write");
            }
        }
    }
}

PreviewGraphProgram precompilePreviewGraph(
    std::string_view rendering_config_json,
    const std::function<std::string(std::string_view)> &load_feature_json,
    bool runtime_shader_compiler_enabled) {
    const auto base = loadRenderingPassConfigJsonFromString(rendering_config_json,
                                                             "preview graph");
    auto resolved = resolveRenderPipeline(
        RenderPipelineRequest{base, "preview graph"},
        RenderEnvironmentCapabilities{runtime_shader_compiler_enabled,
                                      RenderPipelineGraphVariant::preview},
        RenderPipelineResolveDependencies{
            .load_feature_json = load_feature_json,
            .load_pipeline_json = load_feature_json,
            .include_feature = includeFeatureInPreviewGraph,
            // The terminal is compiled against a request-owned image
            // identity. No swapchain image, mirror sink, present queue, or
            // shared descriptor view is representable in the retained
            // program.
            .transform_config = redirectSwapchainToRequestLocalCapture,
            .validate_config = validatePreviewGraphConfig,
        });

    PreviewGraphProgram result;
    result.excluded_feature_names =
        std::move(resolved.excluded_feature_names);
    result.composed_config = std::move(resolved.normalized_config);
    result.generation = generationOf(result.composed_config);
    for (const auto &graph : result.composed_config.at("rendering_passes")) {
        if (!graph.is_object() || !graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            continue;
        }
        for (const auto &pass : graph.at("passes")) {
            if (pass.is_object() &&
                pass.value("type", std::string{}) != "canonical_anchor") {
                result.pass_names.push_back(pass.value("name", std::string{"unnamed"}));
            }
        }
    }
    // The authored swapchain terminal is a semantic output in this program;
    // PreviewExecutor binds it to its request-local capture image.
    return result;
}

} // namespace Pelican
