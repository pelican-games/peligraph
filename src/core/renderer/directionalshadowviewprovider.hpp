#pragma once

namespace Pelican {

struct CompiledFrameGraphExecution;
class LightContainer;
class RenderTargetContainer;
class RenderViewFamilyProviderRegistry;

// Matches the provider-owned shadow target to the scene-derived family
// cardinality. Returns true when image resources were republished and their
// sampled descriptors must be rebound.
bool prepareDirectionalShadowRenderTarget(
    const CompiledFrameGraphExecution
        &frame_graph,
    const LightContainer &lights,
    RenderTargetContainer &render_targets);

void registerDirectionalShadowViewFamilyProvider(
    RenderViewFamilyProviderRegistry
        &registry);

} // namespace Pelican
