#pragma once

// RC-3c1b: what the production compiler program tests share: a temporary
// project, the providers (two graph transforms, a tagged subgraph
// replacement, the builtin render strategy), the configuration they
// rewrite, and the compile the registration runs (its own variant
// requests, makeRenderGraphVariantRequests). In an unnamed namespace: each
// test file has its own copy.
#include "../src/core/container.hpp"
#include "../src/core/imgui/imguiruntime.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/registrationownerhost.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderingpassconfigregistration.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/project/authoringlowering.hpp"
#include "../src/project/logicalrendergraph.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "../src/project/renderingjsonnormalize.hpp"
#include "render_graph_ir_test_support.hpp"
#include "render_graph_root_test_support.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace Pelican {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

Json image(const std::string &name, Json usage = Json::array({"COLOR_ATTACHMENT", "SAMPLED"})) {
    return Json{{"name", name},
                {"extent_scale", 1.0},
                {"format", "R16G16B16A16_SFLOAT"},
                {"usage", std::move(usage)}};
}

Json fullscreen(const std::string &name, const std::vector<std::string> &input, const std::string &output,
                const std::vector<std::string> &after = {}) {
    Json pass{{"name", name},
              {"type", "fullscreen"},
              {"output", {{"color", Json::array({Json{{"target", output}}})}, {"depth", nullptr}}},
              {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://scene_present"}}}};
    if (!input.empty()) pass["input"] = input;
    if (!after.empty()) pass["after"] = after;
    return pass;
}

// --- the providers -------------------------------------------------------------

enum class TransformBehavior {
    // t<index>_pass reads what tone read and tone reads its output.
    chain,
    // x (writes x_out, read by nobody) at the head of the graph.
    independent,
    // draw waits on present: a cycle.
    cycle,
    // a config compute task x_trace that queries the scene.
    ray_task,
    // RC-3c2b1: a config ray tracing task x_call (no ray group layout) with
    // one callable and its record schema ({value: u32}), its inline record
    // {value: 1}.
    ray_callable_task,
    // a second pass named draw.
    duplicate,
};

struct TransformState {
    int index = 1;
    TransformBehavior behavior = TransformBehavior::chain;
    // What the callback received, and what it answered.
    std::string received_config;
    std::string received_graphs;
    std::uint64_t received_fingerprint = 0;
    std::string response;
};

RenderGraphTransform::Status resolveTransform(void *context,
                                              const RenderGraphTransform::ResolveGraphTransformInputV1 *input,
                                              RenderGraphTransform::GraphTransformOutputV1 *output) noexcept {
    auto &state = *static_cast<TransformState *>(context);
    try {
        state.received_config.assign(input->config_json_utf8, input->config_json_size);
        state.received_graphs.assign(input->logical_graphs_json_utf8, input->logical_graphs_json_size);
        state.received_fingerprint = input->logical_graphs_fingerprint;
        auto config = Json::parse(state.received_config);
        const auto tag = "t" + std::to_string(state.index);
        for (auto &graph : config["rendering_passes"]) {
            auto &passes = graph["passes"];
            const auto named = [&](std::string_view name) {
                return std::find_if(passes.begin(), passes.end(),
                                    [&](const Json &pass) { return pass.value("name", "") == name; });
            };
            switch (state.behavior) {
            case TransformBehavior::chain: {
                auto tone = named("tone");
                if (tone == passes.end()) break;
                const auto read = (*tone)["input"];
                (*tone)["input"] = Json::array({tag + "_scratch"});
                passes.insert(tone, fullscreen(tag + "_pass", read, tag + "_scratch"));
                config["render_targets"].push_back(image(tag + "_scratch"));
                break;
            }
            case TransformBehavior::independent:
                passes.insert(passes.begin(), fullscreen("x", {}, "x_out"));
                config["render_targets"].push_back(image("x_out"));
                break;
            case TransformBehavior::cycle:
                (*named("draw"))["after"] = Json::array({"present"});
                break;
            case TransformBehavior::ray_task:
            case TransformBehavior::ray_callable_task:
                break;
            case TransformBehavior::duplicate:
                passes.insert(passes.begin(), fullscreen("draw", {}, "scratch"));
                break;
            }
        }
        if (state.behavior == TransformBehavior::ray_task) {
            config["buffers"].push_back(Json{{"name", "hits"}});
            config["compute_tasks"].push_back(
                Json{{"name", "x_trace"},
                     {"shader", "engine://shaders/compute/fixture"},
                     {"writes", Json::array({"hits"})},
                     {"dispatch", {{"groups", Json::array({1, 1, 1})}}},
                     {"ray_query", {{"acceleration_structure", "scene"}}}});
        }
        if (state.behavior == TransformBehavior::ray_callable_task) {
            config["compute_tasks"].push_back(
                Json{{"name", "x_call"},
                     {"ray_tracing",
                      {{"raygen", "shaders/x_call"},
                       {"miss", "shaders/x_call"},
                       {"closesthit", "shaders/x_call"},
                       {"callable", Json::array({"shaders/x_callable"})},
                       {"callable_record", Json::array({Json{{"name", "value"}, {"type", "u32"}}})},
                       {"callable_records", Json::array({Json{{"value", 1}}})}}},
                     {"dispatch", {{"rays", Json::array({1, 1, 1})}}}});
        }
        state.response = config.dump();
    } catch (...) {
        return RenderGraphTransform::Status::provider_error;
    }
    static const std::string implementation = "rc3c1b.transform@1";
    *output = RenderGraphTransform::descriptor<RenderGraphTransform::GraphTransformOutputV1>();
    output->implementation_id_utf8 = implementation.data();
    output->implementation_id_size = static_cast<std::uint32_t>(implementation.size());
    output->config_json_utf8 = state.response.data();
    output->config_json_size = static_cast<std::uint32_t>(state.response.size());
    return RenderGraphTransform::Status::ok;
}

// Splits tone into tone_a (what tone read -> scratch) and tone_b (scratch ->
// what tone wrote).
struct SubgraphState {
    std::string received_authored;
    std::uint64_t received_fingerprint = 0;
    std::string response;
};

RenderSubgraph::Status resolveRegion(void *context, const RenderSubgraph::ResolveRegionInputV1 *input,
                                     RenderSubgraph::RegionReplacementV1 *output) noexcept {
    auto &state = *static_cast<SubgraphState *>(context);
    try {
        state.received_authored.assign(input->authored_subgraph_json_utf8, input->authored_subgraph_json_size);
        state.received_fingerprint = input->contract->fingerprint;
        const auto authored = Json::parse(state.received_authored);
        const auto &tone = authored.at(0);
        const auto read = tone.at("input").get<std::vector<std::string>>();
        const auto written = tone.at("output").at("color").at(0).at("target").get<std::string>();
        auto a = fullscreen("tone_a", read, "scratch");
        auto b = fullscreen("tone_b", {"scratch"}, written, {"tone_a"});
        a["regions"] = tone.at("regions");
        b["regions"] = tone.at("regions");
        state.response = Json::array({a, b}).dump();
    } catch (...) {
        return RenderSubgraph::Status::provider_error;
    }
    static const std::string implementation = "rc3c1b.subgraph@1";
    *output = RenderSubgraph::descriptor<RenderSubgraph::RegionReplacementV1>();
    output->implementation_id_utf8 = implementation.data();
    output->implementation_id_size = static_cast<std::uint32_t>(implementation.size());
    output->subgraph_json_utf8 = state.response.data();
    output->subgraph_json_size = static_cast<std::uint32_t>(state.response.size());
    return RenderSubgraph::Status::ok;
}

// `global`: in the engine's registries, where a registration takes its
// providers from (otherwise in the test's own).
struct Providers {
    GraphTransformRegistry own_transforms;
    SubgraphReplacementRegistry own_subgraphs;
    RenderStrategyRegistry strategies;
    GraphTransformRegistry &transforms;
    SubgraphReplacementRegistry &subgraphs;
    TransformState first{.index = 1};
    TransformState second{.index = 2};
    SubgraphState subgraph;
    internal::RegistrationOwner owner = internal::allocateRegistrationOwner();

    explicit Providers(bool global = false)
        : transforms{global ? graphTransformRegistry() : own_transforms},
          subgraphs{global ? subgraphReplacementRegistry() : own_subgraphs} {
        const auto add = [&](std::string_view name, TransformState &state) {
            auto provider = RenderGraphTransform::descriptor<RenderGraphTransform::ProviderV1>();
            provider.capability_bits = RenderGraphTransform::builtinProviderCapabilitiesV1;
            provider.name_utf8 = name.data();
            provider.name_size = static_cast<std::uint32_t>(name.size());
            provider.context = &state;
            provider.resolve_graph_transform = resolveTransform;
            RenderGraphTransform::ProviderHandleV1 handle{};
            REQUIRE(transforms.registerProvider(provider, owner, handle) == RenderGraphTransform::Status::ok);
        };
        static constexpr std::string_view first_name = "rc3c1b.transform_1";
        static constexpr std::string_view second_name = "rc3c1b.transform_2";
        static constexpr std::string_view subgraph_name = "rc3c1b.subgraph";
        add(first_name, first);
        add(second_name, second);
        auto provider = RenderSubgraph::descriptor<RenderSubgraph::ProviderV1>();
        provider.capability_bits = RenderSubgraph::builtinProviderCapabilitiesV1;
        provider.name_utf8 = subgraph_name.data();
        provider.name_size = static_cast<std::uint32_t>(subgraph_name.size());
        provider.context = &subgraph;
        provider.resolve_region = resolveRegion;
        RenderSubgraph::ProviderHandleV1 handle{};
        REQUIRE(subgraphs.registerProvider(provider, owner, handle) == RenderSubgraph::Status::ok);
        transforms.activateOwner(owner);
        subgraphs.activateOwner(owner);
    }
    ~Providers() {
        transforms.releaseOwner(owner);
        subgraphs.releaseOwner(owner);
        internal::releaseRegistrationOwner(owner);
    }
    Providers(const Providers &) = delete;
    Providers &operator=(const Providers &) = delete;
};

// draw -> tone (the tagged region) -> present; `transforms` graph transforms
// in front of tone, the region replaced when `subgraph`, the builtin
// authored render strategy.
Json section(int transforms, bool subgraph) {
    auto tone = fullscreen("tone", {"scene"}, "post", {"draw"});
    tone["regions"] = Json::array({"rc3c1b.tone"});
    Json graph{{"name", "main"},
               {"passes", Json::array({fullscreen("draw", {}, "scene"), tone,
                                       fullscreen("present", {"post"}, "swapchain", {"tone"})})}};
    if (subgraph) {
        graph["region_replacements"] =
            Json::array({Json{{"region", "rc3c1b.tone"}, {"provider", "rc3c1b.subgraph"}}});
    }
    Json result{{"render_strategy", {{"name", "rc3c1b.strategy"}}},
                {"render_targets", Json::array({image("scene"), image("scratch"), image("post")})},
                {"rendering_passes", Json::array({graph})}};
    auto requests = Json::array();
    for (int index = 1; index <= transforms; ++index) {
        requests.push_back(Json{{"name", "rc3c1b.t" + std::to_string(index)},
                                {"provider", "rc3c1b.transform_" + std::to_string(index)}});
    }
    if (!requests.empty()) result["graph_transforms"] = std::move(requests);
    return result;
}

// A temporary project: a graph root whose buffers and compute tasks (if any)
// are in a project task feature, as a graph root keeps them.
struct Project {
    fs::path root;
    PathResolver paths;

    Project() {
        root = fs::temp_directory_path() /
               ("pelican-rc3c1b-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        paths.setup(root, false);
    }
    ~Project() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    Project(const Project &) = delete;
    Project &operator=(const Project &) = delete;

    Json rootOf(Json rendering_section) {
        return TestSupport::renderGraphRootWithTaskFeature(std::move(rendering_section), root);
    }

    std::string load(std::string_view reference) const {
        constexpr std::string_view project = "project://";
        if (reference.starts_with(project)) {
            std::ifstream file{root / std::string{reference.substr(project.size())}, std::ios::binary};
            std::ostringstream text;
            text << file.rdbuf();
            return text.str();
        }
        constexpr std::string_view engine = "engine://";
        return engineResourceOrThrow(reference.substr(engine.size()));
    }
};

enum class Variants { flat, flat_and_preview, flat_xr_and_preview, preview, xr };

// The registration's own requests (makeRenderGraphVariantRequests): ImGui is
// composed into flat when the launch configuration runs it.
std::vector<RenderCompilerProgramVariantRequest> requestsOf(Variants variants) {
    std::vector<RenderingPassConfigRegistrationDependencies::Options> options;
    if (variants == Variants::xr) {
        options.push_back({.graph_variant = RenderPipelineGraphVariant::xr});
        return makeRenderGraphVariantRequests(std::span{options}, false);
    }
    if (variants != Variants::preview) options.push_back({.graph_variant = RenderPipelineGraphVariant::flat});
#if PELICAN_WITH_OPENXR
    if (variants == Variants::flat_xr_and_preview) options.push_back({.graph_variant = RenderPipelineGraphVariant::xr});
#endif
    return makeRenderGraphVariantRequests(std::span{options}, variants != Variants::flat);
}

std::size_t runtimeVariantsOf(Variants variants) {
    switch (variants) {
    case Variants::flat:
    case Variants::flat_and_preview:
        return 1;
    case Variants::flat_xr_and_preview:
#if PELICAN_WITH_OPENXR
        return 2;
#else
        return 1;
#endif
    case Variants::preview:
        return 0;
    case Variants::xr:
        return 1;
    }
    return 0;
}

// `imgui`: the launch configuration runs ImGui (the default one does; a
// headless driver does not). `program`: another compiler program than the
// engine's default.
RenderCompilerProgramOutput compile(Project &project, Providers &providers, const Json &rendering_section,
                                    Variants variants, bool imgui = true,
                                    const VulkanRenderCompilerBackendContext *device = nullptr,
                                    const RenderCompilerProgram *program = nullptr) {
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = !imgui;
    const auto root = project.rootOf(rendering_section);
    const auto lowering = lowerAuthoringDocument(root);
    const auto requests = requestsOf(variants);
    const auto transforms = providers.transforms.snapshot();
    const auto strategies = providers.strategies.snapshot();
    const auto subgraphs = providers.subgraphs.snapshot();
    const VulkanRenderCompilerBackendContext compiler_only{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    const auto &backend = device != nullptr ? *device : compiler_only;
    return runRenderCompilerProgram(
        program != nullptr ? *program : defaultVulkanRenderCompilerProgram(),
        RenderCompilerProgramInput{
            .rendering_config = lowering.document,
            .source_name = "rc3c1b",
            .path_resolver = project.paths,
            .load_document = [&project](std::string_view reference) { return project.load(reference); },
            .runtime_shader_compiler_enabled = true,
            .graph_transforms = transforms,
            .render_strategies = strategies,
            .subgraph_replacements = subgraphs,
            .backend_context = backend,
            .variants = requests,
        });
}

// What the transform chain hands the next provider after a response: the
// response, normalized, parsed once, its logical graphs.
struct Revision {
    std::string config;
    std::string graphs;
    std::uint64_t fingerprint = 0;
    RenderGraphIR ir;
};

Revision revisionOf(const std::string &response) {
    const auto candidate = normalizeProviderRenderingJson(Json::parse(response));
    Revision result;
    result.config = nlohmann::ordered_json(candidate).dump();
    result.ir = TestSupport::renderGraphOf(candidate);
    auto logical = TestSupport::logicalGraphsOf(result.ir);
    result.fingerprint = logicalGraphSetFingerprint(logical);
    std::sort(logical.begin(), logical.end(),
              [](const auto &left, const auto &right) { return left.name < right.name; });
    auto encoded = nlohmann::ordered_json::array();
    for (const auto &graph : logical) encoded.push_back(compiledLogicalRenderGraphToJson(graph));
    result.graphs = encoded.dump();
    return result;
}

const RenderCompilerProgramVariantOutput &variantOf(const RenderCompilerProgramOutput &output,
                                                    RenderPipelineGraphVariant variant) {
    const auto found = std::find_if(output.variants.begin(), output.variants.end(),
                                    [&](const auto &entry) { return entry.graph_variant == variant; });
    REQUIRE(found != output.variants.end());
    return *found;
}

} // namespace
} // namespace Pelican
