#include "rendercompilerprogram.hpp"
#include "passdefinitionfromir.hpp"
#include "computetaskdefinition.hpp"
#include "../../project/gpudrawsource.hpp"
#include "../../project/graphcomposition.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

void validateInput(
    const RenderCompilerProgramInput &input) {
    if (input.backend_context.backend().empty()) {
        throw std::runtime_error(
            "Render compiler backend context requires a "
            "non-empty backend identity");
    }
    if (input.variants.empty()) {
        throw std::runtime_error(
            "Render compiler program requires at least "
            "one graph variant");
    }
    std::set<RenderPipelineGraphVariant>
        graph_variants;
    for (const auto &request : input.variants) {
        switch (request.artifact) {
        case RenderCompilerProgramArtifact::
            runtime_package:
        case RenderCompilerProgramArtifact::data_only:
            break;
        default:
            throw std::runtime_error(
                "Render compiler program input has an "
                "unknown artifact kind");
        }
        if (renderPipelineGraphVariantName(
                request.graph_variant) ==
                "unknown" ||
            !graph_variants
                 .insert(request.graph_variant)
                 .second) {
            throw std::runtime_error(
                "Render compiler program input has an "
                "unknown or duplicate graph variant");
        }
    }
}

void validateSelection(
    const RenderCompilerProgramSelection
        &selection,
    const RenderCompilerBackendContext
        &backend_context) {
    if (selection.schema_version != 1) {
        throw std::runtime_error(
            "Render compiler program has unsupported "
            "selection schema_version");
    }
    if (selection.name.empty() ||
        selection.implementation.empty() ||
        selection.backend.empty()) {
        throw std::runtime_error(
            "Render compiler program selection requires "
            "non-empty name, implementation, and backend");
    }
    if (selection.backend !=
        backend_context.backend()) {
        throw std::runtime_error(
            "Render compiler program selection backend '" +
            selection.backend +
            "' does not match runtime backend '" +
            std::string{backend_context.backend()} +
            "'");
    }
    if (renderCompilerProgramModeName(
            selection.mode) == "unknown") {
        throw std::runtime_error(
            "Render compiler program selection has "
            "unknown mode");
    }
}

void validateVariantOutput(
    RenderCompilerProgramVariantOutput &variant,
    const RenderCompilerProgramVariantRequest
        &request,
    const RenderCompilerProgramSelection
        &selection) {
    if (variant.graph_variant !=
        request.graph_variant) {
        throw std::runtime_error(
            "Render compiler program returned variants "
            "out of order");
    }
    // PV-1: an artifact the program could not compose carries nothing else.
    if (variant.unavailable) {
        if (request.artifact !=
            RenderCompilerProgramArtifact::data_only) {
            throw std::runtime_error(
                "Render compiler runtime artifact cannot be "
                "unavailable");
        }
        if (variant.compiled_pipeline != nullptr ||
            variant.physical_package != nullptr ||
            !variant.frame_plans.empty() || variant.render_graph != nullptr) {
            throw std::runtime_error(
                "Render compiler unavailable data-only artifact "
                "returned compiled output");
        }
        return;
    }
    if (variant.compiled_pipeline == nullptr) {
        throw std::runtime_error(
            "Render compiler program returned a null "
            "compiled pipeline");
    }
    if (variant.compiled_pipeline
            ->graph_variant_policy.variant !=
        request.graph_variant) {
        throw std::runtime_error(
            "Render compiler program returned a compiled "
            "pipeline for the wrong graph variant");
    }
    const auto refuse_graph = [&](std::string_view reason, std::string_view graph = {}) {
        nlohmann::json detail{
            {"variant", renderPipelineGraphVariantName(variant.graph_variant)},
            {"reason", reason}};
        if (!graph.empty()) detail["graph"] = graph;
        throw GraphRegistryError{"render_graph.compiler_output_invalid@1", std::move(detail)};
    };
    if (!variant.render_graph) refuse_graph("render_graph_required");
    // The removed buffer-name index also rejected malformed identities.
    // Keep that boundary check on the declarations themselves.
    std::unordered_set<std::string> seen_buffers;
    for (const auto &buffer : variant.render_graph->buffers) {
        const auto invalid = [&](std::string_view reason) {
            throw GraphRegistryError{"render_graph.buffer_invalid@1",
                {{"variant", renderPipelineGraphVariantName(variant.graph_variant)},
                 {"resource", buffer.name}, {"field", "name"}, {"reason", reason}}};
        };
        if (buffer.name.empty()) invalid("required");
        if (!seen_buffers.insert(buffer.name).second) invalid("duplicate");
    }
    std::set<std::string> ir_graph_names;
    for (const auto &graph : variant.render_graph->graphs) {
        if (graph.name.empty()) refuse_graph("graph_name_required");
        if (!ir_graph_names.insert(graph.name).second)
            refuse_graph("duplicate_graph", graph.name);
        if (request.artifact == RenderCompilerProgramArtifact::runtime_package &&
            !variant.frame_plans.contains(graph.name))
            refuse_graph("frame_plan_missing", graph.name);
    }
    std::vector<std::string> frame_graph_names;
    frame_graph_names.reserve(
        variant.frame_plans.size());
    for (const auto &[name, frame_plan] :
         variant.frame_plans) {
        if (name.empty() ||
            frame_plan.name != name) {
            refuse_graph("frame_plan_name_mismatch", name);
        }
        if (!ir_graph_names.contains(name)) {
            refuse_graph("graph_missing", name);
        }
        // RC-2b: the compiler output boundary.
        validateFramePlanStructure(frame_plan);
        frame_graph_names.push_back(name);
    }
    std::sort(
        frame_graph_names.begin(),
        frame_graph_names.end());
    if (request.artifact ==
        RenderCompilerProgramArtifact::
            runtime_package) {
        if (variant.physical_package == nullptr) {
            throw std::runtime_error(
                "Render compiler runtime artifact "
                "returned a null physical package");
        }
        if (variant.physical_package->backend() !=
            selection.backend) {
            throw std::runtime_error(
                "Render compiler physical package "
                "backend '" +
                std::string{
                    variant.physical_package
                        ->backend()} +
                "' does not match selection backend '" +
                selection.backend + "'");
        }
        variant.physical_package->validate(
            frame_graph_names);
    } else {
        if (variant.physical_package != nullptr) {
            throw std::runtime_error(
                "Render compiler data-only artifact "
                "returned a physical package");
        }
        if (!variant.frame_plans.empty()) {
            throw std::runtime_error(
                "Render compiler data-only artifact returned "
                "runtime frame plans");
        }
    }

    auto stamped =
        std::make_shared<CompiledRenderPipeline>(
            *variant.compiled_pipeline);
    stamped->render_compiler_program =
        selection;
    variant.compiled_pipeline =
        std::move(stamped);
}

} // namespace

GenerationResources completeGenerationResources(
    std::span<const RenderCompilerProgramVariantOutput> variants,
    ResolvedResourceExtent output_extent,
    const std::function<void(std::string_view, const ResourceCheckRead &)>
        &observe_resource_checks) {
    std::vector<GenerationResourceVariant> inputs;
    std::vector<const RenderCompilerProgramVariantOutput *> runtime;
    for (const auto &variant : variants) {
        if (variant.physical_package == nullptr) continue;
        if (variant.render_graph == nullptr) {
            throw std::runtime_error(
                "Render compiler runtime variant has no render graph");
        }
        const auto placements =
            variant.physical_package->imagePlacements();
        inputs.push_back(GenerationResourceVariant{
            .name = std::string{renderPipelineGraphVariantName(
                variant.graph_variant)},
            .render_graph = variant.render_graph.get(),
            .placements = {placements.begin(), placements.end()},
            .runtime_layered_images =
                runtimeLayeredImagesOf(*variant.compiled_pipeline),
        });
        runtime.push_back(&variant);
    }
    if (inputs.empty()) return {};
    auto resources =
        resolveGenerationResources(inputs, output_extent);
    for (std::size_t index = 0; index < runtime.size(); ++index) {
        const auto &variant = *runtime[index];
        std::vector<FrameGraphBufferDefinition> buffers;
        for (const auto buffer : resources.variants[index].buffers)
            buffers.push_back(resources.buffers[buffer]);
        validateComputeTaskBufferContracts(*variant.render_graph, buffers);
        validateGpuDrawSourceBufferContracts(
            *variant.render_graph, buffers);
        ResourceCheckObserver observe;
        if (observe_resource_checks) {
            observe = [&observe_resource_checks,
                       &name = inputs[index].name](
                          const ResourceCheckRead &read) {
                observe_resource_checks(name, read);
            };
        }
        validateRenderGraphResources(*variant.render_graph, resources, observe);
        (void)renderingPassDefinitionsOf(*variant.render_graph, resources,
                                         generationRenderTargetNames(resources));
    }
    return resources;
}

RenderCompilerProgramResult runRenderCompilerProgram(
    const RenderCompilerProgram &program,
    const RenderCompilerProgramInput &input) {
    validateInput(input);
    const auto selection =
        program.selection(input.backend_context);
    validateSelection(
        selection, input.backend_context);
    auto authored_output = program.compile(input);
    RenderCompilerProgramResult output{.variants = std::move(authored_output.variants)};
    if (output.variants.size() !=
        input.variants.size()) {
        throw std::runtime_error(
            "Render compiler program returned the wrong "
            "number of graph variants");
    }
    for (std::size_t index = 0;
         index < output.variants.size(); ++index) {
        validateVariantOutput(
            output.variants[index],
            input.variants[index],
            selection);
        const auto &variant = output.variants[index];
        if (variant.render_graph && variant.compiled_pipeline && variant.compiled_pipeline->material_routing)
            validateMaterialRoutingAgainstIR(*variant.compiled_pipeline->material_routing, *variant.render_graph);
    }
    // RC-3c2b1: the compute tasks every runtime variant registers, from its
    // IR (the ray tracing refusals before the resources', as the task parse
    // came first).
    for (const auto &variant : output.variants) {
        if (variant.physical_package == nullptr) continue;
        if (variant.render_graph == nullptr) {
            throw std::runtime_error(
                "Render compiler runtime variant has no render graph");
        }
        output.compute_task_definitions.push_back(computeTaskDefinitionsOf(
            *variant.render_graph, input.ray_tracing_project.get()));
    }
    output.generation_resources = completeGenerationResources(
        output.variants, input.backend_context.outputExtent(),
        input.observe_resource_checks);
    return output;
}

} // namespace Pelican
