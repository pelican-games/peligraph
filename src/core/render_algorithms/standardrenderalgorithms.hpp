#pragma once

namespace Pelican {

class RenderViewFamilyProviderRegistry;

// Registers the C++ policy portion of the optional standard algorithm
// package. Resource embedding is declared by the sibling resource manifest.
void registerStandardRenderAlgorithmProviders(
    RenderViewFamilyProviderRegistry
        &registry);

} // namespace Pelican
