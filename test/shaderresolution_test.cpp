#include "../src/core/container.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/passdefinitionjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/shaderresolution.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;

Json readJson(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error(
            "failed to open WP354 fixture: " + path.string());
    }
    return Json::parse(input);
}

ShaderReference shader(std::string ref, ShaderStage stage) {
    return makeShaderReference(std::move(ref), stage);
}

DeclaredShaderReference declared(
    DeclaredShaderStage stage, std::string ref,
    std::optional<std::size_t> index = std::nullopt) {
    return DeclaredShaderReference{
        .stage = stage,
        .index = index,
        .ref = std::move(ref),
    };
}

void addPass(CompiledRenderingPass &compiled, std::string name,
             PassInfo info,
             std::vector<DeclaredShaderReference> declarations = {},
             std::optional<PassImplementationSelection> selection =
                 std::nullopt) {
    PassDefinition pass;
    pass.name = std::move(name);
    pass.pass_info = std::move(info);
    pass.shader_declaration_parsed = true;
    pass.declared_shader_refs = std::move(declarations);
    pass.implementation_selection = std::move(selection);
    compiled.passes.push_back(CompiledPass{
        .definition = std::move(pass),
        .pass_id = PassId{
            static_cast<int>(compiled.passes.size())},
    });
}

const Json &node(const Json &plan, std::string_view name) {
    const auto found = std::find_if(
        plan.at("nodes").begin(), plan.at("nodes").end(),
        [&](const Json &candidate) {
            return candidate.at("name").get_ref<const std::string &>() ==
                   name;
        });
    REQUIRE(found != plan.at("nodes").end());
    return *found;
}

std::vector<std::string> stageNames(const Json &resolution) {
    std::vector<std::string> result;
    for (const auto &stage : resolution.at("stages")) {
        result.push_back(stage.at("stage").get<std::string>());
    }
    return result;
}

FramePlan familyFramePlan() {
    FramePlan plan;
    plan.name = "wp354_family_contract";
    const struct Entry {
        const char *name;
        FramePlanNodeKind kind;
    } entries[] = {
        {"material", FramePlanNodeKind::render},
        {"fullscreen", FramePlanNodeKind::render},
        {"raster", FramePlanNodeKind::render},
        {"output_transform", FramePlanNodeKind::output_transform},
        {"debug_draw", FramePlanNodeKind::render},
        {"gizmo", FramePlanNodeKind::render},
        {"debug_text", FramePlanNodeKind::render},
        {"shadow_depth", FramePlanNodeKind::render},
        {"velocity", FramePlanNodeKind::render},
        {"picking", FramePlanNodeKind::render},
        {"ui", FramePlanNodeKind::render},
        {"canonical_anchor", FramePlanNodeKind::anchor},
        {"snapshot_copy", FramePlanNodeKind::snapshot_copy},
        {"compute_task", FramePlanNodeKind::compute},
        {"ray_task", FramePlanNodeKind::compute},
    };
    plan.nodes.reserve(std::size(entries));
    plan.levels.reserve(std::size(entries));
    for (std::size_t index = 0; index < std::size(entries); ++index) {
        plan.nodes.push_back(FramePlanNode{
            .name = entries[index].name,
            .kind = entries[index].kind,
            .declaration_index = index,
            .order = index,
            .level = index,
        });
        plan.levels.push_back({entries[index].name});
    }
    return plan;
}

void eraseRequiredObjectPointer(Json &document,
                                const std::string &pointer) {
    const auto separator = pointer.rfind('/');
    REQUIRE(separator != std::string::npos);
    const auto parent_pointer = pointer.substr(0, separator);
    const auto field = pointer.substr(separator + 1);
    auto &parent = document.at(Json::json_pointer{parent_pointer});
    CAPTURE(pointer);
    REQUIRE(parent.is_object());
    REQUIRE(parent.erase(field) == 1);
}

} // namespace

TEST_CASE("WP354 publishes the complete 14 pass family plus compute task contract table",
          "[wp354][shader-resolution]") {
    const auto contracts = shaderResolutionFamilyContracts();
    REQUIRE(contracts.size() == 15);
    std::set<std::string> families;
    for (const auto &contract : contracts) {
        INFO("family=" << std::string{contract.family}
                       << " state=" << std::string{contract.state}
                       << " owner=" << std::string{contract.owner});
        REQUIRE(families.insert(std::string{contract.family}).second);
        REQUIRE((contract.state == "resolved" ||
                 contract.state == "material_owned" ||
                 contract.state == "not_applicable"));
    }
    REQUIRE(families == std::set<std::string>{
                            "material", "fullscreen", "raster",
                            "output_transform", "debug_draw", "gizmo",
                            "debug_text", "shadow_depth", "velocity",
                            "picking", "ui", "imgui",
                            "canonical_anchor", "snapshot_copy",
                            "compute_task"});
    const std::map<std::string, std::pair<std::string, std::vector<std::string>>>
        expected{
            {"material", {"material_owned", {}}},
            {"fullscreen", {"resolved", {"vertex", "fragment"}}},
            {"raster", {"resolved", {"vertex", "fragment?"}}},
            {"output_transform", {"resolved", {"vertex", "fragment"}}},
            {"debug_draw", {"resolved", {"vertex", "fragment"}}},
            {"gizmo", {"resolved", {"vertex", "fragment"}}},
            {"debug_text", {"resolved", {"vertex", "fragment"}}},
            {"shadow_depth", {"resolved", {"vertex"}}},
            {"velocity", {"resolved", {"vertex", "skinned_vertex", "fragment"}}},
            {"picking", {"resolved", {"vertex", "skinned_vertex", "fragment"}}},
            {"ui", {"resolved", {"vertex", "fragment"}}},
            {"imgui", {"resolved", {"vertex", "fragment"}}},
            {"canonical_anchor", {"not_applicable", {}}},
            {"snapshot_copy", {"not_applicable", {}}},
            {"compute_task", {"resolved", {"compute", "raygen", "miss[index]*", "closesthit[index]*"}}},
        };
    for (const auto &contract : contracts) {
        const auto found = expected.find(std::string{contract.family});
        REQUIRE(found != expected.end());
        REQUIRE(std::string{contract.state} == found->second.first);
        REQUIRE(std::vector<std::string>{
                    contract.stages.begin(), contract.stages.end()} ==
                found->second.second);
    }
}

TEST_CASE("WP354 shipping shader families project their concrete effective references",
          "[wp354][shader-resolution][shipping]") {
    const auto source_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto sprite = readJson(
        source_root / "projects" / "sprite_demo" / "passes" /
        "main.json");
    const auto &sprite_passes =
        sprite.at("rendering_passes").at(0).at("passes");
    const auto raster_json = std::find_if(
        sprite_passes.begin(), sprite_passes.end(),
        [](const auto &pass) {
            return pass.value("name", std::string{}) == "ssao_clear";
        });
    REQUIRE(raster_json != sprite_passes.end());
    const RenderTargetNameResolver raster_names{
        [](const std::string &name) {
            return name == "ssao_blur" ? GlobalRenderTargetId{0}
                                        : noRenderTargetId();
        }};
    const RenderTargetMetadataResolver raster_metadata{
        [](GlobalRenderTargetId id) -> RenderTargetMetadata {
            if (id != GlobalRenderTargetId{0}) {
                throw std::runtime_error("unexpected sprite target");
            }
            return RenderTargetMetadata{
                "ssao_blur",
                vk::ImageUsageFlagBits::eColorAttachment |
                    vk::ImageUsageFlagBits::eSampled,
                vk::Format::eR8Unorm,
                vk::Extent2D{128, 128}};
        }};
    auto raster = parsePassDefinitionFromJson(
        *raster_json, raster_names, raster_metadata);

    const auto velocity_feature = readJson(
        source_root / "src" / "core" / "resources" / "features" /
        "velocity.json");
    const auto &velocity_json =
        velocity_feature.at("passes").at(0).at("pass");
    const RenderTargetNameResolver velocity_names{
        [](const std::string &name) {
            if (name == "velocity") return GlobalRenderTargetId{0};
            if (name == "velocity_depth") return GlobalRenderTargetId{1};
            return noRenderTargetId();
        }};
    const RenderTargetMetadataResolver velocity_metadata{
        [](GlobalRenderTargetId id) -> RenderTargetMetadata {
            if (id == GlobalRenderTargetId{0}) {
                return RenderTargetMetadata{
                    "velocity",
                    vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR16G16Sfloat,
                    vk::Extent2D{128, 128}};
            }
            if (id == GlobalRenderTargetId{1}) {
                return RenderTargetMetadata{
                    "velocity_depth",
                    vk::ImageUsageFlagBits::eDepthStencilAttachment,
                    vk::Format::eD32Sfloat,
                    vk::Extent2D{128, 128}};
            }
            throw std::runtime_error("unexpected velocity target");
        }};
    auto velocity = parsePassDefinitionFromJson(
        velocity_json, velocity_names, velocity_metadata);

    const auto clustered = readJson(
        source_root / "src" / "core" / "resources" / "features" /
        "clustered_lighting.json");
    auto compute_tasks =
        parseComputeTaskDefinitionsFromConfigJson(clustered);
    const auto clustered_task = std::find_if(
        compute_tasks.begin(), compute_tasks.end(),
        [](const auto &task) {
            return task.name == "clustered_light_select";
        });
    REQUIRE(clustered_task != compute_tasks.end());

    auto rt_feature = readJson(
        source_root / "src" / "core" / "resources" / "features" /
        "rt_shadow_mask_pipeline.json");
    auto &ray_pipeline =
        rt_feature["compute_tasks"][0]["ray_tracing"];
    ray_pipeline["miss"] = nlohmann::json::array({
        ray_pipeline.at("miss"), ray_pipeline.at("miss")});
    auto ray_tasks =
        parseComputeTaskDefinitionsFromConfigJson(rt_feature);
    REQUIRE(ray_tasks.size() == 1);

    CompiledRenderingPass compiled;
    compiled.name = "wp354_shipping";
    compiled.passes.push_back(CompiledPass{
        .definition = std::move(raster), .pass_id = PassId{0}});
    compiled.passes.push_back(CompiledPass{
        .definition = std::move(velocity), .pass_id = PassId{1}});
    compiled.compute_tasks.push_back(CompiledComputeTask{
        .definition = *clustered_task, .task_id = ComputeTaskId{0}});
    compiled.compute_tasks.push_back(CompiledComputeTask{
        .definition = std::move(ray_tasks.front()),
        .task_id = ComputeTaskId{1}});

    Json plan{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"graph", "wp354_shipping"},
        {"nodes",
         Json::array({
             {{"name", "ssao_clear"}, {"kind", "render"}},
             {{"name", "velocity_pass"}, {"kind", "render"}},
             {{"name", "clustered_light_select"}, {"kind", "compute"}},
             {{"name", "rt_shadow_mask_pipeline"}, {"kind", "compute"}},
         })},
    };
    PathResolver resolver;
    resolver.setup(
        source_root / "projects" / "sprite_demo", false);
    appendShaderResolution(plan, compiled, resolver);

    const auto &raster_stages =
        node(plan, "ssao_clear").at("shader_resolution").at("stages");
    REQUIRE(raster_stages.at(0).at("effective_ref") ==
            "shaders/fullscreen");
    REQUIRE(raster_stages.at(0).at("source_open_ref") ==
            "project://shaders/fullscreen.vert");
    REQUIRE(raster_stages.at(1).at("effective_ref") ==
            "shaders/white");
    REQUIRE(raster_stages.at(1).at("source_open_ref") ==
            "project://shaders/white.frag");

    const auto &velocity_stages =
        node(plan, "velocity_pass").at("shader_resolution").at("stages");
    REQUIRE(stageNames(
                node(plan, "velocity_pass").at("shader_resolution")) ==
            std::vector<std::string>{
                "vertex", "skinned_vertex", "fragment"});
    REQUIRE(velocity_stages.at(0).at("effective_ref") ==
            "engine://velocity");
    REQUIRE(velocity_stages.at(1).at("effective_ref") ==
            "engine://velocity_skinned");
    REQUIRE(velocity_stages.at(2).at("effective_ref") ==
            "engine://velocity");

    const auto &compute_stage =
        node(plan, "clustered_light_select")
            .at("shader_resolution").at("stages").at(0);
    REQUIRE(compute_stage.at("stage") == "compute");
    REQUIRE(compute_stage.at("effective_ref") ==
            "engine://shaders/compute/clustered_light_select");

    const auto &ray_stages =
        node(plan, "rt_shadow_mask_pipeline")
            .at("shader_resolution").at("stages");
    REQUIRE(stageNames(
                node(plan, "rt_shadow_mask_pipeline")
                    .at("shader_resolution")) ==
            std::vector<std::string>{
                "raygen", "miss", "miss", "closesthit"});
    REQUIRE(ray_stages.at(1).at("index") == 0);
    REQUIRE(ray_stages.at(2).at("index") == 1);
    REQUIRE(ray_stages.at(1).at("declared_ref") ==
            "engine://rt_shadow_mask_pipeline");
    REQUIRE(ray_stages.at(2).at("declared_ref") ==
            "engine://rt_shadow_mask_pipeline");
}

TEST_CASE("renderer-owned WP354 append covers every family and stripWp354 changes only node shader fields",
          "[wp354][shader-resolution][fixture][production-seam]") {
    const auto source_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto baseline = readJson(
        source_root / "test" / "fixtures" / "wp354" /
        "runtime_projection.pre_wp354.json");
    REQUIRE_FALSE(baseline.contains("profile"));
    for (const auto &entry : baseline.at("nodes")) {
        CAPTURE(entry.at("name"));
        REQUIRE_FALSE(entry.contains("shader_resolution"));
    }

    CompiledRenderingPass compiled;
    compiled.name = "wp354_family_contract";
    addPass(compiled, "material", MaterialPassInfo{});
    addPass(
        compiled, "fullscreen",
        FullscreenPassInfo{
            .vert_shader = shader("engine://fullscreen", ShaderStage::vertex),
            .frag_shader = shader("engine://fullscreen", ShaderStage::fragment),
        },
        {declared(DeclaredShaderStage::vertex, "authored/fullscreen"),
         declared(DeclaredShaderStage::fragment, "authored/fullscreen")},
        PassImplementationSelection{
            .provider = "wp354.fixture.provider",
            .implementation = "fixture.fullscreen@1",
            .provider_owner = 77,
        });
    addPass(
        compiled, "raster",
        GenericRasterPassInfo{
            .vert_shader = shader("shaders/fullscreen", ShaderStage::vertex),
            .frag_shader = shader("shaders/white", ShaderStage::fragment),
        },
        {declared(DeclaredShaderStage::vertex, "shaders/fullscreen"),
         declared(DeclaredShaderStage::fragment, "shaders/white")});
    addPass(
        compiled, "output_transform",
        FullscreenPassInfo{
            .vert_shader = shader("engine://fullscreen", ShaderStage::vertex),
            .frag_shader = shader("engine://output_transform", ShaderStage::fragment),
        },
        {declared(DeclaredShaderStage::vertex, "engine://fullscreen"),
         declared(DeclaredShaderStage::fragment,
                  "engine://output_transform")});
    addPass(
        compiled, "debug_draw",
        DebugDrawPassInfo{
            shader("engine://debug_draw", ShaderStage::vertex),
            shader("engine://debug_draw", ShaderStage::fragment)},
        {declared(DeclaredShaderStage::vertex, "engine://debug_draw"),
         declared(DeclaredShaderStage::fragment, "engine://debug_draw")});
    addPass(
        compiled, "gizmo",
        GizmoPassInfo{
            shader("engine://gizmo", ShaderStage::vertex),
            shader("engine://gizmo", ShaderStage::fragment)},
        {declared(DeclaredShaderStage::vertex, "engine://gizmo"),
         declared(DeclaredShaderStage::fragment, "engine://gizmo")});
    addPass(
        compiled, "debug_text",
        DebugTextPassInfo{
            shader("engine://debug_text", ShaderStage::vertex),
            shader("engine://debug_text", ShaderStage::fragment)},
        {declared(DeclaredShaderStage::vertex, "engine://debug_text"),
         declared(DeclaredShaderStage::fragment, "engine://debug_text")});
    addPass(
        compiled, "shadow_depth",
        ShadowDepthPassInfo{
            shader("engine://shadow_depth", ShaderStage::vertex)});
    addPass(
        compiled, "velocity",
        VelocityPassInfo{
            shader("engine://velocity", ShaderStage::vertex),
            shader("engine://velocity_skinned", ShaderStage::vertex),
            shader("engine://velocity", ShaderStage::fragment)},
        {declared(DeclaredShaderStage::vertex, "engine://velocity"),
         declared(DeclaredShaderStage::skinned_vertex,
                  "engine://velocity_skinned"),
         declared(DeclaredShaderStage::fragment, "engine://velocity")});
    addPass(
        compiled, "picking",
        PickingPassInfo{
            shader("engine://picking", ShaderStage::vertex),
            shader("engine://picking_skinned", ShaderStage::vertex),
            shader("engine://picking", ShaderStage::fragment)},
        {declared(DeclaredShaderStage::vertex, "engine://picking"),
         declared(DeclaredShaderStage::skinned_vertex,
                  "engine://picking_skinned"),
         declared(DeclaredShaderStage::fragment, "engine://picking")});
    addPass(compiled, "ui", UiPassInfo{});

    ComputeTaskDefinition compute;
    compute.name = "compute_task";
    compute.shader = shader(
        "engine://shaders/compute/clustered_light_select",
        ShaderStage::compute);
    compute.declared_shader_refs = {
        declared(DeclaredShaderStage::compute,
                 "engine://shaders/compute/clustered_light_select")};
    compiled.compute_tasks.push_back(CompiledComputeTask{
        .definition = std::move(compute),
        .task_id = ComputeTaskId{0},
    });

    ComputeTaskDefinition ray;
    ray.name = "ray_task";
    ray.ray_tracing = RayTracingTaskShaderDefinition{
        .raygen = shader("engine://rt_shadow_mask_pipeline",
                         ShaderStage::raygen),
        .misses = {
            shader("engine://rt_shadow_mask_pipeline", ShaderStage::miss),
            shader("engine://rt_shadow_mask_pipeline", ShaderStage::miss)},
        .closest_hits = {
            shader("engine://rt_shadow_mask_pipeline",
                   ShaderStage::closesthit)},
    };
    ray.declared_shader_refs = {
        declared(DeclaredShaderStage::raygen,
                 "engine://rt_shadow_mask_pipeline"),
        declared(DeclaredShaderStage::miss,
                 "engine://rt_shadow_mask_pipeline", 0),
        declared(DeclaredShaderStage::miss,
                 "engine://rt_shadow_mask_pipeline", 1),
        declared(DeclaredShaderStage::closesthit,
                 "engine://rt_shadow_mask_pipeline", 0),
    };
    compiled.compute_tasks.push_back(CompiledComputeTask{
        .definition = std::move(ray),
        .task_id = ComputeTaskId{1},
    });

    constexpr RenderingPassId pass_id{41};
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(
        source_root / "projects" / "sprite_demo", false);
    GET_MODULE(FrameGraphRuntimeContainer)
        .registerExecutionPlan(
            pass_id, compiled, familyFramePlan(),
            std::make_shared<CompiledRenderPipeline>());
    Renderer renderer{RendererFramePlanCpuSeam{}, pass_id};
    const auto current = renderer.currentFramePlanJson();

    REQUIRE(current.at("profile") == "runtime");
    REQUIRE(current.at("nodes").size() ==
            baseline.at("nodes").size());
    for (const auto &entry : current.at("nodes")) {
        CAPTURE(entry.at("name"));
        REQUIRE(entry.contains("shader_resolution"));
    }
    REQUIRE(node(current, "material").at("shader_resolution") ==
            Json{{"state", "material_owned"}});
    REQUIRE(node(current, "canonical_anchor").at("shader_resolution") ==
            Json{{"state", "not_applicable"}});
    REQUIRE(node(current, "snapshot_copy").at("shader_resolution") ==
            Json{{"state", "not_applicable"}});
    for (const auto family : {"ui"}) {
        const auto &fixed =
            node(current, family).at("shader_resolution").at("stages");
        REQUIRE(stageNames(
                    node(current, family).at("shader_resolution")) ==
                std::vector<std::string>{"vertex", "fragment"});
        for (const auto &stage : fixed) {
            REQUIRE(stage.at("origin") == "generated");
            REQUIRE(stage.at("source_open_ref").is_null());
            REQUIRE(stage.at("source_open_reason") ==
                    "embedded_engine_resource");
            REQUIRE_FALSE(stage.contains("declared_ref"));
        }
    }
    REQUIRE(stageNames(node(current, "velocity").at("shader_resolution")) ==
            std::vector<std::string>{
                "vertex", "skinned_vertex", "fragment"});
    REQUIRE(stageNames(node(current, "ray_task").at("shader_resolution")) ==
            std::vector<std::string>{
                "raygen", "miss", "miss", "closesthit"});
    const auto &ray_stages =
        node(current, "ray_task").at("shader_resolution").at("stages");
    REQUIRE(ray_stages.at(1).at("index") == 0);
    REQUIRE(ray_stages.at(2).at("index") == 1);
    REQUIRE(ray_stages.at(3).at("index") == 0);

    const auto &provider =
        node(current, "fullscreen").at("shader_resolution").at("stages");
    REQUIRE(provider.at(0).at("origin") == "provider");
    REQUIRE(provider.at(1).at("origin") == "provider");
    REQUIRE(provider.at(0).at("declared_ref") ==
            "authored/fullscreen");
    REQUIRE(provider.at(0).at("effective_ref") ==
            "engine://fullscreen");
    const auto &shadow =
        node(current, "shadow_depth")
            .at("shader_resolution")
            .at("stages")
            .at(0);
    REQUIRE_FALSE(shadow.contains("declared_ref"));
    REQUIRE(shadow.at("effective_ref") == "engine://shadow_depth");
    REQUIRE(shadow.at("origin") == "engine_default");
    REQUIRE(node(current, "raster")
                .at("shader_resolution")
                .at("stages")
                .at(0)
                .at("source_open_ref") ==
            "project://shaders/fullscreen.vert");
    REQUIRE(node(current, "compute_task")
                .at("shader_resolution")
                .at("stages")
                .at(0)
                .at("effective_ref") ==
            "engine://shaders/compute/clustered_light_select");

    std::vector<std::string> permitted_projection_pointers{
        "/profile"};
    for (std::size_t index = 0;
         index < current.at("nodes").size(); ++index) {
        permitted_projection_pointers.push_back(
            "/nodes/" + std::to_string(index) +
            "/shader_resolution");
    }
    auto stripped = current;
    for (const auto &pointer : permitted_projection_pointers) {
        eraseRequiredObjectPointer(stripped, pointer);
    }
    const auto recursive_diff = Json::diff(baseline, stripped);
    INFO("unexpected recursive diff: " << recursive_diff.dump(2));
    REQUIRE(recursive_diff.empty());
}

TEST_CASE("non-null planner overload stays exact and shader-free before renderer enrichment",
          "[wp354][shader-resolution][planner-negative-control][fixture]") {
    const auto source_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto expected = readJson(
        source_root / "test" / "fixtures" / "wp354" /
        "planner_non_null.json");

    FramePlan raw;
    raw.name = "wp354_non_null_planner";
    raw.nodes.push_back(FramePlanNode{
        .name = "raw_pass",
        .kind = FramePlanNodeKind::render,
    });
    raw.levels = {{"raw_pass"}};
    CompiledRenderPipeline render_pipeline;
    auto wire = framePlanToJson(raw, &render_pipeline);

    REQUIRE(wire == expected);
    REQUIRE_FALSE(wire.contains("profile"));
    REQUIRE_FALSE(
        wire.at("nodes").at(0).contains("shader_resolution"));

    CompiledRenderingPass compiled;
    addPass(compiled, "raw_pass", MaterialPassInfo{});
    PathResolver resolver;
    resolver.setup(
        source_root / "projects" / "sprite_demo", false);
    appendShaderResolution(wire, compiled, resolver);
    REQUIRE(wire.at("nodes").at(0).at("shader_resolution") ==
            Json{{"state", "material_owned"}});
}

TEST_CASE("provider projection permits a missing authored declaration",
          "[wp354][shader-resolution][provider][origin-iff]") {
    CompiledRenderingPass compiled;
    compiled.name = "wp354_provider_without_declaration";
    addPass(
        compiled, "provided",
        FullscreenPassInfo{
            .vert_shader = shader(
                "engine://fullscreen", ShaderStage::vertex),
            .frag_shader = shader(
                "engine://scene_present", ShaderStage::fragment),
        },
        {},
        PassImplementationSelection{
            .provider = "fixture.provider.no_declaration",
            .implementation = "fixture.provider.no_declaration@1",
            .provider_owner = 77,
        });

    Json plan{
        {"schema", "pelican.frame_plan"},
        {"version", 1},
        {"graph", "wp354_provider_without_declaration"},
        {"nodes",
         Json::array({
             {{"name", "provided"}, {"kind", "render"}},
         })},
    };
    PathResolver resolver;
    resolver.setup(
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR}, false);
    appendShaderResolution(plan, compiled, resolver);

    const auto &stages =
        plan.at("nodes").at(0)
            .at("shader_resolution").at("stages");
    REQUIRE(stages.size() == 2);
    for (const auto &stage : stages) {
        REQUIRE(stage.at("origin") == "provider");
        REQUIRE_FALSE(stage.contains("declared_ref"));
    }
}

TEST_CASE("raw framePlanToJson remains profile-less and shader-free",
          "[wp354][shader-resolution][planner-negative-control]") {
    FramePlan raw;
    raw.name = "wp354_raw_control";
    raw.nodes.push_back(FramePlanNode{
        .name = "raw_pass",
        .kind = FramePlanNodeKind::render,
    });
    raw.levels = {{"raw_pass"}};
    const auto wire = framePlanToJson(raw);
    REQUIRE_FALSE(wire.contains("profile"));
    REQUIRE_FALSE(wire.at("nodes").at(0).contains("shader_resolution"));
}

TEST_CASE("WP354 compiler-off keeps logical source projection while naming the missing SPIR-V candidate",
          "[wp354][shader-resolution][compiler-off]") {
    const auto source_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    PathResolver resolver;
    resolver.setup(
        source_root / "projects" / "sprite_demo", false);
    const auto source = resolver.resolveShaderSourceOpenReference(
        "shaders/fullscreen", ShaderSourceStage::vertex);
    REQUIRE(source.logical_ref ==
            std::optional<std::string>{
                "project://shaders/fullscreen.vert"});
#if !PELICAN_RUNTIME_SHADER_COMPILER
    ShaderLibrary library{
        ShaderLibraryModuleMode::reflection_only};
    REQUIRE_THROWS_WITH(
        library.loadFromReference(
            makeShaderReference(
                "shaders/fullscreen", ShaderStage::vertex),
            resolver, true),
        Catch::Matchers::ContainsSubstring(
            "Shader stem could not be resolved: shaders/fullscreen (vertex)") &&
            Catch::Matchers::ContainsSubstring(
                "shaders/fullscreen.vert.spv"));
#else
    REQUIRE(shaderSourceCandidateReference(
                "shaders/fullscreen", ShaderSourceStage::vertex, false) ==
            "shaders/fullscreen.vert");
#endif
}

TEST_CASE("typed parsers capture declared stage presence before provider replacement",
          "[wp354][shader-resolution][typed-parser]") {
    const RenderTargetNameResolver names{
        [](const std::string &name) {
            if (name == "scene_color") return GlobalRenderTargetId{0};
            if (name == "scene_depth") return GlobalRenderTargetId{1};
            return noRenderTargetId();
        }};
    const RenderTargetMetadataResolver metadata{
        [](GlobalRenderTargetId id) -> RenderTargetMetadata {
            if (id == GlobalRenderTargetId{0}) {
                return RenderTargetMetadata{
                    "scene_color",
                    vk::ImageUsageFlagBits::eColorAttachment,
                    vk::Format::eR8G8B8A8Unorm,
                    vk::Extent2D{128, 128}};
            }
            if (id == GlobalRenderTargetId{1}) {
                return RenderTargetMetadata{
                    "scene_depth",
                    vk::ImageUsageFlagBits::eDepthStencilAttachment,
                    vk::Format::eD32Sfloat,
                    vk::Extent2D{128, 128}};
            }
            throw std::runtime_error("unexpected metadata lookup");
        }};
    const auto pass = parsePassDefinitionFromJson(
        Json{
            {"name", "provided"},
            {"type", "fullscreen"},
            {"output", {{"color", "scene_color"}, {"depth", nullptr}}},
            {"shader",
             {{"vertex", "project://declared"},
              {"fragment", "project://declared"}}},
            {"implementation", {{"provider", "fixture.provider"}}},
        },
        names, metadata);
    REQUIRE(pass.declared_shader_refs ==
            std::vector<DeclaredShaderReference>{
                declared(DeclaredShaderStage::vertex,
                         "project://declared"),
                declared(DeclaredShaderStage::fragment,
                         "project://declared")});

    const auto shadow = parsePassDefinitionFromJson(
        Json{
            {"name", "shadow"},
            {"type", "shadow_depth"},
            {"output", {{"color", nullptr}, {"depth", "scene_depth"}}},
        },
        names, metadata);
    REQUIRE(shadow.declared_shader_refs.empty());

    const auto tasks = parseComputeTaskDefinitionsFromConfigJson(Json{
        {"compute_tasks",
         {{{"name", "ray"},
           {"ray_tracing",
            {{"raygen", "r"},
             {"miss", {"m0", "m1"}},
             {"closesthit", {"h0", "h1"}}}},
           {"writes", {"mask"}},
           {"resource_ports",
            {{"mask", {{"resource", "mask"}, {"access", "storage"}}}}},
           {"dispatch", {{"rays_from", {{"port", "mask"}}}}}}}},
    });
    REQUIRE(tasks.size() == 1);
    REQUIRE(tasks.front().declared_shader_refs.at(1).index == 0);
    REQUIRE(tasks.front().declared_shader_refs.at(2).index == 1);
    REQUIRE(tasks.front().declared_shader_refs.at(3).index == 0);
    REQUIRE(tasks.front().declared_shader_refs.at(4).index == 1);
}

} // namespace Pelican
