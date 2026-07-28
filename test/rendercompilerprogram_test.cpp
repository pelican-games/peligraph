#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace Pelican {
namespace {

enum class NativeProgramFault {
    none,
    wrong_backend,
    missing_target_index,
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

            variant.frame_plans.emplace(
                "native_graph",
                FramePlan{
                    .name = "native_graph"});
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
