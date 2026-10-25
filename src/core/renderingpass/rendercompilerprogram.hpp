#pragma once

#include "../../project/framegraphbufferdefinition.hpp"
#include "../../project/generationresources.hpp"
#include "frameplanner.hpp"
#include "../../project/rendergraphir.hpp"
#include "../../project/renderpipeline.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <set>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican {

struct RayTracingProjectConfig;

class GraphTransformRegistrySnapshot;
class PathResolver;
class RenderStrategyRegistrySnapshot;
class SubgraphReplacementRegistrySnapshot;

// The common compiler-program interface does not carry a closed GPU API enum.
// A backend contributes a typed context beside this open identity.
class RenderCompilerBackendContext {
  public:
    virtual ~RenderCompilerBackendContext() = default;
    virtual std::string_view backend() const noexcept = 0;
    // RC-3c2a: the output's extent the generation's images resolve at.
    virtual ResolvedResourceExtent outputExtent() const noexcept = 0;
};

// Physical packages are an open backend boundary. A future Metal package can
// derive from this class without adding a fake Metal alternative to a central
// RHI variant.
class RenderCompilerBackendPhysicalPackage {
  public:
    virtual ~RenderCompilerBackendPhysicalPackage() = default;
    virtual std::string_view backend() const noexcept = 0;
    // Verifies backend-owned graph coverage and internal indices. The common
    // host supplies only graph identities, not a lowest-common-denominator
    // physical schema.
    virtual void validate(
        std::span<const std::string>
            frame_graph_names) const = 0;
    // RC-3c2a: what the physical plan placed of each config image of the
    // variant's IR (the generation's resource description merges them).
    virtual std::span<const ImagePlacement> imagePlacements() const noexcept = 0;
};

enum class RenderCompilerProgramArtifact : std::uint8_t {
    // Produces the backend package consumed by the live runtime registration
    // transaction.
    runtime_package,
    // Produces immutable CPU data for a request-local consumer. It joins the
    // same compiler invocation and provider snapshot without entering the
    // shared GPU registration path.
    data_only,
};

struct RenderCompilerProgramVariantRequest {
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
    RenderCompilerProgramArtifact artifact =
        RenderCompilerProgramArtifact::runtime_package;
    bool enable_multiview_runtime = false;
    bool enable_external_depth_export = false;
    // Host-owned logical additions, such as the built-in ImGui graph pass,
    // are applied after authoring resolution and before graph lowering.
    std::function<void(nlohmann::json &)>
        compose_runtime_config;
};

struct RenderCompilerProgramInput {
    const nlohmann::json &rendering_config;
    std::string source_name =
        "rendering pass registration";
    const PathResolver &path_resolver;
    // Request-local authoring overlay. When absent, compilation reads the
    // production PathResolver directly. Feature and preset resolution must
    // use this same loader.
    std::function<std::string(std::string_view)> load_document;
    bool runtime_shader_compiler_enabled = false;
    std::shared_ptr<const RayTracingProjectConfig>
        ray_tracing_project;
    const GraphTransformRegistrySnapshot
        &graph_transforms;
    const RenderStrategyRegistrySnapshot
        &render_strategies;
    const SubgraphReplacementRegistrySnapshot
        &subgraph_replacements;
    const RenderCompilerBackendContext
        &backend_context;
    std::span<const RenderCompilerProgramVariantRequest>
        variants;
    std::function<void(std::string_view, const nlohmann::json &)>
        validate_feature;
    // Runtime owner availability; never part of the authored JSON document.
    std::set<std::string> inactive_graph_scopes;
    // RC-3c2a: receives every image the completion's resource checks read
    // (validateRenderGraphResources' observer), with the variant's name.
    std::function<void(std::string_view, const ResourceCheckRead &)>
        observe_resource_checks;
};

struct RenderCompilerProgramVariantOutput {
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
    std::shared_ptr<const CompiledRenderPipeline>
        compiled_pipeline;
    std::shared_ptr<const RayTracingProjectConfig>
        ray_tracing_project;
    std::unordered_map<std::string, FramePlan>
        frame_plans;
    std::unique_ptr<
        RenderCompilerBackendPhysicalPackage>
        physical_package;
    // PV-1: a data_only artifact only. The program could not compose this
    // request-local artifact from an authoring the runtime artifacts of the
    // same invocation accepted; this is why. Everything else is left empty
    // (compiled_pipeline null). The runtime artifacts stand and publish.
    std::exception_ptr unavailable;
    // RC-3c1b: the variant's final IR (a runtime artifact's finalized one,
    // the frame plans and the physical plan were made from; the preview's as
    // parsed). Required for every available artifact; null when unavailable.
    // Runtime graph names exactly match frame_plans' keys. A data-only
    // artifact carries its IR without runtime plans.
    std::shared_ptr<const RenderGraphIR> render_graph;
};

struct RenderCompilerProgramOutput {
    std::vector<RenderCompilerProgramVariantOutput>
        variants;
};

// Only the common host completion produces this result. A compiler program
// returns RenderCompilerProgramOutput and cannot supply registration's
// resource descriptions or execution definitions through its output.
struct RenderCompilerProgramResult {
    std::vector<RenderCompilerProgramVariantOutput> variants;
    // RC-3c2a: the generation's resource description of the runtime
    // variants (variants[i] is the i-th runtime_package variant, in order),
    // made and checked by runRenderCompilerProgram (completeGenerationResources).
    GenerationResources generation_resources;
    // RC-3c2b1: each runtime variant's compute tasks (the same order), made
    // by runRenderCompilerProgram from the variant's final IR
    // (computeTaskDefinitionsOf). A program returns none of its own: what
    // registers is what the IR says.
    std::vector<std::vector<ComputeTaskDefinition>> compute_task_definitions;
};

// One program owns compilation of the complete requested variant family.
// It may call the engine's portable planning helpers, mix portable and
// backend-specific passes, or directly construct a backend-native package.
class RenderCompilerProgram {
  public:
    virtual ~RenderCompilerProgram() = default;

    virtual RenderCompilerProgramSelection selection(
        const RenderCompilerBackendContext
            &backend_context) const = 0;
    virtual RenderCompilerProgramOutput compile(
        const RenderCompilerProgramInput &input) const = 0;
};

// RC-3c2a: the CPU completion every runtime variant of a program's output
// goes through, once, before anything is registered: the generation's
// resource description (resolveGenerationResources over every runtime
// variant's final IR and placements, at `output_extent`), then each
// variant's resource checks on it -- its compute tasks' and GPU draws'
// buffers (validateComputeTaskBufferContracts and
// validateGpuDrawSourceBufferContracts, over the IR) and its passes' images
// (validateRenderGraphResources). Refuses a runtime variant without its IR.
GenerationResources completeGenerationResources(
    std::span<const RenderCompilerProgramVariantOutput> variants,
    ResolvedResourceExtent output_extent,
    const std::function<void(std::string_view, const ResourceCheckRead &)>
        &observe_resource_checks = {});

// Runs one program, verifies the backend package as a complete immutable CPU
// candidate, makes each runtime variant's compute tasks from its final IR
// (computeTaskDefinitionsOf, against input.ray_tracing_project), completes
// its generation's resources (completeGenerationResources), and stamps
// trusted program provenance before any GPU mutation.
RenderCompilerProgramResult runRenderCompilerProgram(
    const RenderCompilerProgram &program,
    const RenderCompilerProgramInput &input);

} // namespace Pelican
