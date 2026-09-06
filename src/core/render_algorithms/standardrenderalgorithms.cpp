#include "standardrenderalgorithms.hpp"

#include "cube_capture/cubecaptureviewprovider.hpp"
#include "punctual_shadow/punctualshadowviewprovider.hpp"
#include "planar_reflection/planarreflectionviewprovider.hpp"

namespace Pelican {

void registerStandardRenderAlgorithmProviders(
    RenderViewFamilyProviderRegistry
        &registry) {
    registerStandardPlanarReflectionViewProvider(
        registry);
    registerStandardCubeCaptureViewProvider(
        registry);
    registerPunctualShadowViewProviders(registry);
}

} // namespace Pelican
