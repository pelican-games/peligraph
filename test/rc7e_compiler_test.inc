#include "rc7e_test_support.hpp"
#include "../src/core/renderingpass/rendertargetdefinition.hpp"
#include "../src/project/targetrendermemory.hpp"
namespace Pelican {
namespace {
RenderCompilerProgramResult rc7eCompile(const TestSupport::Rc7eOptions &options = {}, bool flat_only = false) {
    ProgramInputFixture fixture;
    fixture.useAuthoredConfig(nlohmann::json::object(), "rc7e");
    fixture.config = TestSupport::renderGraphRoot(TestSupport::rc7eRenderingSection(fixture.task_project, {}, options));
    fixture.backend_context = VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb, {64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    fixture.backend_context.planning_facts = TestSupport::rc7cDeviceFacts();
    // XR first: R1, which only the flat variant uses, is in a later
    // variant's plan.
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::xr}, {.graph_variant = RenderPipelineGraphVariant::flat}};
    if (flat_only) fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    return runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
}
// Each variant's planned images, by graph.
std::vector<std::map<std::string, std::set<std::string>>> rc7ePlanned(const RenderCompilerProgramResult &output) {
    std::vector<std::map<std::string, std::set<std::string>>> result;
    for (const auto &variant : output.variants) {
        auto &graphs = result.emplace_back();
        for (const auto &plan : requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package).target_plan_compilation.plans) {
            auto &names = graphs[plan->graph];
            for (const auto &resource : plan->resources)
                if (vulkanPhysicalResourceIsPlannedImage(resource)) names.insert(resource.logical_resource);
        }
    }
    return result;
}
}

TEST_CASE("RC-7e E1 the images no runtime plan uses are not allocated", "[rendercompiler][rc7e][rc7e-e1]") {
#if PELICAN_WITH_OPENXR
    const auto output = rc7eCompile();
    REQUIRE(output.variants.size() == 2);
    const auto planned = rc7ePlanned(output);
    // Facts first. One graph a variant (main_render#xr, main_render).
    REQUIRE(planned[0].size() == 1);
    REQUIRE(planned[1].size() == 1);
    const auto in = [](const std::map<std::string, std::set<std::string>> &graphs, const std::string &name) {
        std::size_t count = 0;
        for (const auto &[graph, names] : graphs) count += names.contains(name);
        return count;
    };
    for (const auto &graphs : planned) {
        CHECK(in(graphs, "R0") == 0);
        CHECK(in(graphs, "H") == 0);
        // The canonical pipeline's output_transform reads display in every
        // graph: no runtime plan can leave it unused.
        CHECK(in(graphs, "display") == graphs.size());
        CHECK(in(graphs, "A") == 1);
    }
    CHECK(in(planned[0], "R1") == 0);
    CHECK(in(planned[1], "R1") == 1);
    for (const auto *name : {"A", "R0", "R1", "H", "display"}) {
        CAPTURE(name);
        REQUIRE(output.generation_resources.findImage(name) != nullptr);
    }
    const auto held = engineHeldRenderTargetNames();
    CHECK(held == std::vector<std::string>{"display"});
    const auto plans = vulkanRuntimeVariantTargetPlans(output.variants);
    const auto unallocated = unallocatedRenderTargets(output.generation_resources, plans, held);
    CHECK(unallocated == std::vector<UnallocatedRenderTarget>{
        {"H", "unused_in_generation", 2u * 64u * 64u * 4u},
        {"R0", "unused_in_generation", 64u * 64u * 4u},
    });
    // display is held even where no plan uses it.
    std::vector<std::string> without_plans;
    for (const auto &target : unallocatedRenderTargets(output.generation_resources, {}, held))
        without_plans.push_back(target.name);
    CHECK(without_plans == std::vector<std::string>{"A", "H", "R0", "R1"});
    // Every plan counts, not the first one only.
    CHECK(unallocatedRenderTargets(output.generation_resources, std::span{plans}.first(1), held).size() == 3);

    // A declaration error registration alone checks (a history target
    // without SAMPLED) is refused for an unallocated target with the
    // registered one's message.
    const auto refused = rc7eCompile({.history_usage = nlohmann::json::array({"COLOR_ATTACHMENT"})});
    const auto refused_unallocated = unallocatedRenderTargets(refused.generation_resources,
        vulkanRuntimeVariantTargetPlans(refused.variants), held);
    REQUIRE(std::any_of(refused_unallocated.begin(), refused_unallocated.end(), [](const auto &t) { return t.name == "H"; }));
    const auto *history = refused.generation_resources.findImage("H");
    REQUIRE(history);
    CHECK_THROWS_WITH(validateRenderTargetDefinition(renderTargetDefinitionOf(*history), {64, 64}, false),
                      "History render target requires SAMPLED usage: H");
    CHECK_NOTHROW(validateRenderTargetDefinition(renderTargetDefinitionOf(*output.generation_resources.findImage("H")), {64, 64}, false));
#else
    SKIP("XR variant compiler unavailable");
#endif
}

TEST_CASE("RC-7e E1 a target only a view-family provider looks up is allocated", "[rendercompiler][rc7e][rc7e-e1]") {
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    const auto output = rc7eCompile({.cube_capture = true}, true);
    REQUIRE(output.variants.size() == 1);
    const auto planned = rc7ePlanned(output);
    // Facts first: a $capture/cube node writes cube_face; no node of any
    // graph uses cube_capture_color, which the standard cube capture
    // provider looks up by name.
    REQUIRE(planned[0].size() == 1);
    for (const auto &[graph, names] : planned[0]) {
        CHECK(names.contains("cube_face"));
        CHECK_FALSE(names.contains("cube_capture_color"));
    }
    REQUIRE(output.generation_resources.findImage("cube_capture_color") != nullptr);
    const auto names = [](const std::vector<UnallocatedRenderTarget> &targets) {
        std::vector<std::string> result;
        for (const auto &target : targets) result.push_back(target.name);
        return result;
    };
    const auto held = engineHeldRenderTargetNames();
    const auto plans = vulkanRuntimeVariantTargetPlans(output.variants);
    const auto engine_uses = vulkanRuntimeVariantEngineRenderTargetUses(output.variants);
    CHECK(engine_uses == std::vector<std::string>{"cube_capture_color"});
    // The provider's target is in U: registration allocates it.
    CHECK(names(unallocatedRenderTargets(output.generation_resources, plans, held, engine_uses)) ==
          std::vector<std::string>{"H", "R0"});
    // The plans alone would leave it out.
    CHECK(names(unallocatedRenderTargets(output.generation_resources, plans, held)) ==
          std::vector<std::string>{"H", "R0", "cube_capture_color"});
    // Without the feature the provider names nothing.
    CHECK(vulkanRuntimeVariantEngineRenderTargetUses(rc7eCompile({}, true).variants).empty());
#else
    SKIP("standard render algorithms unavailable");
#endif
}

}
