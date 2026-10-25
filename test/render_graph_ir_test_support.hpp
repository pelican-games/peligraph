#pragma once

// RC-3c1b: a completed graph's JSON is read by parseRenderGraph only. A test
// that wrote a rendering config (in the normal form or in the short forms)
// reads it into the IR here, the way the compiler does: the config's short
// forms normalized (normalizeRenderingJson), the test-only single-graph form
// (top-level passes / compute_tasks) wrapped into rendering_passes, then
// parsed with the engine's pass-field ownership.

#include "../src/core/renderingpass/passfieldownershipcapabilities.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/rendergraphir.hpp"
#include "../src/project/rendergraphparse.hpp"
#include "../src/project/renderingjsonnormalize.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string_view>
#include <string>
#include <vector>

namespace Pelican::TestSupport {

// The single-graph form as the normal form: the config becomes the one graph
// of rendering_passes, its render_targets, buffers and compute_tasks (and the
// tasks' tables) the config's (RC-3c2a / RC-3c2b1: a graph declares none of
// its own). A planning fixture's image that only
// names itself (what the planner read of a graph's own image: the name and
// `history`) is completed as a sampled color image: extent_scale 1,
// R8G8B8A8_UNORM, COLOR_ATTACHMENT + SAMPLED. A config with
// rendering_passes, or neither passes nor compute tasks, is returned as it
// is.
inline nlohmann::json wrapSingleGraph(const nlohmann::json &config) {
    if (config.contains("rendering_passes") ||
        !(config.contains("passes") || config.contains("compute_tasks"))) {
        return config;
    }
    auto graph = config;
    nlohmann::json result = nlohmann::json::object();
    for (const auto *key : {"render_targets", "buffers", "compute_tasks", "ray_task_acceleration_structures",
                            "ray_task_callable_records"}) {
        if (const auto found = graph.find(key); found != graph.end()) {
            result[key] = std::move(*found);
            graph.erase(key);
        }
    }
    if (result.contains("render_targets") && result.at("render_targets").is_array()) {
        for (auto &image : result.at("render_targets")) {
            if (!image.is_object()) continue;
            if (!image.contains("extent_scale")) image["extent_scale"] = 1.0;
            if (!image.contains("format") && !image.contains("format_class")) image["format"] = "R8G8B8A8_UNORM";
            if (!image.contains("usage")) image["usage"] = nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"});
        }
    }
    result["rendering_passes"] = nlohmann::json::array({std::move(graph)});
    return result;
}

inline RenderGraphIR renderGraphOf(const nlohmann::json &config) {
    return parseRenderGraph(normalizeRenderingJson(wrapSingleGraph(config)),
                            buildPassFieldOwnershipCapabilities());
}

// The planner's view of every graph (frameGraphDefinitionOf).
inline std::vector<FrameGraphDefinition> frameGraphsOf(const RenderGraphIR &ir) {
    std::vector<FrameGraphDefinition> result;
    result.reserve(ir.graphs.size());
    for (const auto &graph : ir.graphs) result.push_back(frameGraphDefinitionOf(ir, graph));
    return result;
}

inline std::vector<FrameGraphDefinition> frameGraphsOf(const nlohmann::json &config) {
    return frameGraphsOf(renderGraphOf(config));
}

// The one graph of a single-graph config (or of a config with one graph).
inline FrameGraphDefinition frameGraphOf(const nlohmann::json &config) {
    auto graphs = frameGraphsOf(config);
    if (graphs.size() != 1) throw std::runtime_error("expected one graph");
    return std::move(graphs.front());
}

// The frame plan of each graph (planRenderGraph).
inline std::vector<FramePlan> framePlansOf(const nlohmann::json &config) {
    const auto ir = renderGraphOf(config);
    std::vector<FramePlan> result;
    for (const auto &graph : ir.graphs) result.push_back(planRenderGraph(ir, graph));
    return result;
}

// The logical graph of each graph (toLogicalGraph).
inline std::vector<CompiledLogicalRenderGraph> logicalGraphsOf(
    const RenderGraphIR &ir, const ToLogicalGraphOptions &options = {}) {
    std::vector<CompiledLogicalRenderGraph> result;
    for (const auto &graph : ir.graphs) result.push_back(toLogicalGraph(ir, graph, options));
    return result;
}

inline std::vector<CompiledLogicalRenderGraph> logicalGraphsOf(const nlohmann::json &config) {
    return logicalGraphsOf(renderGraphOf(config));
}

// requireRenderGraphInputsProduced on one graph of a config, planned as the
// compiler plans it.
inline void requireInputsProducedOf(const nlohmann::json &config, std::string_view graph_name) {
    const auto ir = renderGraphOf(config);
    const auto *graph = findGraph(ir, graph_name);
    if (graph == nullptr) throw std::runtime_error("no graph " + std::string{graph_name});
    requireRenderGraphInputsProduced(ir, *graph, planRenderGraph(ir, *graph));
}

// RC-3c1b: a named refusal (GraphRegistryError): its code, and each field of
// `subject` with that value in its detail.
template <class Call>
void requireNamedRefusal(Call &&call, std::string_view code, const nlohmann::json &subject = {}) {
    try {
        call();
    } catch (const GraphRegistryError &error) {
        INFO(error.what());
        const std::string expected{code};
        CHECK(error.code() == expected);
        if (subject.is_object()) {
            for (const auto &[key, value] : subject.items()) {
                INFO("detail field " << key);
                CHECK(error.detail().value(key, nlohmann::json()) == value);
            }
        }
        return;
    } catch (const std::exception &error) {
        FAIL("refused without a name: " << error.what());
    }
    FAIL("expected the refusal " << code);
}

} // namespace Pelican::TestSupport
