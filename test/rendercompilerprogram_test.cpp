#include "../src/core/loader/pathresolver.hpp"
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

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Pelican {
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
