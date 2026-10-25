#pragma once

#include "renderingpass.hpp"
#include "rendertargetnameresolver.hpp"
#include "../../project/generationresources.hpp"

namespace Pelican {

// The same conversion serves finalized runtime IR and the editor's selected
// node in unfinalized IR with resolveDeclaredGenerationResources. It never
// parses JSON or finalizes the graph. Required execution stages are checked
// here; authoring shape is checked by parseRenderGraph.
PassDefinition passDefinitionOf(const RenderGraphIR &ir, const GraphIR &graph,
                                 const NodeIR &node, const GenerationResources &resources,
                                 const RenderTargetNameResolver &names);

// Preserve declaration order, excluding only non-pass nodes (anchors,
// snapshots, compute tasks and acceleration-structure builds).
RenderingPassDefinition renderingPassDefinitionOf(
    const RenderGraphIR &ir, const GraphIR &graph, const GenerationResources &resources,
    const RenderTargetNameResolver &names);
std::vector<RenderingPassDefinition> renderingPassDefinitionsOf(
    const RenderGraphIR &ir, const GenerationResources &resources,
    const RenderTargetNameResolver &names);

// A resource description's stable indices, for completion and editor
// validation before a GPU registry exists. The description must outlive it.
RenderTargetNameResolver generationRenderTargetNames(const GenerationResources &resources);

} // namespace Pelican
