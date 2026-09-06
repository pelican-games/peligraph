#pragma once

namespace Pelican {
class RenderViewFamilyProviderRegistry;
class LightContainer;
class RenderTargetContainer;
struct CompiledFrameGraphExecution;

void registerPunctualShadowViewProviders(RenderViewFamilyProviderRegistry &registry);
bool preparePunctualShadowRenderTargets(const CompiledFrameGraphExecution &graph,
    const LightContainer &lights, RenderTargetContainer &targets);
} // namespace Pelican
