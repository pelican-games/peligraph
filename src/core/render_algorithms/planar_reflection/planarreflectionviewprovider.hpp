#pragma once

namespace Pelican {

class RenderViewFamilyProviderRegistry;

void registerStandardPlanarReflectionViewProvider(
    RenderViewFamilyProviderRegistry
        &registry);

} // namespace Pelican
