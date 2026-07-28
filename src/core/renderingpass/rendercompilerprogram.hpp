#pragma once

#include "framegraphbufferdefinition.hpp"
#include "frameplanner.hpp"
#include "../../project/renderpipeline.hpp"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican {

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
};

struct RenderCompilerProgramVariantRequest {
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
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
    bool runtime_shader_compiler_enabled = false;
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
};

struct RenderCompilerProgramVariantOutput {
    RenderPipelineGraphVariant graph_variant =
        RenderPipelineGraphVariant::flat;
    std::shared_ptr<const CompiledRenderPipeline>
        compiled_pipeline;
    nlohmann::json normalized_config;
    std::vector<FrameGraphBufferDefinition>
        buffer_definitions;
    std::unordered_set<std::string> buffer_names;
    std::vector<ComputeTaskDefinition>
        compute_task_definitions;
    std::unordered_map<std::string, FramePlan>
        frame_plans;
    std::unique_ptr<
        RenderCompilerBackendPhysicalPackage>
        physical_package;
};

struct RenderCompilerProgramOutput {
    std::vector<RenderCompilerProgramVariantOutput>
        variants;
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

// Runs one program, verifies the backend package as a complete immutable CPU
// candidate, and stamps trusted program provenance before any GPU mutation.
RenderCompilerProgramOutput runRenderCompilerProgram(
    const RenderCompilerProgram &program,
    const RenderCompilerProgramInput &input);

} // namespace Pelican
