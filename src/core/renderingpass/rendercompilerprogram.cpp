#include "rendercompilerprogram.hpp"

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
    if (!variant.normalized_config.is_object()) {
        throw std::runtime_error(
            "Render compiler program normalized config "
            "must be an object");
    }

    std::unordered_set<std::string>
        expected_buffer_names;
    for (const auto &definition :
         variant.buffer_definitions) {
        if (definition.name.empty() ||
            !expected_buffer_names
                 .insert(definition.name)
                 .second) {
            throw std::runtime_error(
                "Render compiler program produced a "
                "duplicate or unnamed buffer definition");
        }
    }
    if (expected_buffer_names !=
        variant.buffer_names) {
        throw std::runtime_error(
            "Render compiler program buffer-name index "
            "does not match its definitions");
    }
    std::vector<std::string> frame_graph_names;
    frame_graph_names.reserve(
        variant.frame_plans.size());
    for (const auto &[name, frame_plan] :
         variant.frame_plans) {
        if (name.empty() ||
            frame_plan.name != name) {
            throw std::runtime_error(
                "Render compiler program produced a "
                "mismatched or unnamed frame plan");
        }
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
    } else if (
        variant.physical_package != nullptr) {
        throw std::runtime_error(
            "Render compiler data-only artifact "
            "returned a physical package");
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

RenderCompilerProgramOutput runRenderCompilerProgram(
    const RenderCompilerProgram &program,
    const RenderCompilerProgramInput &input) {
    validateInput(input);
    const auto selection =
        program.selection(input.backend_context);
    validateSelection(
        selection, input.backend_context);
    auto output = program.compile(input);
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
    }
    return output;
}

} // namespace Pelican
