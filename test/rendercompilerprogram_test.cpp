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
#include "../src/core/renderer/viewfamilyproviderregistry.hpp"
#include "../src/project/renderconfigdocument.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/project/rasterrayquery.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

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
      for (const auto type : {RenderPassType::ui
#if PELICAN_WITH_IMGUI
                             , RenderPassType::imgui
#endif
                             }) {
        const auto type_name = type == RenderPassType::ui ? "ui" : "imgui";
        INFO(type_name);
        const auto json_graph = parseFrameGraphDefinitionFromJson({{"name", "main"},
            {"passes", nlohmann::json::array({{{"name", "pelican_ui"}, {"type", type_name},
                {"output", {{"color", "swapchain"}, {"depth", nullptr}}}}})}});
        RenderingPassDefinition typed;
        typed.name = "main";
        PassDefinition ui;
        ui.name = "pelican_ui";
        ui.pass_type = type;
        ui.pass_info = UiPassInfo{};
#if PELICAN_WITH_IMGUI
        if (type == RenderPassType::imgui) ui.pass_info = ImGuiPassInfo{};
#endif
        ui.output_color.emplace_back(swapchainRenderTargetId());
        ui.applyDefaultAttachmentOperations(type);
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
                              RenderPassType::shadow_depth, RenderPassType::velocity, RenderPassType::picking}) {
        const auto type_name = type == RenderPassType::debug_draw ? "debug_draw" :
                               type == RenderPassType::debug_text ? "debug_text" :
                               type == RenderPassType::gizmo ? "gizmo" :
                               type == RenderPassType::shadow_depth ? "shadow_depth" :
                               type == RenderPassType::velocity ? "velocity" : "picking";
        INFO(type_name);
        nlohmann::json output_a = {{"color", "a"}, {"depth", nullptr}};
        nlohmann::json output_b = {{"color", "b"}, {"depth", nullptr}};
        const bool geometry = type == RenderPassType::shadow_depth || type == RenderPassType::velocity ||
                              type == RenderPassType::picking;
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
        else if (type == RenderPassType::velocity) draw.pass_info = VelocityPassInfo{};
        else draw.pass_info = PickingPassInfo{};
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
    std::filesystem::path task_project;

    // MG-2b: the authored config is a `pelican.graph` root. A fixture that
    // declares buffers or compute tasks keeps them in a project task feature,
    // so this writes one into a temporary project the resolver reads from.
    void useAuthoredConfig(nlohmann::json flat, std::string_view label) {
        task_project = std::filesystem::temp_directory_path() /
                       ("pelican-mg2b-" + std::string{label} + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count()));
        std::filesystem::create_directories(task_project);
        path_resolver.setup(task_project, false);
        config = Pelican::TestSupport::renderGraphRootWithTaskFeature(std::move(flat), task_project);
    }

    ProgramInputFixture() = default;
    ProgramInputFixture(const ProgramInputFixture &) = delete;
    ProgramInputFixture &operator=(const ProgramInputFixture &) = delete;
    ~ProgramInputFixture() {
        if (task_project.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(task_project, ignored);
    }
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
    fixture.config = Pelican::TestSupport::renderGraphRoot(nlohmann::json{
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
    });
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
    fixture.useAuthoredConfig(readJsonFixture(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test/production_fixtures/pass_field_ownership/gpu_draw_material.json"),
        "node-order");
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    auto &authored = renderingSectionOf(fixture.config);
    authored["frame_node_order"] = {{"main", {"build_visible_draws", "gpu_geometry"}}};
    SECTION("flat and XR accept independent graph orders") {
#if PELICAN_WITH_OPENXR
        fixture.variants.push_back({.graph_variant = RenderPipelineGraphVariant::xr});
        authored["frame_node_order"]["main#xr"] = {"build_visible_draws#xr", "gpu_geometry"};
#endif
        const auto output = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input());
        REQUIRE(output.variants.front().frame_plans.contains("main"));
#if PELICAN_WITH_OPENXR
        REQUIRE(output.variants.back().frame_plans.contains("main#xr"));
#endif
    }
    SECTION("unknown graph is still rejected across the program") {
        authored["frame_node_order"]["unknown"] = nlohmann::json::array();
        REQUIRE_THROWS_WITH(runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input()),
                            Catch::Matchers::ContainsSubstring("frame_node_order unknown graph: unknown"));
    }
    SECTION("data-only compilation still rejects duplicate names") {
        fixture.variants.front().artifact = RenderCompilerProgramArtifact::data_only;
        authored["frame_node_order"]["main"] = {"gpu_geometry", "gpu_geometry"};
        REQUIRE_THROWS_WITH(runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input()),
                            Catch::Matchers::ContainsSubstring("frame_node_order duplicate node"));
    }
}

// P-2a-4b: `rendering_config` is the authored document, so a `pelican.graph`
// root keeps `frame_node_order` in its `rendering` section. Reading the
// document root made this cross-variant check vacuous for every graph-root
// project instead of rejecting an unknown graph by name.
TEST_CASE("P-2a-4b frame_node_order is read in a pelican.graph root's rendering section",
          "[rendercompiler][node-order][graph-root][p2a4b]") {
    ProgramInputFixture fixture;
    auto section = readJsonFixture(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test/production_fixtures/pass_field_ownership/gpu_draw_material.json");
    // A graph root reserves `buffers` and `compute_tasks` for the graph
    // itself, so this fixture drops them and the GPU draw source that reads
    // them; the two passes and their targets are otherwise the shipping ones.
    section.erase("buffers");
    section.erase("compute_tasks");
    auto &passes = section.at("rendering_passes").at(0).at("passes");
    REQUIRE(passes.at(0).at("name") == "gpu_geometry");
    passes.at(0).erase("gpu_draw_source");
    section["frame_node_order"] = {{"main", {"gpu_geometry", "present"}}};

    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    fixture.config = Pelican::TestSupport::renderGraphRoot(section);
    REQUIRE_FALSE(fixture.config.contains("frame_node_order"));

    SECTION("a known graph order is accepted through the section") {
        const auto output = runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(), fixture.input());
        REQUIRE(output.variants.front().frame_plans.contains("main"));
    }
    SECTION("an unknown graph is still rejected by name") {
        renderingSectionOf(fixture.config)["frame_node_order"]["unknown"] =
            nlohmann::json::array();
        REQUIRE_THROWS_WITH(
            runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), fixture.input()),
            Catch::Matchers::ContainsSubstring("frame_node_order unknown graph: unknown"));
    }
}

TEST_CASE(
    "WP311 production compiler retains GPU draw reads and pins final buffer IDs",
    "[wp311][render-compiler][gpu-draw][pass-field-ownership]") {
    ProgramInputFixture fixture;
    fixture.useAuthoredConfig(readJsonFixture(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
        "test/production_fixtures/pass_field_ownership/gpu_draw_material.json"),
        "gpu-draw");
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
        Pelican::TestSupport::renderGraphRoot(nlohmann::json{
            {"pipeline", {{"preset", preset_reference}}},
            {"features", nlohmann::json::array({feature_reference})},
        })
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


// LP-Va: view_select travels from the rendering section through the
// production compiler into the frame plan, the execution plan, and the
// physical scope, and is checked by name at recompile time.
nlohmann::json lpvaViewSelectConfig(nlohmann::json first_select,
                                    std::string family = "$capture/cube") {
    const auto face_pass = [&](std::string name, nlohmann::json select) {
        return nlohmann::json{
            {"name", std::move(name)},
            {"type", "fullscreen"},
            {"view_family", family},
            {"view_select", std::move(select)},
            {"output", {{"color", nlohmann::json::array({"cube_ao"})},
                        {"depth", nullptr}}},
            {"shader", {{"vertex", "engine://fullscreen"},
                        {"fragment", "engine://fullscreen"}}},
        };
    };
    return Pelican::TestSupport::renderGraphRoot(nlohmann::json{
        {"render_targets",
         nlohmann::json::array({
             {{"name", "cube_ao"},
              {"extent_scale", 1.0},
              {"width", 16},
              {"height", 16},
              {"layers", 6},
              {"format", "R8_UNORM"},
              {"usage", nlohmann::json::array(
                            {"COLOR_ATTACHMENT", "SAMPLED"})}},
         })},
        {"rendering_passes",
         nlohmann::json::array({
             {{"name", "main"},
              {"passes",
               nlohmann::json::array({
                   face_pass("face_first", std::move(first_select)),
                   face_pass("face_three", {{"face", 3}}),
                   {{"name", "present"},
                    {"type", "fullscreen"},
                    {"output", {{"color", "swapchain"},
                                {"depth", nullptr}}},
                    {"shader", {{"vertex", "engine://fullscreen"},
                                {"fragment", "engine://fullscreen"}}}},
               })}},
         })},
    });
}

CodedRefusal lpvaCompileRefusal(const nlohmann::json &config) {
    ProgramInputFixture fixture;
    fixture.config = config;
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    try {
        (void)runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(), fixture.input());
    } catch (const std::exception &error) {
        INFO(error.what());
        auto refusal = splitCodedRefusalMessage(error.what());
        REQUIRE(refusal);
        return *refusal;
    }
    FAIL("the compiler accepted an invalid view_select");
    return {};
}

#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
TEST_CASE("LP-Va view_select reaches the frame plan and the physical scope",
          "[rendercompiler][view-select][lpva]") {
    ProgramInputFixture fixture;
    fixture.config = lpvaViewSelectConfig({{"face", 2}});
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    const auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(), fixture.input());
    const auto &variant = output.variants.front();
    const auto &plan = variant.frame_plans.at("main");
    const auto node = std::find_if(
        plan.nodes.begin(), plan.nodes.end(),
        [](const auto &entry) { return entry.name == "face_first"; });
    REQUIRE(node != plan.nodes.end());
    CHECK(node->view_select == RenderViewSelect{{"face", 2}});
    const auto plan_json = framePlanToJson(plan, nullptr);
    bool reported = false;
    for (const auto &entry : plan_json.at("nodes")) {
        if (entry.at("name") == "face_first") {
            CHECK(entry.at("view_select") == nlohmann::json{{"face", 2}});
            reported = true;
        }
        if (entry.at("name") == "present") {
            CHECK_FALSE(entry.contains("view_select"));
        }
    }
    CHECK(reported);
    const auto &execution = variant.execution_plans.at("main");
    const auto executed = std::find_if(
        execution.nodes.begin(), execution.nodes.end(),
        [](const auto &entry) { return entry.name == "face_first"; });
    REQUIRE(executed != execution.nodes.end());
    CHECK(executed->view_select == RenderViewSelect{{"face", 2}});

    const auto &physical =
        requireVulkanRenderCompilerPhysicalPackage(
            *variant.physical_package);
    const auto &target_plan = *physical.target_plans.at("main");
    std::size_t selected_scopes = 0;
    for (const auto &scope : target_plan.scopes) {
        const auto has = [&](std::string_view name) {
            return std::find(scope.nodes.begin(), scope.nodes.end(),
                             name) != scope.nodes.end();
        };
        // Different selections never share a scope.
        CHECK_FALSE((has("face_first") && has("face_three")));
        if (has("face_first")) {
            CHECK(scope.view_select == RenderViewSelect{{"face", 2}});
            ++selected_scopes;
        }
        if (has("face_three")) {
            CHECK(scope.view_select == RenderViewSelect{{"face", 3}});
            ++selected_scopes;
        }
        if (has("present")) CHECK(scope.view_select.empty());
    }
    CHECK(selected_scopes == 2);
    const auto target_json = vulkanTargetPlanToJson(target_plan);
    bool scope_reported = false;
    for (const auto &scope : target_json.at("scopes")) {
        if (scope.contains("view_select") &&
            scope.at("view_select") == nlohmann::ordered_json{{"face", 2}}) {
            scope_reported = true;
        }
    }
    CHECK(scope_reported);
}

TEST_CASE("LP-Va view_select refusals are named at recompile time",
          "[rendercompiler][view-select][lpva][diagnostic]") {
    SECTION("an axis the family does not have") {
        const auto refusal = lpvaCompileRefusal(
            lpvaViewSelectConfig({{"cascade", 0}}));
        CHECK(refusal.code == "graph.view_select_axis_unknown@1");
        CHECK(refusal.fields ==
              nlohmann::json{{"pass", "face_first"},
                             {"family", "$capture/cube"},
                             {"axis", "cascade"}});
    }
    SECTION("a value outside the compile-time length") {
        const auto refusal = lpvaCompileRefusal(
            lpvaViewSelectConfig({{"face", 6}}));
        CHECK(refusal.code == "graph.view_select_axis_unknown@1");
        CHECK(refusal.fields ==
              nlohmann::json{{"pass", "face_first"},
                             {"family", "$capture/cube"},
                             {"axis", "face"},
                             {"value", 6},
                             {"length", 6}});
    }
    SECTION("a runtime axis") {
        const auto refusal = lpvaCompileRefusal(
            lpvaViewSelectConfig({{"light", 0}}, "$shadow/point"));
        CHECK(refusal.code == "graph.view_select_runtime_axis@1");
        CHECK(refusal.fields ==
              nlohmann::json{{"pass", "face_first"},
                             {"family", "$shadow/point"},
                             {"axis", "light"}});
    }
    SECTION("a family without a provider has no axes") {
        const auto refusal = lpvaCompileRefusal(
            lpvaViewSelectConfig({{"face", 0}}, "$authored/probe"));
        CHECK(refusal.code == "graph.view_select_axis_unknown@1");
        CHECK(refusal.fields.at("family") == "$authored/probe");
    }
    SECTION("the primary family") {
        const auto refusal = lpvaCompileRefusal(
            lpvaViewSelectConfig({{"face", 0}}, "$main"));
        CHECK(refusal.code == "graph.view_select_primary_family@1");
        CHECK(refusal.fields ==
              nlohmann::json{{"pass", "face_first"},
                             {"family", "$main"}});
    }
}

// VF-1: two fullscreen passes on the cube family; the second writes
// `second_target`. The renderer sizes the family's views from the targets
// its passes write, so they must agree.
nlohmann::json vf1CubeFamilyConfig(std::string second_target) {
    const auto cube_pass = [](std::string name, std::string target) {
        return nlohmann::json{
            {"name", std::move(name)},
            {"type", "fullscreen"},
            {"view_family", "$capture/cube"},
            {"output", {{"color", nlohmann::json::array({std::move(target)})},
                        {"depth", nullptr}}},
            {"shader", {{"vertex", "engine://fullscreen"},
                        {"fragment", "engine://fullscreen"}}},
        };
    };
    const auto target = [](std::string name, nlohmann::json size) {
        nlohmann::json declaration{
            {"name", std::move(name)},
            {"extent_scale", 1.0},
            {"format", "R8_UNORM"},
            {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
        declaration.update(size);
        return declaration;
    };
    return Pelican::TestSupport::renderGraphRoot(nlohmann::json{
        {"render_targets",
         nlohmann::json::array({
             target("cube_ao", {{"width", 16}, {"height", 16}, {"layers", 6}}),
             target("cube_ao_second", {{"width", 16}, {"height", 16}, {"layers", 6}}),
             target("cube_small", {{"width", 8}, {"height", 8}, {"layers", 6}}),
             // Sized from the output: the screen.
             target("screen_ao", nlohmann::json::object()),
         })},
        {"rendering_passes",
         nlohmann::json::array({
             {{"name", "main"},
              {"passes",
               nlohmann::json::array({
                   cube_pass("cube_first", "cube_ao"),
                   cube_pass("cube_second", std::move(second_target)),
                   {{"name", "present"},
                    {"type", "fullscreen"},
                    {"output", {{"color", "swapchain"},
                                {"depth", nullptr}}},
                    {"shader", {{"vertex", "engine://fullscreen"},
                                {"fragment", "engine://fullscreen"}}}},
               })}},
         })},
    });
}

TEST_CASE("VF-1 a view family's passes at different extents are refused by name at compile",
          "[rendercompiler][view-family][vf1][diagnostic]") {
    SECTION("a cube-family pass writing a screen-sized target") {
        const auto refusal = lpvaCompileRefusal(vf1CubeFamilyConfig("screen_ao"));
        CHECK(refusal.code == "render.view_family_extent_mismatch@1");
        CHECK(refusal.fields ==
              nlohmann::json{{"family", "$capture/cube"},
                             {"pass", "cube_second"},
                             {"target", "screen_ao"},
                             {"extent", {{"output_scale", 1.0}}},
                             {"expected_pass", "cube_first"},
                             {"expected_target", "cube_ao"},
                             {"expected_extent", {16, 16}}});
    }
    SECTION("a cube-family pass writing a fixed target of another size") {
        const auto refusal = lpvaCompileRefusal(vf1CubeFamilyConfig("cube_small"));
        CHECK(refusal.code == "render.view_family_extent_mismatch@1");
        CHECK(refusal.fields.at("pass") == "cube_second");
        CHECK(refusal.fields.at("extent") == nlohmann::json{8, 8});
        CHECK(refusal.fields.at("expected_extent") == nlohmann::json{16, 16});
        CHECK_FALSE(refusal.fields.contains("loop_origin"));
    }
    SECTION("passes whose targets agree compile") {
        ProgramInputFixture fixture;
        fixture.config = vf1CubeFamilyConfig("cube_ao_second");
        fixture.backend_context = VulkanRenderCompilerBackendContext{
            vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
            VulkanRenderCompilerDevicePlanningMode::compiler_only};
        fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
        CHECK_NOTHROW(runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(), fixture.input()));
    }
}

// LP-Va2: get_frame_plan's nodes carry their family's axes, read from the
// providers against the pipeline the production compiler composed, so the
// cascade length is the feature's cascade_count and not a fixed number.
nlohmann::json lpva2FramePlan(std::uint32_t cascade_count) {
    ProgramInputFixture fixture;
    fixture.config = Pelican::TestSupport::renderGraphRoot(nlohmann::json{
        {"pipeline",
         {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features",
         nlohmann::json::array({
             "engine://features/cube_capture.json",
             {{"ref", "engine://features/shadow_directional.json"},
              {"parameters", {{"cascade_count", cascade_count}}}},
         })},
    });
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    // Both features need the runtime compiler to be admitted; the
    // compiler-only planning mode compiles no shader.
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    const auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(), input);
    const auto &variant = output.variants.front();
    REQUIRE(variant.compiled_pipeline != nullptr);
    return framePlanToJson(variant.frame_plans.at("main_render"),
                           variant.compiled_pipeline.get());
}

nlohmann::json lpva2Node(const nlohmann::json &plan,
                         std::string_view name) {
    for (const auto &node : plan.at("nodes")) {
        if (node.at("name").get<std::string>() == name) return node;
    }
    FAIL("no frame plan node " << name);
    return {};
}

TEST_CASE("LP-Va2 frame plan nodes carry their family's view axes",
          "[rendercompiler][view-select][lpva2]") {
    const auto cube_axes = nlohmann::json::array({
        {{"axis", "face"},
         {"length", 6},
         {"runtime", false},
         {"labels", {"+X", "-X", "+Y", "-Y", "+Z", "-Z"}}},
    });
    for (const std::uint32_t cascades : {3u, 2u}) {
        INFO("cascade_count " << cascades);
        const auto plan = lpva2FramePlan(cascades);
        // Every node of the cube capture feature (its 8 passes, the two
        // snapshot copies among them) has the face axis.
        std::size_t cube_nodes = 0;
        for (const auto &node : plan.at("nodes")) {
            INFO(node.dump());
            if (node.value("view_family", std::string{}) ==
                "$capture/cube") {
                CHECK(node.at("view_axes") == cube_axes);
                ++cube_nodes;
            }
            // A pass (snapshot copies included) or a compute task always
            // has the field; anchors and the output transform never.
            const auto kind = node.at("kind").get<std::string>();
            CHECK(node.contains("view_axes") ==
                  (kind == "render" || kind == "compute" ||
                   kind == "snapshot_copy"));
        }
        CHECK(cube_nodes == 8);
        // Directional shadows: the light (outermost) is known per frame,
        // the cascade count when the graph is recompiled.
        CHECK(lpva2Node(plan, "shadow_depth").at("view_axes") ==
              nlohmann::json::array({
                  {{"axis", "light"}, {"length", nullptr}, {"runtime", true}},
                  {{"axis", "cascade"},
                   {"length", cascades},
                   {"runtime", false}},
              }));
        // The primary family has no axes.
        const auto main_pass = lpva2Node(plan, "deferred_geometry");
        CHECK_FALSE(main_pass.contains("view_family"));
        CHECK(main_pass.at("view_axes") == nlohmann::json::array());
    }
}

// VF-2: get_frame_plan's secondary-family nodes carry the extent VF-1 holds
// the family to (family_extent, family_layers, family_extent_pass).
TEST_CASE("VF-2 frame plan nodes carry their view family's reference extent",
          "[rendercompiler][view-family][vf2]") {
    for (const std::uint32_t cascades : {3u, 2u}) {
        INFO("cascade_count " << cascades);
        const auto plan = lpva2FramePlan(cascades);
        std::size_t cube_nodes = 0;
        for (const auto &node : plan.at("nodes")) {
            INFO(node.dump());
            const auto family =
                node.value("view_family", std::string{});
            if (family == "$capture/cube") {
                // Snapshot copies included: every node of the family.
                CHECK(node.at("family_extent") ==
                      nlohmann::json{256, 256});
                CHECK(node.at("family_layers") == 6);
                CHECK(node.at("family_extent_pass") ==
                      "cube_capture_geometry");
                ++cube_nodes;
            } else if (family.empty()) {
                // `$main` has none.
                CHECK_FALSE(node.contains("family_extent"));
                CHECK_FALSE(node.contains("family_layers"));
                CHECK_FALSE(node.contains("family_extent_pass"));
            }
        }
        CHECK(cube_nodes == 8);
        const auto shadow = lpva2Node(plan, "shadow_depth");
        CHECK(shadow.at("family_extent") == nlohmann::json{2048, 2048});
        CHECK(shadow.at("family_layers") == cascades);
        CHECK(shadow.at("family_extent_pass") == "shadow_depth");
        CHECK_FALSE(lpva2Node(plan, "deferred_geometry")
                        .contains("family_extent"));
    }
}

TEST_CASE("VF-2 the reported reference is the one VF-1 refuses against",
          "[rendercompiler][view-family][vf2][diagnostic]") {
    ProgramInputFixture fixture;
    fixture.config = vf1CubeFamilyConfig("cube_ao_second");
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    const auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(), fixture.input());
    const auto &variant = output.variants.front();
    REQUIRE(variant.compiled_pipeline != nullptr);
    const auto &plan = variant.frame_plans.at("main");
    const auto plan_json =
        framePlanToJson(plan, variant.compiled_pipeline.get());
    const auto second = lpva2Node(plan_json, "cube_second");
    CHECK(second.at("family_extent") == nlohmann::json{16, 16});
    CHECK(second.at("family_layers") == 6);
    CHECK(second.at("family_extent_pass") == "cube_first");
    CHECK(lpva2Node(plan_json, "cube_first").at("family_extent") ==
          second.at("family_extent"));
    CHECK_FALSE(lpva2Node(plan_json, "present").contains("family_extent"));
    // A raw plan (no compiled pipeline) has no reference to report.
    for (const auto &node : framePlanToJson(plan, nullptr).at("nodes")) {
        CHECK_FALSE(node.contains("family_extent"));
    }
    // The refusal of a pass that disagrees names the same reference.
    for (const auto *other : {"screen_ao", "cube_small"}) {
        INFO(other);
        const auto refusal = lpvaCompileRefusal(vf1CubeFamilyConfig(other));
        REQUIRE(refusal.code == "render.view_family_extent_mismatch@1");
        CHECK(refusal.fields.at("expected_extent") ==
              second.at("family_extent"));
        CHECK(refusal.fields.at("expected_pass") ==
              second.at("family_extent_pass"));
    }
    // The helper both call answers the same.
    const auto references =
        variant.compiled_pipeline->view_family_extents;
    REQUIRE(references.size() == 1);
    CHECK(references.front().family == "$capture/cube");
    CHECK(references.front().target == "cube_ao");
    CHECK(renderViewFamilyExtentJson(references.front()) ==
          second.at("family_extent"));
}

// LP-0e: a graph with render.snapshot_copy / render.shadow_depth nodes in the
// place of feature passes, through the production compiler (compiler-only
// planning compiles no shader).
struct Lp0eCompiled {
    nlohmann::json frame_plan;
    FrameExecutionPlan execution;
};

Lp0eCompiled lp0eCompile(nlohmann::json nodes, nlohmann::json definitions,
                         nlohmann::json features) {
    ProgramInputFixture fixture;
    fixture.config = nlohmann::json{
        {"schema", "pelican.graph"},
        {"version", 1},
        {"definitions", std::move(definitions)},
        {"nodes", std::move(nodes)},
        {"outputs", nlohmann::json::object()},
        {"rendering",
         {{"pipeline",
           {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
          {"features", std::move(features)}}},
    };
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    const auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(), input);
    const auto &variant = output.variants.front();
    REQUIRE(variant.compiled_pipeline != nullptr);
    return {framePlanToJson(variant.frame_plans.at("main_render"),
                            variant.compiled_pipeline.get()),
            variant.execution_plans.at("main_render")};
}

nlohmann::json lp0eImageType(const char *semantic, const char *topology) {
    nlohmann::json arguments{{"topology", topology}};
    if (std::string_view{semantic} == "pelican.render.depth@1") {
        arguments["representation"] = "device";
        arguments["space"] = "projection";
    } else {
        arguments["reference"] = "scene";
        arguments["transfer"] = "linear";
        arguments["range"] = "extended";
    }
    return {{"semantic", semantic}, {"arguments", std::move(arguments)}};
}

nlohmann::json lp0eRef(const char *id, const char *name,
                       const nlohmann::json &type) {
    return {{"id", id},
            {"op", "render.target_ref"},
            {"parameters", {{"name", name}, {"type", type}}},
            {"outputs", {"resource"}}};
}

// A frame plan node less who authored it, renamed.
nlohmann::json lp0ePlanNode(const nlohmann::json &plan, std::string_view name,
                            const std::string &as) {
    auto node = lpva2Node(plan, name);
    for (const auto *key :
         {"source", "provider_feature", "provider_ref", "authored_name",
          "origin", "replaces"}) {
        node.erase(key);
    }
    node["name"] = as;
    return node;
}

TEST_CASE("LP-0e graph shadow_depth and snapshot_copy nodes compile in the "
          "features' passes' place",
          "[rendercompiler][view-select][lp0e][shadow][snapshot][cube]") {
    using Json = nlohmann::json;
    SECTION("a cube capture copy: the same frame plan node and execution edge") {
        const Json copy{
            {"id", "opaque_color_copy"},
            {"op", "render.snapshot_copy"},
            {"parameters",
             {{"pass_set", "main_render"},
              {"view_family", "$capture/cube"},
              {"snapshot_after", "cube_capture_forward_opaque"}}},
            {"inputs",
             {{"source", {{"node", "capture_color"}, {"port", "resource"}}},
              {"destination",
               {{"node", "opaque_color"}, {"port", "resource"}}}}},
            {"outputs", Json::array()}};
        const auto kept = lp0eCompile(
            Json::array(), Json::object(),
            Json::array({"engine://features/cube_capture.json"}));
        const auto graph = lp0eCompile(
            Json::array({lp0eRef("capture_color", "cube_capture_color",
                                 lp0eImageType("pelican.render.color_signal@1",
                                               "cube")),
                         lp0eRef("opaque_color", "cube_capture_opaque_color",
                                 lp0eImageType("pelican.render.color_signal@1",
                                               "two_d_array")),
                         copy}),
            Json::object(),
            Json::array({{{"ref", "engine://features/cube_capture.json"},
                          {"pass_replacements",
                           {{"cube_capture_snapshot_opaque_color",
                             "opaque_color_copy"}}}}}));
        CHECK(lp0ePlanNode(graph.frame_plan, "opaque_color_copy",
                           "opaque_color_copy") ==
              lp0ePlanNode(kept.frame_plan,
                           "cube_capture_snapshot_opaque_color",
                           "opaque_color_copy"));
        const auto edge = [](const FrameExecutionPlan &plan,
                             const std::string &to) {
            std::vector<std::pair<std::string, std::string>> found;
            for (const auto &dependency : plan.dependencies) {
                if (dependency.to == to) {
                    found.emplace_back(dependency.from, dependency.reason);
                }
            }
            return found;
        };
        const auto graph_edges = edge(graph.execution, "opaque_color_copy");
        CHECK(graph_edges ==
              edge(kept.execution, "cube_capture_snapshot_opaque_color"));
        CHECK(std::find(graph_edges.begin(), graph_edges.end(),
                        std::pair<std::string, std::string>{
                            "cube_capture_forward_opaque",
                            "pelican.dependency.snapshot_after@1"}) !=
              graph_edges.end());
    }
    SECTION("a loop over the cascades, each copy selecting its own") {
        const Json body{
            {"nodes",
             Json::array({
                 {{"id", "target"}, {"op", "graph.input"}},
                 {{"id", "depth"},
                  {"op", "render.shadow_depth"},
                  {"parameters",
                   {{"pass_set", "main_render"},
                    {"view_family", "$shadow/directional"},
                    {"view_select", {{"cascade", {{"$expr", "element"}}}}},
                    {"depth_store_op", "store"}}},
                  {"inputs", {{"depth", {{"node", "target"}, {"port", "value"}}}}},
                  {"outputs", Json::array()}},
             })},
            {"outputs", Json::object()}};
        const Json loop{
            {"id", "cascades"},
            {"op", "graph.loop"},
            {"parameters",
             {{"over", {{"range", {{"count", 3}}}}},
              {"branches", Json::array({{{"body", "cascade"}}})}}},
            {"inputs", {{"target", {{"node", "shadow"}, {"port", "resource"}}}}},
            {"outputs", Json::array()}};
        const auto compiled = lp0eCompile(
            Json::array({lp0eRef("shadow", "shadow_map",
                                 lp0eImageType("pelican.render.depth@1",
                                               "two_d_array")),
                         loop}),
            Json{{"cascade", body}},
            Json::array({{{"ref", "engine://features/shadow_directional.json"},
                          {"parameters", {{"cascade_count", 3}}},
                          {"pass_replacements",
                           {{"shadow_depth", "cascades"}}}}}));
        const auto axes = Json::array({
            {{"axis", "light"}, {"length", nullptr}, {"runtime", true}},
            {{"axis", "cascade"}, {"length", 3}, {"runtime", false}},
        });
        for (std::int64_t element = 0; element < 3; ++element) {
            const auto name =
                "cascades/" + std::to_string(element) + "/depth";
            INFO(name);
            const auto node = lpva2Node(compiled.frame_plan, name);
            CHECK(node.at("view_select") == Json{{"cascade", element}});
            CHECK(node.at("view_axes") == axes);
            CHECK(node.at("writes") == Json::array({"shadow_map"}));
            const auto executed = std::find_if(
                compiled.execution.nodes.begin(),
                compiled.execution.nodes.end(),
                [&](const auto &entry) { return entry.name == name; });
            REQUIRE(executed != compiled.execution.nodes.end());
            CHECK(executed->view_select ==
                  RenderViewSelect{
                      {"cascade", static_cast<std::uint32_t>(element)}});
        }
        for (const auto &node : compiled.frame_plan.at("nodes")) {
            CHECK(node.at("name") != "shadow_depth");
        }
    }
}

// GL-2a: every frame plan node a graph node made says which node, and where
// it is authored (origin); a node that took a feature pass's place says which
// (replaces); the physical plan's resources, alias groups, decisions and
// backend candidates name the frame plan nodes they concern (from_nodes).
// So from any element of any layer, a node name and its origin lead back to
// the authored graph -- also under an engine:// preset, where every one of
// these passes has source "engine".
TEST_CASE("GL-2a frame plan nodes carry the graph node that made them",
          "[rendercompiler][frame-plan][gl2a]") {
    using Json = nlohmann::json;
    const Json scene_color{
        {"semantic", "pelican.render.color_signal@1"},
        {"arguments",
         {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    const auto image = [&](const char *id) {
        return Json{{"id", id},
                    {"op", "render.image"},
                    {"parameters",
                     {{"type", scene_color},
                      {"format", "R16G16B16A16_SFLOAT"},
                      {"extent_scale", 1.0},
                      {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
                    {"outputs", {"resource"}}};
    };
    const auto fill = [&](const char *id, const char *target) {
        return Json{
            {"id", id},
            {"op", "render.fullscreen"},
            {"parameters",
             {{"pass_set", "main_render"},
              {"shader",
               {{"vertex", "engine://fullscreen"},
                {"fragment", "engine://passthrough"}}},
              {"ports",
               {{"color",
                 {{"kind", "image"}, {"type", scene_color}, {"access", "write"},
                  {"attachment", "color"}, {"location", 0}}}}}}},
            {"inputs", {{"color", {{"node", target}, {"port", "resource"}}}}},
            {"outputs", {"color"}}};
    };
    const auto compute = [](const char *id, const char *buffer) {
        return Json{
            {"id", id},
            {"op", "render.compute"},
            {"parameters",
             {{"shader", "engine://ray_deform"},
              {"dispatch", {{"groups", {1, 1, 1}}}},
              {"ports",
               {{"data",
                 {{"kind", "buffer"}, {"element", "float"},
                  {"access", "read_write"}}}}}}},
            {"inputs", {{"data", {{"node", buffer}, {"port", "resource"}}}}},
            {"outputs", {"data"}}};
    };
    const auto buffer = [](const char *id) {
        return Json{{"id", id},
                    {"op", "render.buffer"},
                    {"parameters", {{"element", "float"}, {"size", 16}}},
                    {"outputs", {"resource"}}};
    };
    const Json cascade{
        {"nodes",
         Json::array({
             {{"id", "target"}, {"op", "graph.input"}},
             {{"id", "depth"},
              {"op", "render.shadow_depth"},
              {"parameters",
               {{"pass_set", "main_render"},
                {"view_family", "$shadow/directional"},
                {"view_select", {{"cascade", {{"$expr", "element"}}}}},
                {"depth_store_op", "store"}}},
              {"inputs", {{"depth", {{"node", "target"}, {"port", "value"}}}}},
              {"outputs", Json::array()}},
         })},
        {"outputs", Json::object()}};
    const Json paint{
        {"nodes", Json::array({image("canvas"), fill("fill", "canvas"),
                               buffer("data"), compute("step", "data")})},
        {"outputs", Json::object()}};
    const auto nodes = Json::array({
        image("tint_image"), fill("tint", "tint_image"),
        buffer("probe_data"), compute("probe", "probe_data"),
        {{"id", "left"}, {"op", "graph.instance"}, {"definition", "paint"}},
        lp0eRef("shadow", "shadow_map",
                lp0eImageType("pelican.render.depth@1", "two_d_array")),
        {{"id", "cascades"},
         {"op", "graph.loop"},
         {"parameters",
          {{"over", {{"range", {{"count", 3}}}}},
           {"branches", Json::array({{{"body", "cascade"}}})}}},
         {"inputs", {{"target", {{"node", "shadow"}, {"port", "resource"}}}}},
         {"outputs", Json::array()}},
    });
    ProgramInputFixture fixture;
    fixture.config = Json{
        {"schema", "pelican.graph"},
        {"version", 1},
        {"definitions", {{"paint", paint}, {"cascade", cascade}}},
        {"nodes", nodes},
        {"outputs", Json::object()},
        {"rendering",
         {{"pipeline",
           {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
          {"features",
           Json::array({{{"ref", "engine://features/shadow_directional.json"},
                         {"parameters", {{"cascade_count", 3}}},
                         {"pass_replacements",
                          {{"shadow_depth", "cascades"}}}}})}}},
    };
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    const auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(), input);
    const auto &variant = output.variants.front();
    REQUIRE(variant.compiled_pipeline != nullptr);
    const std::string graph = "passes/main.json";
    auto plan = framePlanToJson(variant.frame_plans.at("main_render"),
                                variant.compiled_pipeline.get(), graph);
    REQUIRE(variant.physical_package != nullptr);
    const auto &physical =
        requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
    REQUIRE(physical.target_plans.contains("main_render"));
    plan["physical_target_plan"] =
        vulkanTargetPlanToJson(*physical.target_plans.at("main_render"));
    appendFramePlanPhysicalNodeReferences(plan);
    INFO(plan.dump());

    std::set<std::string> names;
    for (const auto &node : plan.at("nodes"))
        names.insert(node.at("name").get<std::string>());
    const auto node = [&](const std::string &name) { return lpva2Node(plan, name); };

    // A root node's pass and task: the node, nothing else.
    CHECK(node("tint").at("source") == "engine");
    CHECK(node("tint").at("origin") == Json{{"graph", graph}, {"node", "tint"}});
    CHECK(node("probe").at("origin") == Json{{"graph", graph}, {"node", "probe"}});
    // Inside an instance: the definition and the instance path. The same
    // place get_compute_task_authoring's graph_node names for the task
    // ({scope: "paint", node: "step", instance: "left"}).
    CHECK(node("left/fill").at("origin") ==
          Json{{"graph", graph}, {"node", "fill"}, {"definition", "paint"},
               {"instance_path", "left"}});
    CHECK(node("left/step").at("origin") ==
          Json{{"graph", graph}, {"node", "step"}, {"definition", "paint"},
               {"instance_path", "left"}});
    // A loop's copies: the body node, the loop, its branch and the element
    // (the element loop_origin names), and the feature pass they replace.
    for (std::int64_t element = 0; element < 3; ++element) {
        const auto name = "cascades/" + std::to_string(element) + "/depth";
        INFO(name);
        const auto copy = node(name);
        CHECK(copy.at("origin") ==
              Json{{"graph", graph}, {"node", "depth"}, {"definition", "cascade"},
                   {"loop", "cascades"}, {"loop_branch", 0}, {"element", element}});
        CHECK(copy.at("loop_origin").at("element") == element);
        CHECK(copy.at("replaces") ==
              Json::array({{{"feature", "shadow_directional"}, {"pass", "shadow_depth"}}}));
    }
    CHECK_FALSE(names.contains("shadow_depth"));
    // The preset's and the feature's passes carry none, though the preset's
    // are "engine" as the graph's are.
    for (const auto &entry : plan.at("nodes")) {
        const auto name = entry.at("name").get<std::string>();
        INFO(name);
        const bool graph_node = name == "tint" || name == "probe" ||
                                name.starts_with("left/") ||
                                name.starts_with("cascades/");
        CHECK(entry.contains("origin") == graph_node);
        CHECK(entry.contains("replaces") == name.starts_with("cascades/"));
    }
    CHECK(node("deferred_geometry").at("source") == "engine");

    // The physical plan: every element that names no node itself names the
    // frame plan nodes it concerns, and every such name is a frame plan node.
    const auto &physical_plan = plan.at("physical_target_plan");
    const auto checkNames = [&](const Json &element) {
        REQUIRE(element.contains("from_nodes"));
        for (const auto &name : element.at("from_nodes"))
            CHECK(names.contains(name.get<std::string>()));
    };
    bool shadow_map_found = false;
    for (const auto &resource : physical_plan.at("resources")) {
        INFO(resource.at("logical_resource").dump());
        checkNames(resource);
        if (resource.at("logical_resource") == "shadow_map") {
            shadow_map_found = true;
            const auto &from = resource.at("from_nodes");
            for (const auto *copy :
                 {"cascades/0/depth", "cascades/1/depth", "cascades/2/depth"})
                CHECK(std::find(from.begin(), from.end(), copy) != from.end());
        }
        if (resource.at("logical_resource") == "tint_image")
            CHECK(resource.at("from_nodes") == Json::array({"tint"}));
    }
    CHECK(shadow_map_found);
    for (const auto &group : physical_plan.at("alias_groups")) checkNames(group);
    std::size_t decisions = 0;
    for (const auto *container : {&physical_plan, &physical_plan.at("lowering_graph"),
                                  &physical_plan.at("planning_opportunities"),
                                  &physical_plan.at("backend_selection")})
        for (const auto &decision : container->at("decisions")) {
            INFO(decision.dump());
            checkNames(decision);
            ++decisions;
        }
    CHECK(decisions > 0);
    for (const auto &candidate :
         physical_plan.at("backend_selection").at("candidates"))
        checkNames(candidate);

    // The pipeline's diagnostics and the nodes the variant made inactive.
    CHECK(plan.at("deactivated_graph_nodes") == Json::array());
    const auto &diagnostics = plan.at("diagnostics");
    CHECK(std::find(diagnostics.begin(), diagnostics.end(),
                    Json{{"kind", "graph_variant_selected"}, {"subject", "flat"},
                         {"detail", "selected_by_environment"}}) != diagnostics.end());
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](const Json &entry) {
        return entry.at("kind") == "pipeline_preset_resolved" &&
               entry.at("subject") == "engine://render_pipelines/hybrid_v1.json";
    }));
}

// FG-1: a fold group's decisions are written in the root's node ids; every
// composition maps them onto the names it expanded (a loop's elements) and
// adds them to target_planning, and get_frame_plan marks the decisions they
// made with the group (from_fold). They are used only while the open form
// is the one verified; otherwise the open form runs and says why.
nlohmann::json fg1Image(const char *id) {
    return {{"id", id},
            {"op", "render.image"},
            {"parameters",
             {{"type",
               {{"semantic", "pelican.render.color_signal@1"},
                {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}}},
              {"format", "R16G16B16A16_SFLOAT"},
              {"extent_scale", 1.0},
              {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
            {"outputs", {"resource"}}};
}

nlohmann::json fg1Fill(const char *id, const char *target) {
    return {{"id", id},
            {"op", "render.fullscreen"},
            {"parameters",
             {{"pass_set", "main_render"},
              {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
              {"ports",
               {{"color",
                 {{"kind", "image"},
                  {"type",
                   {{"semantic", "pelican.render.color_signal@1"},
                    {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}}},
                  {"access", "write"},
                  {"attachment", "color"},
                  {"location", 0}}}}}}},
            {"inputs", {{"color", {{"node", target}, {"port", "resource"}}}}},
            {"outputs", {"color"}}};
}

const std::vector<std::string> fg1GroupNodes{"fold_a", "fold_b", "fold_image", "blur"};

// Two root passes, the image one of them writes, and a loop over `count`
// elements whose body paints an image of its own; the group "post" holds
// them all. `folded` is the group's folded form without `verified`.
nlohmann::json fg1Document(std::int64_t count, const nlohmann::json &folded) {
    using Json = nlohmann::json;
    const Json body{{"nodes", Json::array({fg1Image("canvas"), fg1Fill("paint", "canvas")})},
                    {"outputs", Json::object()}};
    Json document{
        {"schema", "pelican.graph"},
        {"version", 1},
        {"definitions", {{"blur_body", body}}},
        {"nodes",
         Json::array({fg1Image("fold_image"), fg1Fill("fold_a", "fold_image"), fg1Image("other_image"),
                      fg1Fill("fold_b", "other_image"),
                      {{"id", "blur"},
                       {"op", "graph.loop"},
                       {"parameters",
                        {{"over", {{"range", {{"count", count}}}}},
                         {"branches", Json::array({{{"body", "blur_body"}}})}}},
                       {"inputs", Json::object()},
                       {"outputs", Json::array()}}})},
        {"outputs", Json::object()},
        {"rendering", {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}}},
    };
    if (!folded.is_null())
        document["fold_groups"] = {{"post", {{"nodes", fg1GroupNodes}, {"folded", folded}}}};
    return document;
}

// The document with the group marked verified against its open form.
nlohmann::json fg1Verified(nlohmann::json document) {
    document["fold_groups"]["post"]["folded"]["verified"] = {
        {"digest", graphFoldGroupDigest(document, fg1GroupNodes)}, {"at", "2026-10-02T00:00:00Z"}};
    return document;
}

struct Fg1Compiled {
    nlohmann::json plan;
    TargetPlanningPolicy planning;
};

Fg1Compiled fg1Compile(const nlohmann::json &config) {
    ProgramInputFixture fixture;
    fixture.config = config;
    fixture.backend_context = VulkanRenderCompilerBackendContext{
        vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    const auto output = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
    const auto &variant = output.variants.front();
    REQUIRE(variant.compiled_pipeline != nullptr);
    auto plan = framePlanToJson(variant.frame_plans.at("main_render"), variant.compiled_pipeline.get());
    REQUIRE(variant.physical_package != nullptr);
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
    REQUIRE(physical.target_plans.contains("main_render"));
    plan["physical_target_plan"] = vulkanTargetPlanToJson(*physical.target_plans.at("main_render"));
    appendFramePlanPhysicalNodeReferences(plan);
    return {std::move(plan), variant.compiled_pipeline->target_planning};
}

// The code of the graph refusal compiling `config` raises.
std::string fg1Refusal(const nlohmann::json &config, nlohmann::json *detail = nullptr) {
    try {
        (void)fg1Compile(config);
    } catch (const GraphRegistryError &error) {
        if (detail != nullptr) *detail = error.detail();
        return error.code();
    } catch (const std::exception &error) {
        return std::string{"not a graph refusal: "} + error.what();
    }
    return "not refused";
}

// What target_planning says for main_render's node / resource `name`.
std::optional<bool> fg1Isolated(const TargetPlanningPolicy &policy, std::string_view name) {
    for (const auto &graph : policy.graphs) {
        if (graph.graph != "main_render") continue;
        for (const auto &node : graph.nodes)
            if (node.node == name) return node.isolate;
    }
    return std::nullopt;
}

std::optional<bool> fg1NoAlias(const TargetPlanningPolicy &policy, std::string_view name) {
    for (const auto &graph : policy.graphs) {
        if (graph.graph != "main_render") continue;
        for (const auto &resource : graph.resources)
            if (resource.resource == name) return resource.no_alias;
    }
    return std::nullopt;
}

bool fg1Aliased(const nlohmann::json &plan, std::string_view resource) {
    for (const auto &group : plan.at("physical_target_plan").at("alias_groups"))
        for (const auto &member : group.at("resources"))
            if (member.is_string() && member.get<std::string>() == resource) return true;
    return false;
}

std::vector<nlohmann::json> fg1FoldDecisions(const nlohmann::json &plan) {
    std::vector<nlohmann::json> result;
    for (const auto &decision : plan.at("physical_target_plan").at("decisions"))
        if (decision.contains("from_fold")) result.push_back(decision);
    return result;
}

std::vector<nlohmann::json> fg1Diagnostics(const nlohmann::json &plan, std::string_view kind) {
    std::vector<nlohmann::json> result;
    for (const auto &diagnostic : plan.at("diagnostics"))
        if (diagnostic.at("kind").get<std::string>() == kind) result.push_back(diagnostic);
    return result;
}

TEST_CASE("FG-1 a verified fold group's decisions reach target_planning and the frame plan by the expanded names",
          "[rendercompiler][frame-plan][fg1]") {
    using Json = nlohmann::json;
    const Json folded{{"kind", "decisions"},
                      {"decisions",
                       {{"isolate", Json::array({{{"node", "blur"}}})},
                        {"no_alias", Json::array({{{"node", "fold_image"}}})}}}};

    // The open form alone: the image is aliased, nothing is isolated.
    const auto open = fg1Compile(fg1Document(3, nullptr));
    INFO(open.plan.at("physical_target_plan").at("alias_groups").dump());
    CHECK(fg1Aliased(open.plan, "fold_image"));
    CHECK_FALSE(fg1Isolated(open.planning, "blur/0/paint").has_value());
    CHECK_FALSE(open.plan.contains("fold_decisions"));

    SECTION("verified: each element of the loop is isolated and the image kept out of every alias group") {
        for (const std::int64_t count : {3, 2}) {
            INFO("count " << count);
            // The same decisions, whatever the count; only the mark is the
            // open form's.
            const auto compiled = fg1Compile(fg1Verified(fg1Document(count, folded)));
            INFO(compiled.plan.dump());
            for (std::int64_t element = 0; element < 3; ++element) {
                const auto name = "blur/" + std::to_string(element) + "/paint";
                CHECK(fg1Isolated(compiled.planning, name) ==
                      (element < count ? std::optional<bool>{true} : std::nullopt));
            }
            CHECK_FALSE(fg1Isolated(compiled.planning, "fold_a").has_value());
            CHECK(fg1NoAlias(compiled.planning, "fold_image") == std::optional<bool>{true});
            CHECK_FALSE(fg1Aliased(compiled.plan, "fold_image"));

            // The plan says which group each decision came from.
            std::set<std::pair<std::string, std::string>> marked;
            for (const auto &decision : fg1FoldDecisions(compiled.plan)) {
                CHECK(decision.at("id") == "pelican.plan.fold_decision_applied@1");
                CHECK(decision.at("from_fold") == "post");
                marked.emplace(decision.at("subject").get<std::string>(),
                               decision.at("selected").get<std::string>());
                if (decision.at("selected") == "isolate")
                    CHECK(decision.at("from_nodes") == Json::array({decision.at("subject")}));
                else
                    CHECK(decision.at("from_nodes") == Json::array({"fold_a"}));
            }
            std::set<std::pair<std::string, std::string>> expected{{"fold_image", "no_alias"}};
            for (std::int64_t element = 0; element < count; ++element)
                expected.emplace("blur/" + std::to_string(element) + "/paint", "isolate");
            CHECK(marked == expected);
            CHECK(compiled.plan.at("fold_decisions").size() == expected.size());
            CHECK(fg1Diagnostics(compiled.plan, "fold_stale").empty());
            CHECK(fg1Diagnostics(compiled.plan, "fold_unverified").empty());
        }
        // One element of the loop only.
        Json one = folded;
        one["decisions"]["isolate"] = Json::array({{{"node", "blur"}, {"element", 1}}});
        const auto compiled = fg1Compile(fg1Verified(fg1Document(3, one)));
        CHECK_FALSE(fg1Isolated(compiled.planning, "blur/0/paint").has_value());
        CHECK(fg1Isolated(compiled.planning, "blur/1/paint") == std::optional<bool>{true});
        CHECK_FALSE(fg1Isolated(compiled.planning, "blur/2/paint").has_value());
    }

    SECTION("the root's layout is not part of the open form") {
        auto document = fg1Document(3, folded);
        const auto digest = graphFoldGroupDigest(document, fg1GroupNodes);
        document["layout"] = {{"nodes", {{"fold_a", {{"x", 10}, {"y", 20}}}}}};
        document["definitions"]["blur_body"]["layout"] = {{"nodes", {{"paint", {{"x", 1}, {"y", 2}}}}}};
        CHECK(graphFoldGroupDigest(document, fg1GroupNodes) == digest);
        // A node outside the group is not either.
        document["nodes"].push_back(fg1Image("elsewhere"));
        CHECK(graphFoldGroupDigest(document, fg1GroupNodes) == digest);
    }

    SECTION("stale or unverified: the decisions are dropped and the open form runs, said by name") {
        auto document = fg1Verified(fg1Document(3, folded));
        const auto verified_digest =
            document["fold_groups"]["post"]["folded"]["verified"]["digest"].get<std::string>();
        // A parameter of a node inside the group.
        for (auto &node : document["nodes"])
            if (node.at("id") == "fold_image") node["parameters"]["extent_scale"] = 0.5;
        const auto current_digest = graphFoldGroupDigest(document, fg1GroupNodes);
        REQUIRE(current_digest != verified_digest);
        auto compiled = fg1Compile(document);
        CHECK(fg1Diagnostics(compiled.plan, "fold_stale") ==
              std::vector<Json>{{{"kind", "fold_stale"},
                                 {"subject", "post"},
                                 {"detail", {{"verified_digest", verified_digest},
                                             {"current_digest", current_digest}}}}});
        CHECK_FALSE(fg1Isolated(compiled.planning, "blur/0/paint").has_value());
        CHECK_FALSE(fg1NoAlias(compiled.planning, "fold_image").has_value());
        CHECK(fg1FoldDecisions(compiled.plan).empty());
        CHECK_FALSE(compiled.plan.contains("fold_decisions"));

        document["fold_groups"]["post"]["folded"].erase("verified");
        compiled = fg1Compile(document);
        CHECK(fg1Diagnostics(compiled.plan, "fold_unverified") ==
              std::vector<Json>{{{"kind", "fold_unverified"},
                                 {"subject", "post"},
                                 {"detail", {{"current_digest", current_digest}}}}});
        CHECK(fg1Diagnostics(compiled.plan, "fold_stale").empty());
        CHECK_FALSE(fg1Isolated(compiled.planning, "blur/0/paint").has_value());
    }

    SECTION("refused by name: an explicit opposite, a subject outside the group, a kind not yet supported") {
        Json detail;
        Json opposite = folded;
        opposite["decisions"]["isolate"].push_back({{"node", "fold_a"}});
        auto document = fg1Verified(fg1Document(3, opposite));
        document["rendering"]["target_planning"] = {
            {"graphs", {{"main_render", {{"nodes", {{"fold_a", {{"isolate", false}}}}}}}}}};
        CHECK(fg1Refusal(document, &detail) == "graph.fold_decision_conflict@1");
        CHECK(detail == Json{{"group", "post"}, {"name", "fold_a"}, {"field", "isolate"}});
        // The same value is no conflict.
        document["rendering"]["target_planning"]["graphs"]["main_render"]["nodes"]["fold_a"]["isolate"] = true;
        CHECK(fg1Isolated(fg1Compile(document).planning, "fold_a") == std::optional<bool>{true});

        Json outside = folded;
        outside["decisions"]["no_alias"].push_back({{"node", "other_image"}});
        document = fg1Document(3, nullptr);
        document["fold_groups"] = {{"post", {{"nodes", {"fold_a", "blur", "fold_image"}}, {"folded", outside}}}};
        CHECK(fg1Refusal(document, &detail) == "graph.fold_subject_outside@1");
        CHECK(detail == Json{{"group", "post"}, {"subject", {{"node", "other_image"}}}});

        document = fg1Document(3, Json{{"kind", "replacement"}, {"provider", "engine://x"}});
        CHECK(fg1Refusal(document, &detail) == "graph.fold_kind_unsupported@1");
        CHECK(detail == Json{{"group", "post"}, {"kind", "replacement"}});
    }
}
#endif
} // namespace
} // namespace Pelican
