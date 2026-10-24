// RC-3c1b: the production compiler program (the one RegistrationStages::prepare
// runs, with the variant requests the registration makes: ImGui composed into
// flat, the request-local preview last) reads each completed revision once
// (parseRenderGraph), hands the provider chain's IR on, finalizes the last
// revision once, and refuses by name where an input is first refused.
#include "../src/core/imgui/imguiruntime.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/registrationownerhost.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
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

struct Providers {
    GraphTransformRegistry transforms;
    SubgraphReplacementRegistry subgraphs;
    RenderStrategyRegistry strategies;
    TransformState first{.index = 1};
    TransformState second{.index = 2};
    SubgraphState subgraph;
    internal::RegistrationOwner owner = internal::allocateRegistrationOwner();

    Providers() {
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

enum class Variants { flat, flat_and_preview, flat_xr_and_preview, preview };

std::vector<RenderCompilerProgramVariantRequest> requestsOf(Variants variants) {
    std::vector<RenderCompilerProgramVariantRequest> requests;
    if (variants != Variants::preview) {
        RenderCompilerProgramVariantRequest flat{.graph_variant = RenderPipelineGraphVariant::flat};
#if PELICAN_WITH_IMGUI
        // As makeRenderGraphVariantRequests composes it.
        flat.compose_runtime_config = [](nlohmann::json &config) { appendImGuiPassToCanonicalGraphs(config); };
#endif
        requests.push_back(std::move(flat));
    }
#if PELICAN_WITH_OPENXR
    if (variants == Variants::flat_xr_and_preview)
        requests.push_back({.graph_variant = RenderPipelineGraphVariant::xr});
#endif
    if (variants != Variants::flat) {
        requests.push_back({.graph_variant = RenderPipelineGraphVariant::preview,
                            .artifact = RenderCompilerProgramArtifact::data_only});
    }
    return requests;
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
    }
    return 0;
}

RenderCompilerProgramOutput compile(Project &project, Providers &providers, const Json &rendering_section,
                                    Variants variants) {
    const auto root = project.rootOf(rendering_section);
    const auto lowering = lowerAuthoringDocument(root);
    const auto requests = requestsOf(variants);
    const auto transforms = providers.transforms.snapshot();
    const auto strategies = providers.strategies.snapshot();
    const auto subgraphs = providers.subgraphs.snapshot();
    const VulkanRenderCompilerBackendContext backend{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    return runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(),
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

TEST_CASE("RC-3c1b B1 the production compiler reads each completed revision once",
          "[rc3c1b][render-compiler][parse-count]") {
    // 1 + T + S per runtime variant (revision 0, each transform's response,
    // each spliced replacement), 1 for the preview; the number moves with the
    // providers as the formula says.
    for (const auto variants : {Variants::flat, Variants::flat_and_preview, Variants::flat_xr_and_preview}) {
        for (const int transforms : {0, 1, 2}) {
            for (const bool subgraph : {false, true}) {
                DYNAMIC_SECTION("variants " << static_cast<int>(variants) << " transforms " << transforms
                                            << " subgraph " << subgraph) {
                    Project project;
                    Providers providers;
                    const auto before = renderGraphParseCount();
                    const auto output = compile(project, providers, section(transforms, subgraph), variants);
                    const auto parses = renderGraphParseCount() - before;
                    const auto preview = variants == Variants::flat ? 0u : 1u;
                    const auto expected = runtimeVariantsOf(variants) *
                                              static_cast<std::size_t>(1 + transforms + (subgraph ? 1 : 0)) +
                                          preview;
                    CHECK(parses == expected);
                    for (const auto &variant : output.variants) CHECK_FALSE(variant.unavailable);
                }
            }
        }
    }
    SECTION("the preview alone (data_only) reads its composition once") {
        Project project;
        Providers providers;
        const auto before = renderGraphParseCount();
        const auto output = compile(project, providers, section(2, true), Variants::preview);
        CHECK(renderGraphParseCount() - before == 1);
        REQUIRE(output.variants.size() == 1);
        REQUIRE(output.variants.front().render_graph != nullptr);
        CHECK(output.variants.front().render_graph->graphs.at(0).name == "main");
    }
}

TEST_CASE("RC-3c1b B1 each provider receives the revision the previous response made",
          "[rc3c1b][render-compiler][provider-chain]") {
    // Two transforms that change the graph and a replacement: the second
    // transform's inputs are the first's response read once (its normalized
    // config, its logical graphs and their fingerprint), and the replacement
    // reads the region as the second transform left it.
    Project project;
    Providers providers;
    const auto output = compile(project, providers, section(2, true), Variants::flat);
    const auto after_first = revisionOf(providers.first.response);
    CHECK(providers.second.received_config == after_first.config);
    CHECK(providers.second.received_graphs == after_first.graphs);
    CHECK(providers.second.received_fingerprint == after_first.fingerprint);
    // Not revision 0: the first transform's pass is in what the second read.
    CHECK(providers.second.received_graphs.find("t1_pass") != std::string::npos);
    CHECK(providers.first.received_graphs.find("t1_pass") == std::string::npos);
    // The replacement reads tone as the second transform left it.
    const auto after_second = revisionOf(providers.second.response);
    const auto authored = Json::parse(providers.subgraph.received_authored);
    CHECK(authored.at(0).at("input") == Json::array({"t2_scratch"}));
    const auto logical = TestSupport::logicalGraphsOf(after_second.ir);
    CHECK(providers.subgraph.received_fingerprint ==
          makeTaggedRegionContract(logical.at(0), "rc3c1b.tone").fingerprint);
    // The final revision: the replacement's passes, the provenance of all
    // three on the graph (the provider read none).
    const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
    REQUIRE(flat.render_graph != nullptr);
    const auto &graph = flat.render_graph->graphs.at(0);
    CHECK(graph.graph_transforms.size() == 2);
    CHECK(graph.subgraph_replacements.size() == 1);
    CHECK(graph.render_strategy.has_value());
    CHECK(providers.second.received_graphs.find("graph_transforms\":[{") == std::string::npos);
    const auto order = framePlanOrder(flat.frame_plans.at("main"));
    for (const auto *name : {"t1_pass", "t2_pass", "tone_a", "tone_b"}) {
        INFO(name);
        CHECK(std::find(order.begin(), order.end(), name) != order.end());
    }
}

TEST_CASE("RC-3c1b B2 frame_node_order names the node a provider added",
          "[rc3c1b][render-compiler][frame-node-order]") {
    // The provider puts x at the head of main; the authored side is
    // independent of it. Two orders, both agreeing with the dependencies, give
    // two plans (side first is not the declaration order).
    auto authored = section(1, false);
    authored["render_targets"].push_back(image("side_out"));
    authored["rendering_passes"][0]["passes"].push_back(fullscreen("side", {}, "side_out"));
    const auto order_of = [&](const std::vector<std::string> &order, bool with_provider) {
        auto rendering = authored;
        rendering["frame_node_order"] = {{"main", order}};
        if (!with_provider) rendering.erase("graph_transforms");
        Project project;
        Providers providers;
        providers.first.behavior = TransformBehavior::independent;
        const auto output = compile(project, providers, rendering, Variants::flat);
        return framePlanOrder(variantOf(output, RenderPipelineGraphVariant::flat).frame_plans.at("main"));
    };
    const auto position = [](const std::vector<std::string> &order, std::string_view name) {
        return std::find(order.begin(), order.end(), name) - order.begin();
    };
    const auto side_first = order_of({"side", "x"}, true);
    const auto x_first = order_of({"x", "side"}, true);
    CHECK(position(side_first, "side") < position(side_first, "x"));
    CHECK(position(x_first, "x") < position(x_first, "side"));
    CHECK(side_first != x_first);
    // Control: without the provider there is no x, and the order is refused
    // by name.
    TestSupport::requireNamedRefusal([&] { (void)order_of({"side", "x"}, false); },
                                     "render_graph.frame_node_order_unknown@1", {{"graph", "main"}, {"node", "x"}});
}

TEST_CASE("RC-3c1b B4 the production compiler refuses by name where an input is first refused",
          "[rc3c1b][render-compiler][refusal]") {
    const auto refused = [](const Json &rendering, std::string_view code, const Json &subject,
                            TransformBehavior behavior = TransformBehavior::chain) {
        Project project;
        Providers providers;
        providers.first.behavior = behavior;
        TestSupport::requireNamedRefusal(
            [&] { (void)compile(project, providers, rendering, Variants::flat_and_preview); }, code, subject);
    };
    auto &&base = section(0, false);
    SECTION("a cycle a provider's response makes") {
        refused(section(1, false), "render_graph.cycle@1", {{"graph", "main"}}, TransformBehavior::cycle);
    }
    SECTION("frame_node_order naming a graph no variant has") {
        auto rendering = base;
        rendering["frame_node_order"] = {{"ghost", Json::array()}};
        refused(rendering, "render_graph.frame_node_order_unknown@1", {{"graph", "ghost"}});
    }
    SECTION("frame_node_order naming a node the graph has not") {
        auto rendering = base;
        rendering["frame_node_order"] = {{"main", Json::array({"draw", "ghost"})}};
        refused(rendering, "render_graph.frame_node_order_unknown@1", {{"graph", "main"}, {"node", "ghost"}});
    }
    SECTION("a pass input nothing writes before it") {
        auto rendering = base;
        rendering["render_targets"].push_back(image("never"));
        rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "never"});
        refused(rendering, "render_graph.input_not_produced@1", {{"node", "present"}, {"resource", "never"}});
    }
    SECTION("a read of an undeclared resource") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "ghost"});
        refused(rendering, "render_graph.unknown_resource@1", {{"node", "present"}, {"resource", "ghost"}});
    }
    SECTION("a history read of an image without history") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "scene@history"});
        refused(rendering, "render_graph.unknown_history_resource@1", {{"node", "present"}, {"resource", "scene"}});
    }
    SECTION("after naming no node") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["after"] = Json::array({"tone", "nowhere"});
        refused(rendering, "render_graph.order_reference_unknown@1", {{"node", "present"}, {"reference", "nowhere"}});
    }
    SECTION("a node after itself") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][2]["after"] = Json::array({"present"});
        refused(rendering, "render_graph.self_dependency@1", {{"node", "present"}});
    }
    SECTION("an authored cycle") {
        auto rendering = base;
        rendering["rendering_passes"][0]["passes"][0]["after"] = Json::array({"present"});
        refused(rendering, "render_graph.cycle@1", {{"graph", "main"}});
    }
    SECTION("two unordered writers of one buffer") {
        auto rendering = base;
        rendering["buffers"] = Json::array({Json{{"name", "shared"}}});
        const auto writer = [](const std::string &name) {
            return Json{{"name", name},
                        {"shader", "engine://shaders/compute/fixture"},
                        {"writes", Json::array({"shared"})},
                        {"dispatch", {{"groups", Json::array({1, 1, 1})}}}};
        };
        rendering["compute_tasks"] = Json::array({writer("write_a"), writer("write_b")});
        refused(rendering, "render_graph.ambiguous_writes@1", {{"resource", "shared"}});
    }
    SECTION("a provider's response with two passes of one name") {
        // An authored duplicate is refused by the composition first (its own
        // sentence); a provider's response reaches the IR.
        refused(section(1, false), "render_graph.duplicate_node@1", {{"node", "draw"}},
                TransformBehavior::duplicate);
    }
    SECTION("an acceleration structure nobody declares") {
        auto rendering = base;
        rendering["buffers"] = Json::array({Json{{"name", "hits"}}});
        rendering["compute_tasks"] = Json::array(
            {Json{{"name", "trace"},
                  {"shader", "engine://shaders/compute/fixture"},
                  {"writes", Json::array({"hits"})},
                  {"dispatch", {{"groups", Json::array({1, 1, 1})}}},
                  {"ray_query", {{"acceleration_structure", "missing"}}}}});
        refused(rendering, "render_graph.acceleration_structure_unknown@1",
                {{"node", "trace"}, {"acceleration_structure", "missing"}});
    }
}

TEST_CASE("RC-3c1b ray_task_acceleration_structures may name a task a provider adds",
          "[rc3c1b][render-compiler][finalize]") {
    // The table is a protected control field: a transform cannot change it,
    // but may add the task it names. Each revision holds its form; the final
    // revision resolves it.
    auto rendering = section(1, false);
    rendering["ray_task_acceleration_structures"] = {{"x_trace", "scene"}};
    const auto run = [&](bool with_provider) {
        auto authored = rendering;
        if (!with_provider) authored.erase("graph_transforms");
        Project project;
        Providers providers;
        providers.first.behavior = TransformBehavior::ray_task;
        (void)compile(project, providers, authored, Variants::flat);
    };
    // With the provider the table resolves; compiled without a device the
    // compile then stops at the physical plan (a query needs the device's
    // ray query), after the finalize and the frame plans.
    try {
        run(true);
        FAIL("a ray query compiled without a device's ray query");
    } catch (const GraphRegistryError &error) {
        FAIL("refused by name: " << error.what());
    } catch (const std::exception &error) {
        INFO(error.what());
        CHECK(std::string_view{error.what()}.find("pelican.vulkan.ray_query@1") != std::string_view::npos);
    }
    // Control: without the provider no task has the name, refused in the
    // finalize by name.
    TestSupport::requireNamedRefusal([&] { run(false); }, "render_graph.compute_task_invalid@1",
                                     {{"node", "x_trace"}, {"field", "ray_task_acceleration_structures"},
                                      {"reason", "unknown_task"}});
}

TEST_CASE("RC-3c1b the preview's own refusal stays the preview's", "[rc3c1b][render-compiler][preview]") {
    // A history image the runtime draws and the preview cannot take: the
    // runtime variant compiles, the preview is unavailable (its IR is parsed
    // inside its own try), and the authored frame_node_order's graph names
    // are checked across the runtime variants.
    auto rendering = section(0, false);
    auto accum = image("accum");
    accum["history"] = true;
    rendering["render_targets"].push_back(accum);
    rendering["rendering_passes"][0]["passes"][1]["input"] = Json::array({"scene", "accum@history"});
    rendering["rendering_passes"][0]["passes"].insert(
        rendering["rendering_passes"][0]["passes"].begin() + 1, fullscreen("keep", {"scene"}, "accum", {"draw"}));
    rendering["frame_node_order"] = {{"main", Json::array({"draw", "tone"})}};
    Project project;
    Providers providers;
    const auto output = compile(project, providers, rendering, Variants::flat_and_preview);
    const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
    CHECK_FALSE(flat.unavailable);
    CHECK(flat.frame_plans.contains("main"));
    const auto &preview = variantOf(output, RenderPipelineGraphVariant::preview);
    REQUIRE(preview.unavailable);
    CHECK(preview.render_graph == nullptr);
}

TEST_CASE("RC-3c1b a format class without a format reaches the preview's IR unresolved",
          "[rc3c1b][render-compiler][preview][format-class]") {
    // resolver_version 2: the runtime package resolves each class to a
    // format; the preview composes without resolving it. Its IR takes the
    // image with its format unresolved, so the preview stays available (the
    // planning parse it replaced never read a format).
    auto rendering = section(0, false);
    rendering["resolver_version"] = 2;
    for (auto &target : rendering["render_targets"]) {
        target.erase("format");
        target["format_class"] = "scene";
    }
    Project project;
    Providers providers;
    const auto output = compile(project, providers, rendering, Variants::flat_and_preview);
    const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
    CHECK_FALSE(flat.unavailable);
    // The runtime's images have their resolved formats.
    for (const auto &image : flat.render_graph->images) CHECK_FALSE(image.format_unresolved);
    const auto &preview = variantOf(output, RenderPipelineGraphVariant::preview);
    CHECK_FALSE(preview.unavailable);
    REQUIRE(preview.render_graph != nullptr);
    CHECK(std::any_of(preview.render_graph->images.begin(), preview.render_graph->images.end(),
                      [](const ImageResource &image) { return image.format_unresolved; }));
}

TEST_CASE("RC-3c1b a compute task's persistent image is not a pass input; a pass input is",
          "[rc3c1b][render-compiler][input-not-produced]") {
    // The same compile: a compute task reads and writes a persistent image
    // (its last contents) and is accepted; a pass reading an image nothing
    // writes before it is refused by name.
    auto rendering = section(0, false);
    auto state = image("state", Json::array({"STORAGE", "SAMPLED"}));
    state["lifetime"] = "persistent";
    rendering["render_targets"].push_back(state);
    rendering["compute_tasks"] = Json::array(
        {Json{{"name", "evolve"},
              {"shader", "engine://shaders/compute/fixture"},
              {"reads", Json::array({"state"})},
              {"writes", Json::array({"state"})},
              {"dispatch", {{"groups", Json::array({1, 1, 1})}}}}});
    {
        Project project;
        Providers providers;
        const auto output = compile(project, providers, rendering, Variants::flat);
        CHECK(variantOf(output, RenderPipelineGraphVariant::flat).frame_plans.contains("main"));
    }
    rendering["render_targets"].push_back(image("never"));
    rendering["rendering_passes"][0]["passes"][2]["input"] = Json::array({"post", "never"});
    Project project;
    Providers providers;
    TestSupport::requireNamedRefusal(
        [&] { (void)compile(project, providers, rendering, Variants::flat); },
        "render_graph.input_not_produced@1", {{"node", "present"}, {"resource", "never"}});
}

} // namespace Pelican
