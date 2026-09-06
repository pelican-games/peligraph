#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/renderconfigcandidate.hpp"
#include "../src/core/renderingpass/frameexecutionadapter.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/renderingpassdefinitionjsonparser.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerprogramdetail.hpp"
#include "../src/project/renderconfigdocument.hpp"
#include "../src/project/rasterrayquery.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Pelican {
TEST_CASE("named scene AS definitions validate mask and preserve metadata", "[rendercompiler][rt6][multi-tlas]") {
    ResolvedRenderPipeline resolved;
    resolved.normalized_config["scene_acceleration_structures"] = nlohmann::json::array({
        {{"name", "actors"}, {"instance_mask", 1}}, {{"name", "everything"}},
        {{"name", "empty"}, {"instance_mask", 0}}});
    const auto compiled = compileRenderPipeline(resolved);
    REQUIRE(compiled.scene_acceleration_structures.size() == 3);
    CHECK(compiled.scene_acceleration_structures[0].instance_mask == 1);
    CHECK(compiled.scene_acceleration_structures[1].instance_mask == 255);
    CHECK(compiled.scene_acceleration_structures[2].instance_mask == 0);
    const auto metadata = serializeCompiledRenderPipelineMetadata(compiled);
    CHECK(metadata.at("scene_acceleration_structures")[0].at("name") == "actors");
    for (const auto &entry : nlohmann::json::array({
            {{"name", "scene"}}, {{"name", ""}},
            {{"name", "bad"}, {"instance_mask", -1}},
            {{"name", "bad"}, {"instance_mask", 256}},
            {{"name", "bad"}, {"instance_mask", 1.5}},
            {{"name", "bad"}, {"unknown", 1}}})) {
        resolved.normalized_config["scene_acceleration_structures"] = nlohmann::json::array({entry});
        CHECK_THROWS(compileRenderPipeline(resolved));
    }
    resolved.normalized_config["scene_acceleration_structures"] = nlohmann::json::array({
        {{"name", "same"}}, {{"name", "same"}}});
    CHECK_THROWS(compileRenderPipeline(resolved));
}

TEST_CASE("scene AS operation follows variant-wide ray requirements", "[rendercompiler][rt6][as-node]") {
    std::vector<FrameGraphDefinition> graphs{
        {.name = "main", .nodes = {{.name = "draw", .kind = FramePlanNodeKind::render}}},
        {.name = "aux", .nodes = {{.name = "copy", .kind = FramePlanNodeKind::anchor}}}};
    TargetPlanningPolicy policy;
    std::vector<ComputeTaskDefinition> tasks;
    const auto require_as_nodes = [&] {
        for (const auto &graph : graphs) {
            REQUIRE(graph.nodes.size() == 2);
            CHECK(graph.nodes[0].kind == FramePlanNodeKind::acceleration_structure);
            CHECK(graph.nodes[0].name == "__scene_acceleration_structure");
            CHECK(graph.nodes[0].declaration_index == 0);
            CHECK(graph.nodes[1].declaration_index == 1);
            if (graph.nodes[1].kind == FramePlanNodeKind::anchor)
                CHECK(graph.nodes[1].after.empty());
            else
                CHECK(graph.nodes[1].after == std::vector<std::string>{"__scene_acceleration_structure"});
        }
    };
    SECTION("non-ray variant is unchanged") {
        detail::insertSceneAccelerationStructureNodes(graphs, policy, tasks, false);
        REQUIRE(graphs[0].nodes.size() == 1);
        CHECK(graphs[0].nodes[0].name == "draw");
        CHECK(graphs[1].nodes.size() == 1);
    }
    SECTION("one ray graph creates operations for the whole owner") {
        policy.graphs.push_back({.graph = "main",
            .required_capabilities = {"pelican.vulkan.ray_query@1"}});
        detail::insertSceneAccelerationStructureNodes(graphs, policy, tasks, false);
        require_as_nodes();
    }
    SECTION("ray project creates operations") {
        detail::insertSceneAccelerationStructureNodes(graphs, policy, tasks, true);
        require_as_nodes();
    }
    SECTION("declared raster query creates an AS and independent raster can precede it") {
        graphs[0].nodes[0].ray_query = true;
        graphs[0].nodes.push_back({.name = "independent", .kind = FramePlanNodeKind::render, .ray_query = false});
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, false);
        CHECK(graphs[0].nodes[1].after == std::vector<std::string>{"__scene_acceleration_structure"});
        CHECK(graphs[0].nodes[2].after.empty());
        detail::applyFrameNodeOrder(graphs, {{"main", {"independent", "__scene_acceleration_structure", "draw"}}});
        const auto plan = planFrameGraph(graphs[0]);
        CHECK(plan.nodes[0].name == "independent");
        CHECK(plan.nodes[1].name == "__scene_acceleration_structure");
        CHECK(plan.nodes[2].name == "draw");
        detail::applyFrameNodeOrder(graphs, {{"main", {"draw", "__scene_acceleration_structure"}}});
        CHECK_THROWS_WITH(planFrameGraph(graphs[0]), Catch::Matchers::ContainsSubstring("Cycle"));
    }
    SECTION("ray task creates operations") {
        ComputeTaskDefinition task;
        task.name = "trace";
        task.ray_tracing.emplace();
        tasks.push_back(std::move(task));
        graphs[0].nodes[0].name = "trace";
        graphs[0].nodes[0].kind = FramePlanNodeKind::compute;
        detail::insertSceneAccelerationStructureNodes(graphs, policy, tasks, false);
        require_as_nodes();
    }
    SECTION("fixed sprite shaders can run before AS construction") {
        graphs[0].nodes.push_back({.name = "__anchor_sprite", .kind = FramePlanNodeKind::anchor});
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, true);
        CHECK(graphs[0].nodes.back().after.empty());
        detail::applyFrameNodeOrder(graphs, {{"main", {"__anchor_sprite", "__scene_acceleration_structure", "draw"}}});
        CHECK(planFrameGraph(graphs[0]).nodes[0].name == "__anchor_sprite");
    }
    SECTION("fixed UI shaders can run before AS construction in both graph lowering paths") {
        const auto json_graph = parseFrameGraphDefinitionFromJson({{"name", "main"},
            {"passes", nlohmann::json::array({{{"name", "pelican_ui"}, {"type", "ui"},
                {"output", {{"color", "swapchain"}, {"depth", nullptr}}}}})}});
        RenderingPassDefinition typed;
        typed.name = "main";
        PassDefinition ui;
        ui.name = "pelican_ui";
        ui.pass_type = RenderPassType::ui;
        ui.pass_info = UiPassInfo{};
        ui.output_color.emplace_back(swapchainRenderTargetId());
        ui.applyDefaultAttachmentOperations(RenderPassType::ui);
        typed.passes.push_back(std::move(ui));
        for (const auto &lowered : {json_graph, makeFrameGraphDefinition(typed)}) {
            REQUIRE(lowered.nodes.size() == 1);
            CHECK(lowered.nodes[0].ray_query == false);
            graphs = {lowered};
            detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, true);
            CHECK(graphs[0].nodes.back().after.empty());
            detail::applyFrameNodeOrder(graphs, {{"main", {"pelican_ui", "__scene_acceleration_structure"}}});
            const auto plan = planFrameGraph(graphs[0]);
            REQUIRE(plan.nodes.size() == 2);
            CHECK(plan.nodes[0].name == "pelican_ui");
            CHECK(plan.nodes[1].name == "__scene_acceleration_structure");
        }
    }
    SECTION("reserved name collision is rejected") {
        graphs[0].nodes[0].name = "__scene_acceleration_structure";
        REQUIRE_THROWS_WITH(detail::insertSceneAccelerationStructureNodes(graphs, policy, tasks, true),
            Catch::Matchers::ContainsSubstring("Reserved scene acceleration-structure node name"));
    }
}

TEST_CASE("named AS operations depend on shared construction and precede consumers", "[rendercompiler][rt7][named-as-node]") {
    std::vector<FrameGraphDefinition> graphs{
        {.name = "main", .nodes = {{.name = "trace", .kind = FramePlanNodeKind::compute}}}};
    const std::vector<SceneAccelerationStructureDefinition> definitions{
        {.name = "actors", .instance_mask = 1}, {.name = "world", .instance_mask = 2}};
    SECTION("material declaration controls AS placement and selection") {
        graphs[0] = parseFrameGraphDefinitionFromJson({{"name", "main"}, {"passes", nlohmann::json::array({
            {{"name", "independent"}, {"type", "material"}, {"material_contract", "deferred_geometry_v1"},
             {"output", {{"color", {"a", "b", "c", "d", "e"}}, {"depth", "depth"}}}},
            {{"name", "query"}, {"type", "material"}, {"material_contract", "deferred_geometry_v1"},
             {"ray_query", {{"acceleration_structure", "actors"}}},
             {"output", {{"color", {"f", "g", "h", "i", "j"}}, {"depth", "depth2"}}}}
        })}});
        REQUIRE(graphs[0].nodes.size() == 2);
        CHECK(graphs[0].nodes[0].ray_query == false);
        CHECK(graphs[0].nodes[1].ray_query == true);
        CHECK(graphs[0].nodes[1].ray_query_acceleration_structure == "actors");
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, false, definitions);
        detail::applyFrameNodeOrder(graphs, {{"main", {"independent", "__scene_acceleration_structure", "__scene_acceleration_structure:actors", "query"}}});
        CHECK(planFrameGraph(graphs[0]).nodes[0].name == "independent");
        detail::applyFrameNodeOrder(graphs, {{"main", {"query", "__scene_acceleration_structure:actors"}}});
        CHECK_THROWS_WITH(planFrameGraph(graphs[0]), Catch::Matchers::ContainsSubstring("Cycle"));
    }
    SECTION("auxiliary drawing declares its query dependency in JSON and typed graphs") {
      for (const auto type : {RenderPassType::debug_draw, RenderPassType::debug_text, RenderPassType::gizmo,
                              RenderPassType::shadow_depth, RenderPassType::velocity}) {
        const auto type_name = type == RenderPassType::debug_draw ? "debug_draw" :
                               type == RenderPassType::debug_text ? "debug_text" :
                               type == RenderPassType::gizmo ? "gizmo" :
                               type == RenderPassType::shadow_depth ? "shadow_depth" : "velocity";
        INFO(type_name);
        nlohmann::json output_a = {{"color", "a"}, {"depth", nullptr}};
        nlohmann::json output_b = {{"color", "b"}, {"depth", nullptr}};
        const bool geometry = type == RenderPassType::shadow_depth || type == RenderPassType::velocity;
        if (geometry) {
            output_a["depth"] = "depth_a";
            output_b["depth"] = "depth_b";
        }
        if (type == RenderPassType::shadow_depth) {
            output_a["color"] = nullptr;
            output_b["color"] = nullptr;
        }
        graphs[0] = parseFrameGraphDefinitionFromJson({{"name", "main"}, {"passes", nlohmann::json::array({
            {{"name", "independent"}, {"type", type_name}, {"output", output_a}},
            {{"name", "query"}, {"type", type_name}, {"ray_query", {{"acceleration_structure", "actors"}}},
             {"output", output_b}}
        })}});
        REQUIRE(graphs[0].nodes.size() == 2);
        CHECK(graphs[0].nodes[0].ray_query == false);
        CHECK(graphs[0].nodes[1].ray_query == true);
        CHECK(graphs[0].nodes[1].ray_query_acceleration_structure == "actors");
        RenderingPassDefinition typed;
        typed.name = "main";
        PassDefinition draw;
        draw.name = "independent";
        draw.pass_type = type;
        if (type == RenderPassType::debug_draw) draw.pass_info = DebugDrawPassInfo{};
        else if (type == RenderPassType::debug_text) draw.pass_info = DebugTextPassInfo{};
        else if (type == RenderPassType::gizmo) draw.pass_info = GizmoPassInfo{};
        else if (type == RenderPassType::shadow_depth) draw.pass_info = ShadowDepthPassInfo{};
        else draw.pass_info = VelocityPassInfo{};
        if (type != RenderPassType::shadow_depth) draw.output_color.emplace_back(GlobalRenderTargetId{0});
        if (geometry) draw.output_depth = GlobalRenderTargetId{2};
        draw.applyDefaultAttachmentOperations(type);
        typed.passes.push_back(draw);
        draw.name = "query";
        draw.ray_query = true;
        draw.ray_query_acceleration_structure = "actors";
        if (type != RenderPassType::shadow_depth) draw.output_color = {GlobalRenderTargetId{1}};
        if (geometry) draw.output_depth = GlobalRenderTargetId{3};
        typed.passes.push_back(draw);
        const auto typed_graph = makeFrameGraphDefinition(typed);
        for (const auto &lowered : {graphs[0], typed_graph}) {
            graphs = {lowered};
            CHECK(graphs[0].nodes[0].ray_query == false);
            CHECK(graphs[0].nodes[1].ray_query_acceleration_structure == "actors");
            detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, true, definitions);
            detail::applyFrameNodeOrder(graphs, {{"main", {"independent", "__scene_acceleration_structure", "__scene_acceleration_structure:actors", "query"}}});
            const auto plan = planFrameGraph(graphs[0]);
            CHECK(plan.nodes.front().name == "independent");
            CHECK(plan.nodes.back().name == "query");
            detail::applyFrameNodeOrder(graphs, {{"main", {"query", "__scene_acceleration_structure:actors"}}});
            CHECK_THROWS_WITH(planFrameGraph(graphs[0]), Catch::Matchers::ContainsSubstring("Cycle"));
        }
      }
    }
    SECTION("one executable operation per named AS") {
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, true, definitions);
        REQUIRE(graphs[0].nodes.size() == 4);
        const auto &nodes = graphs[0].nodes;
        CHECK(nodes[0].name == "__scene_acceleration_structure");
        for (std::size_t i = 1; i < 3; ++i) {
            CHECK(nodes[i].name == "__scene_acceleration_structure:" + definitions[i - 1].name);
            CHECK(nodes[i].kind == FramePlanNodeKind::acceleration_structure);
            CHECK(nodes[i].after == std::vector<std::string>{nodes[0].name});
            CHECK(nodes[i].declaration_index == i);
        }
        CHECK(nodes[3].after.empty());
    }
    SECTION("named consumers can execute before an unrelated TLAS") {
        ComputeTaskDefinition task;
        task.name = "trace";
        task.ray_tracing.emplace();
        task.ray_tracing->acceleration_structure = "actors";
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {task}, true, definitions);
        CHECK(graphs[0].nodes.back().after == std::vector<std::string>{"__scene_acceleration_structure", "__scene_acceleration_structure:actors"});
        detail::applyFrameNodeOrder(graphs, {{"main", {"__scene_acceleration_structure:actors", "trace", "__scene_acceleration_structure:world"}}});
        const auto plan = planFrameGraph(graphs[0]);
        REQUIRE(plan.nodes.size() == 4);
        CHECK(plan.nodes[2].name == "trace");
        CHECK(plan.nodes[3].name == "__scene_acceleration_structure:world");
    }
    SECTION("query consumers cannot precede their selected TLAS") {
        ComputeTaskDefinition task;
        task.name = "trace";
        task.ray_query_acceleration_structure = "actors";
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {task}, true, definitions);
        detail::applyFrameNodeOrder(graphs, {{"main", {"trace", "__scene_acceleration_structure:actors"}}});
        CHECK_THROWS_WITH(planFrameGraph(graphs[0]), Catch::Matchers::ContainsSubstring("Cycle"));
    }
    SECTION("generated named operation cannot collide with an authored node") {
        graphs[0].nodes[0].name = "__scene_acceleration_structure:actors";
        CHECK_THROWS_WITH(detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, true, definitions),
            Catch::Matchers::ContainsSubstring("Reserved scene acceleration-structure node name"));
    }
    SECTION("raster queries select and wait for their named TLAS") {
        auto &node = graphs[0].nodes[0];
        node.kind = FramePlanNodeKind::render;
        node.ray_query = true;
        node.ray_query_acceleration_structure = *parseRasterRayQuery({{"ray_query", {{"acceleration_structure", "actors"}}}});
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, false, definitions);
        CHECK(graphs[0].nodes.back().after == std::vector<std::string>{"__scene_acceleration_structure", "__scene_acceleration_structure:actors"});
        detail::applyFrameNodeOrder(graphs, {{"main", {"trace", "__scene_acceleration_structure:actors"}}});
        CHECK_THROWS_WITH(planFrameGraph(graphs[0]), Catch::Matchers::ContainsSubstring("Cycle"));
        CHECK(parseRasterRayQuery({{"ray_query", true}}) == "scene");
        CHECK_FALSE(parseRasterRayQuery({{"ray_query", false}}));
        for (const auto &bad : nlohmann::json::array({42, "actors", nlohmann::json::object(),
                 {{"acceleration_structure", ""}}, {{"acceleration_structure", "actors"}, {"extra", 1}}}))
            CHECK_THROWS(parseRasterRayQuery({{"ray_query", bad}}));
    }
    SECTION("unknown raster TLAS is rejected before physical planning") {
        graphs[0].nodes[0].kind = FramePlanNodeKind::render;
        graphs[0].nodes[0].ray_query = true;
        graphs[0].nodes[0].ray_query_acceleration_structure = "missing";
        CHECK_THROWS_WITH(detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, false, definitions),
            Catch::Matchers::ContainsSubstring("unknown scene acceleration structure"));
    }
}

TEST_CASE("frame node ordering adds constraints without rewriting resource dependencies",
          "[rendercompiler][rt7][node-order]") {
    using Json = nlohmann::json;
    std::vector<FrameGraphDefinition> graphs{{.name = "main", .nodes = {
        {.name = "first", .kind = FramePlanNodeKind::compute, .declaration_index = 0},
        {.name = "second", .kind = FramePlanNodeKind::compute, .declaration_index = 1}}}};
    SECTION("independent nodes execute in the requested order") {
        detail::applyFrameNodeOrder(graphs, {{"main", {"second", "first"}}});
        CHECK(graphs[0].nodes[0].name == "first");
        CHECK(graphs[0].nodes[0].declaration_index == 0);
        const auto plan = planFrameGraph(graphs[0]);
        REQUIRE(plan.nodes.size() == 2);
        CHECK(plan.nodes[0].name == "second");
        CHECK(plan.nodes[1].name == "first");
    }
    SECTION("reversing producer and consumer is rejected") {
        graphs[0].declared_resources = {"result"};
        graphs[0].nodes[0].writes = {"result"};
        graphs[0].nodes[1].reads = {"result"};
        detail::applyFrameNodeOrder(graphs, {{"main", {"second", "first"}}});
        CHECK_THROWS_WITH(planFrameGraph(graphs[0]), Catch::Matchers::ContainsSubstring("Cycle"));
    }
    SECTION("named AS order changes within the shared publication boundary") {
        const std::vector<SceneAccelerationStructureDefinition> definitions{{.name = "left"}, {.name = "right"}};
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {}, true, definitions);
        detail::applyFrameNodeOrder(graphs, {{"main", {"__scene_acceleration_structure:right", "__scene_acceleration_structure:left"}}});
        const auto plan = planFrameGraph(graphs[0]);
        REQUIRE(plan.nodes.size() == 5);
        CHECK(plan.nodes[0].name == "__scene_acceleration_structure");
        CHECK(plan.nodes[1].name == "__scene_acceleration_structure:right");
        CHECK(plan.nodes[2].name == "__scene_acceleration_structure:left");
    }
    SECTION("consumer cannot precede AS publication") {
        ComputeTaskDefinition task;
        task.name = "first";
        task.ray_query_acceleration_structure = "scene";
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {task}, true);
        detail::applyFrameNodeOrder(graphs, {{"main", {"first", "__scene_acceleration_structure"}}});
        CHECK_THROWS_WITH(planFrameGraph(graphs[0]), Catch::Matchers::ContainsSubstring("Cycle"));
    }
    SECTION("independent compute can precede shared AS construction") {
        ComputeTaskDefinition task;
        task.name = "first";
        detail::insertSceneAccelerationStructureNodes(graphs, {}, {task}, true);
        detail::applyFrameNodeOrder(graphs, {{"main", {"first", "__scene_acceleration_structure"}}});
        const auto plan = planFrameGraph(graphs[0]);
        CHECK(plan.nodes[0].name == "first");
        CHECK(plan.nodes[1].name == "__scene_acceleration_structure");
    }
    SECTION("partial and empty orders retain unspecified nodes") {
        detail::applyFrameNodeOrder(graphs, {{"main", Json::array()}});
        detail::applyFrameNodeOrder(graphs, {{"main", {"second"}}});
        CHECK(planFrameGraph(graphs[0]).nodes[0].name == "first");
    }
    SECTION("malformed or stale runtime names are rejected") {
        for (const auto &order : std::vector<Json>{
                 Json::array(), {{"missing", Json::array()}}, {{"main", "first"}},
                 {{"main", {1}}}, {{"main", {""}}}, {{"main", {"missing"}}},
                 {{"main", {"first", "first"}}}}) {
            CHECK_THROWS(detail::applyFrameNodeOrder(graphs, order));
        }
    }
}

namespace {

nlohmann::json readJsonFixture(
    const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error(
            "failed to open JSON fixture: " + path.string());
    }
    return nlohmann::json::parse(file);
}

void publishGenerationProbe(
    FrameGraphRuntimeContainer &runtime) {
    CompiledRenderingPass rendering_pass;
    rendering_pass.name = "wp311_publication_probe";
    FramePlan frame_plan;
    frame_plan.name = rendering_pass.name;
    runtime.registerExecutionPlan(
        RenderingPassId{0}, rendering_pass,
        std::move(frame_plan),
        std::make_shared<CompiledRenderPipeline>());
}

enum class NativeProgramFault {
    none,
    wrong_backend,
    missing_target_index,
    missing_execution_plan,
    data_only_physical_package,
};

class NativeVulkanProgram final
    : public RenderCompilerProgram {
  public:
    explicit NativeVulkanProgram(
        NativeProgramFault fault =
            NativeProgramFault::none)
        : fault_{fault} {}

    mutable std::size_t compile_calls = 0;

    RenderCompilerProgramSelection selection(
        const RenderCompilerBackendContext
            &) const override {
        return {
            .schema_version = 1,
            .name = "test.native",
            .implementation =
                "test.backend_native_v1",
            .backend =
                fault_ ==
                        NativeProgramFault::
                            wrong_backend
                    ? "metal"
                    : "vulkan",
            .mode =
                RenderCompilerProgramMode::
                    backend_native,
        };
    }

    RenderCompilerProgramOutput compile(
        const RenderCompilerProgramInput
            &input) const override {
        ++compile_calls;
        RenderCompilerProgramOutput output;
        for (const auto &request :
             input.variants) {
            auto pipeline =
                std::make_shared<
                    CompiledRenderPipeline>();
            pipeline->graph_variant_policy.variant =
                request.graph_variant;

            RenderCompilerProgramVariantOutput
                variant;
            variant.graph_variant =
                request.graph_variant;
            variant.compiled_pipeline =
                std::move(pipeline);
            variant.normalized_config =
                nlohmann::json::object();
            if (request.artifact ==
                RenderCompilerProgramArtifact::
                    data_only) {
                if (fault_ ==
                    NativeProgramFault::
                        data_only_physical_package) {
                    variant.physical_package =
                        std::make_unique<
                            VulkanRenderCompilerPhysicalPackage>();
                }
                output.variants.push_back(
                    std::move(variant));
                continue;
            }

            auto target_plan =
                std::make_shared<VulkanTargetPlan>();
            target_plan->graph = "native_graph";
            auto physical =
                std::make_unique<
                    VulkanRenderCompilerPhysicalPackage>();
            physical->target_plan_compilation
                .plans.push_back(target_plan);
            if (fault_ !=
                NativeProgramFault::
                    missing_target_index) {
                physical->target_plans.emplace(
                    target_plan->graph,
                    target_plan);
            }

            auto frame_plan = FramePlan{
                .name = "native_graph"};
            if (fault_ !=
                NativeProgramFault::
                    missing_execution_plan) {
                variant.execution_plans.emplace(
                    "native_graph",
                    makeCompatibilityFrameExecutionPlan(
                        frame_plan));
            }
            variant.frame_plans.emplace(
                "native_graph",
                std::move(frame_plan));
            variant.physical_package =
                std::move(physical);
            output.variants.push_back(
                std::move(variant));
        }
        return output;
    }

  private:
    NativeProgramFault fault_;
};

struct ProgramInputFixture {
    nlohmann::json config =
        nlohmann::json::object();
    PathResolver path_resolver;
    GraphTransformRegistrySnapshot
        graph_transforms;
    RenderStrategyRegistrySnapshot
        render_strategies;
    SubgraphReplacementRegistrySnapshot
        subgraph_replacements;
    VulkanRenderCompilerBackendContext
        backend_context;
    std::vector<RenderCompilerProgramVariantRequest>
        variants{
            RenderCompilerProgramVariantRequest{
                .graph_variant =
                    RenderPipelineGraphVariant::flat,
            },
            RenderCompilerProgramVariantRequest{
                .graph_variant =
                    RenderPipelineGraphVariant::xr,
            },
            RenderCompilerProgramVariantRequest{
                .graph_variant =
                    RenderPipelineGraphVariant::
                        preview,
                .artifact =
                    RenderCompilerProgramArtifact::
                        data_only,
            },
        };

    RenderCompilerProgramInput input() const {
        return {
            .rendering_config = config,
            .source_name = "native program test",
            .path_resolver = path_resolver,
            .runtime_shader_compiler_enabled =
                false,
            .graph_transforms =
                graph_transforms,
            .render_strategies =
                render_strategies,
            .subgraph_replacements =
                subgraph_replacements,
            .backend_context =
                backend_context,
            .variants = variants,
        };
    }
};

TEST_CASE(
    "backend-native compiler programs converge at the "
    "verified Vulkan package") {
    ProgramInputFixture fixture;
    NativeVulkanProgram program;

    auto output = runRenderCompilerProgram(
        program, fixture.input());

    REQUIRE(program.compile_calls == 1);
    REQUIRE(output.variants.size() == 3);
    const auto &variant = output.variants.front();
    REQUIRE(
        variant.compiled_pipeline
            ->render_compiler_program
            .has_value());
    CHECK(
        variant.compiled_pipeline
            ->render_compiler_program->name ==
        "test.native");
    CHECK(
        variant.compiled_pipeline
            ->render_compiler_program->mode ==
        RenderCompilerProgramMode::
            backend_native);
    const auto &physical =
        requireVulkanRenderCompilerPhysicalPackage(
            *variant.physical_package);
    CHECK(
        physical.target_plans.contains(
            "native_graph"));
    const auto metadata =
        serializeCompiledRenderPipelineMetadata(
            *variant.compiled_pipeline);
    REQUIRE(
        metadata.contains(
            "render_compiler_program"));
    CHECK(
        metadata["render_compiler_program"]
                ["mode"] ==
        "backend_native");
    CHECK(
        output.variants.at(1)
            .compiled_pipeline
            ->graph_variant_policy.variant ==
        RenderPipelineGraphVariant::xr);
    const auto &preview =
        output.variants.back();
    CHECK(
        preview.compiled_pipeline
            ->graph_variant_policy.variant ==
        RenderPipelineGraphVariant::preview);
    CHECK(
        preview.physical_package == nullptr);
    REQUIRE(
        preview.compiled_pipeline
            ->render_compiler_program
            .has_value());
    CHECK(
        preview.compiled_pipeline
            ->render_compiler_program->name ==
        "test.native");
}

TEST_CASE(
    "WP311 data-only production compiler rejects non-material GPU draw ownership before publication",
    "[wp311][render-compiler][data-only][pass-field-ownership]") {
    ProgramInputFixture fixture;
    fixture.config = {
        {"render_targets", nlohmann::json::array()},
        {"rendering_passes",
         nlohmann::json::array({
             {
                 {"name", "preview_graph"},
                 {"passes",
                  nlohmann::json::array({
                      {
                          {"name", "preview_fullscreen"},
                          {"type", "fullscreen"},
                          {"gpu_draw_source",
                           {{"commands", "visible_draws"},
                            {"count", "visible_draw_count"},
                            {"max_draw_count", 2}}},
                          {"output",
                           {{"color", "swapchain"},
                            {"depth", nullptr}}},
                      },
                  })},
             },
         })},
    };
    fixture.backend_context =
        VulkanRenderCompilerBackendContext{
            vk::Format::eB8G8R8A8Srgb,
            vk::Extent2D{64, 64}, {}, {}, {},
            VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {
        RenderCompilerProgramVariantRequest{
            .graph_variant =
                RenderPipelineGraphVariant::preview,
            .artifact =
                RenderCompilerProgramArtifact::data_only,
        },
    };
    FrameGraphRuntimeContainer runtime;
    FrameGraphRuntimeContainer publication_control;
    publishGenerationProbe(publication_control);
    REQUIRE(publication_control.activeGeneration() == 1);

    const auto compile_then_publish = [&] {
        (void)runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(),
            fixture.input());
        publishGenerationProbe(runtime);
    };

    REQUIRE_THROWS_WITH(
        compile_then_publish(),
        Catch::Matchers::ContainsSubstring(
            "Pass 'preview_fullscreen' type 'fullscreen' does not own "
            "field 'gpu_draw_source'"));
    CHECK(runtime.activeGeneration() == 0);
}

TEST_CASE("Frame node orders are scoped to their compiled graph variants",
          "[rendercompiler][node-order][variants]") {
    ProgramInputFixture fixture;
    fixture.config = readJsonFixture(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test/production_fixtures/pass_field_ownership/gpu_draw_material.json");
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    fixture.config["frame_node_order"] = {{"main", {"build_visible_draws", "gpu_geometry"}}};
    SECTION("flat and XR accept independent graph orders") {
#if PELICAN_WITH_OPENXR
        fixture.variants.push_back({.graph_variant = RenderPipelineGraphVariant::xr});
        fixture.config["frame_node_order"]["main#xr"] = {"build_visible_draws#xr", "gpu_geometry"};
#endif
        const auto output = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input());
        REQUIRE(output.variants.front().frame_plans.contains("main"));
#if PELICAN_WITH_OPENXR
        REQUIRE(output.variants.back().frame_plans.contains("main#xr"));
#endif
    }
    SECTION("unknown graph is still rejected across the program") {
        fixture.config["frame_node_order"]["unknown"] = nlohmann::json::array();
        REQUIRE_THROWS_WITH(runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input()),
                            Catch::Matchers::ContainsSubstring("frame_node_order unknown graph: unknown"));
    }
    SECTION("data-only compilation still rejects duplicate names") {
        fixture.variants.front().artifact = RenderCompilerProgramArtifact::data_only;
        fixture.config["frame_node_order"]["main"] = {"gpu_geometry", "gpu_geometry"};
        REQUIRE_THROWS_WITH(runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input()),
                            Catch::Matchers::ContainsSubstring("frame_node_order duplicate node"));
    }
}

TEST_CASE(
    "WP311 production compiler retains GPU draw reads and pins final buffer IDs",
    "[wp311][render-compiler][gpu-draw][pass-field-ownership]") {
    ProgramInputFixture fixture;
    fixture.config = readJsonFixture(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test/production_fixtures/pass_field_ownership/gpu_draw_material.json");
    fixture.backend_context =
        VulkanRenderCompilerBackendContext{
            vk::Format::eB8G8R8A8Srgb,
            vk::Extent2D{64, 64}, {}, {}, {},
            VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {
        RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::flat,
        },
    };

    auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(), fixture.input());
    REQUIRE(output.variants.size() == 1);
    auto &variant = output.variants.front();
    REQUIRE(variant.frame_plans.contains("main"));
    const auto &nodes = variant.frame_plans.at("main").nodes;
    const auto node = std::find_if(
        nodes.begin(), nodes.end(), [](const auto &candidate) {
            return candidate.name == "gpu_geometry";
        });
    REQUIRE(node != nodes.end());
    CHECK(node->reads == std::vector<std::string>{
                             "visible_draws",
                             "visible_draw_count"});

    CompiledFrameGraphExecution execution;
    std::unordered_map<FrameGraphBufferId, std::size_t,
                       FrameGraphBufferId::Hash>
        definition_indices;
    for (std::size_t index = 0;
         index < variant.buffer_definitions.size(); ++index) {
        const auto id =
            FrameGraphBufferId{static_cast<int>(index + 10)};
        execution.buffer_bindings.emplace(
            variant.buffer_definitions[index].name, id);
        definition_indices.emplace(id, index);
    }

    const RenderTargetNameResolver target_names{
        [](const std::string &name) {
            if (name == "swapchain") {
                return swapchainRenderTargetId();
            }
            if (name == "lit_color") {
                return GlobalRenderTargetId{0};
            }
            if (name == "scene_depth") {
                return GlobalRenderTargetId{1};
            }
            return name == "display" ? GlobalRenderTargetId{2}
                                     : noRenderTargetId();
        }};
    const RenderTargetMetadataResolver target_metadata{
        [](GlobalRenderTargetId id) -> RenderTargetMetadata {
            if (id == GlobalRenderTargetId{0}) {
                return {
                    "lit_color",
                    vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR16G16B16A16Sfloat,
                    vk::Extent2D{64, 64},
                };
            }
            if (id == GlobalRenderTargetId{1}) {
                return {
                    "scene_depth",
                    vk::ImageUsageFlagBits::eDepthStencilAttachment,
                    vk::Format::eD32Sfloat,
                    vk::Extent2D{64, 64},
                };
            }
            if (id == GlobalRenderTargetId{2}) {
                return {
                    "display",
                    vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eB8G8R8A8Srgb,
                    vk::Extent2D{64, 64},
                };
            }
            throw std::runtime_error(
                "WP311 fixture target is unknown");
        }};
    auto rendering_pass = parseRenderingPassDefinitionFromJson(
        variant.normalized_config.at("rendering_passes").at(0),
        target_names, target_metadata, variant.buffer_names);
    const auto material = std::find_if(
        rendering_pass.passes.begin(),
        rendering_pass.passes.end(),
        [](const auto &pass) {
            return pass.name == "gpu_geometry";
        });
    REQUIRE(material != rendering_pass.passes.end());
    auto &material_pass = *material;
    pinGpuDrawSourceBufferBindings(
        material_pass,
        [&](std::string_view name) {
            const auto found = execution.buffer_bindings.find(
                std::string{name});
            return found == execution.buffer_bindings.end()
                       ? noFrameGraphBufferId()
                       : found->second;
        },
        [&](FrameGraphBufferId id)
            -> const FrameGraphBufferDefinition & {
            return variant.buffer_definitions.at(
                definition_indices.at(id));
        });
    REQUIRE(material_pass.materialInfo().gpu_draw_source);
    const auto &source =
        *material_pass.materialInfo().gpu_draw_source;
    CHECK(source.commands_id ==
          execution.buffer_bindings.at(source.commands));
    CHECK(source.count_id ==
          execution.buffer_bindings.at(source.count));
}

TEST_CASE(
    "WP334b production compiler resolves a staged preset and feature through one candidate loader",
    "[wp334b][render-compiler][overlay][preset][feature]") {
    ProgramInputFixture fixture;
    const auto project_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
        "animgraph_demo";
    fixture.path_resolver.setup(project_root, false);

    const std::string root_reference = "passes/main.json";
    const std::string preset_reference =
        "project://passes/wp334b-staged-preset.json";
    const std::string feature_reference =
        "project://passes/wp334b-staged-feature.json";
    const std::string root_bytes =
        nlohmann::json{
            {"pipeline", {{"preset", preset_reference}}},
            {"features", nlohmann::json::array({feature_reference})},
        }
            .dump();
    const auto preset_bytes = fixture.path_resolver.loadText(
        "engine://render_pipelines/hybrid_v1.json");
    const auto feature_bytes =
        nlohmann::json{
            {"schema", "pelican.render_feature"},
            {"version", 1},
            {"name", "wp334b_staged_compiler_feature"},
            {"runtime_shader_compiler", "optional"},
            {"passes", nlohmann::json::array()},
        }
            .dump();
    const auto root_key =
        fixture.path_resolver.normalizedReference(root_reference);
    const RenderConfigCandidateDocumentSet documents{
        root_key,
        {{.reference = root_reference,
          .normalized_reference = root_key,
          .path = project_root / "passes" / "main.json",
          .operation = RenderConfigDocumentOperation::replace,
          .expected =
              {.existence = RenderConfigDocumentExistence::present,
               .digest = renderConfigSourceDigest(
                   fixture.path_resolver.loadText(root_reference))},
          .bytes = root_bytes},
         {.reference = preset_reference,
          .normalized_reference =
              fixture.path_resolver.normalizedReference(preset_reference),
          .path = project_root / "passes" /
                  "wp334b-staged-preset.json",
          .operation = RenderConfigDocumentOperation::create,
          .expected =
              {.existence = RenderConfigDocumentExistence::missing},
          .bytes = preset_bytes},
         {.reference = feature_reference,
          .normalized_reference =
              fixture.path_resolver.normalizedReference(feature_reference),
          .path = project_root / "passes" /
                  "wp334b-staged-feature.json",
          .operation = RenderConfigDocumentOperation::create,
          .expected =
              {.existence = RenderConfigDocumentExistence::missing},
          .bytes = feature_bytes}}};
    REQUIRE_THROWS(fixture.path_resolver.loadText(preset_reference));
    REQUIRE_THROWS(fixture.path_resolver.loadText(feature_reference));

    fixture.config = nlohmann::json::parse(root_bytes);
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {
        RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::preview,
            .artifact = RenderCompilerProgramArtifact::data_only,
        },
    };
    auto input = fixture.input();
    input.load_document =
        [&](std::string_view reference) {
            return loadRenderConfigCandidateText(
                documents, fixture.path_resolver, reference);
        };

    const auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(), input);
    REQUIRE(output.variants.size() == 1);
    REQUIRE(output.variants.front().compiled_pipeline != nullptr);
    REQUIRE(std::ranges::find(
                output.variants.front().compiled_pipeline->feature_names,
                "wp334b_staged_compiler_feature") !=
            output.variants.front().compiled_pipeline->feature_names.end());
}

TEST_CASE(
    "compiler program validation rejects backend and "
    "graph-set mismatches") {
    ProgramInputFixture fixture;

    SECTION("selection backend") {
        NativeVulkanProgram program{
            NativeProgramFault::wrong_backend};
        CHECK_THROWS_WITH(
            runRenderCompilerProgram(
                program, fixture.input()),
            Catch::Matchers::ContainsSubstring(
                "does not match runtime backend"));
    }

    SECTION("physical graph index") {
        NativeVulkanProgram program{
            NativeProgramFault::
                missing_target_index};
        CHECK_THROWS_WITH(
            runRenderCompilerProgram(
                program, fixture.input()),
            Catch::Matchers::ContainsSubstring(
                "target-plan index is incomplete"));
    }

    SECTION("execution graph index") {
        NativeVulkanProgram program{
            NativeProgramFault::
                missing_execution_plan};
        CHECK_THROWS_WITH(
            runRenderCompilerProgram(
                program, fixture.input()),
            Catch::Matchers::ContainsSubstring(
                "execution-plan coverage does not match"));
    }

    SECTION("duplicate graph variant") {
        fixture.variants.push_back(
            fixture.variants.front());
        NativeVulkanProgram program;
        CHECK_THROWS_WITH(
            runRenderCompilerProgram(
                program, fixture.input()),
            Catch::Matchers::ContainsSubstring(
                "unknown or duplicate graph variant"));
        CHECK(program.compile_calls == 0);
    }

    SECTION("data-only physical package") {
        NativeVulkanProgram program{
            NativeProgramFault::
                data_only_physical_package};
        CHECK_THROWS_WITH(
            runRenderCompilerProgram(
                program, fixture.input()),
            Catch::Matchers::ContainsSubstring(
                "data-only artifact returned a "
                "physical package"));
    }
}

} // namespace
} // namespace Pelican
