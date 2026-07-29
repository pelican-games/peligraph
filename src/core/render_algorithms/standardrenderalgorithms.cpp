#include "standardrenderalgorithms.hpp"

#include "planar_reflection/planarreflectionviewprovider.hpp"

namespace Pelican {

void registerStandardRenderAlgorithmProviders(
    RenderViewFamilyProviderRegistry
        &registry) {
    registerStandardPlanarReflectionViewProvider(
        registry);
}

} // namespace Pelican
