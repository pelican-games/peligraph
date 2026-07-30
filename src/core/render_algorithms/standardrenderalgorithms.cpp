#include "standardrenderalgorithms.hpp"

#include "cube_capture/cubecaptureviewprovider.hpp"
#include "planar_reflection/planarreflectionviewprovider.hpp"

namespace Pelican {

void registerStandardRenderAlgorithmProviders(
    RenderViewFamilyProviderRegistry
        &registry) {
    registerStandardPlanarReflectionViewProvider(
        registry);
    registerStandardCubeCaptureViewProvider(
        registry);
}

} // namespace Pelican
