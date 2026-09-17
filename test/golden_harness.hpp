#pragma once

namespace Pelican::GoldenHarness {

void runProjectionJitterEquivalence();
void runProjectionJitterShadow();
void runTaaDeterminism();
void runBLayerShadowEquivalence();
void runMultiLightDirectionalShadows();
void runPunctualShadows();
void runShadowFiltering();
void runPlanarReflection();
void runGpuDrawIndirect();
void runGpuOcclusionCulling();
void runGpuSegmentedOcclusionCulling();
void runGpuSegmentedOcclusionXr();
void runGpuSegmentedOcclusionHotReload();
void runGoldenImages();
void runEditorRuntimeBinding();
void runLogicalFrameStereo();
void runLogicalFrameStereoSealedRetirement();
void runOpenXrTaaTransition();
void runVelocityFeature();
void runSetTimeSkinnedVelocity();
void runMorphVelocity();
void runModelReloadGpuState();
void runGpuTimingIdentity();
void runGpuTimingRing();
void runGpuTimingCompute();
void runGpuTimingSprite();
void runGpuDrawBreakEvenTiming();
void runFullscreenRebind();
void runSsaoFlatGolden();
void runSsaoXrSequentialGolden();
void runSsaoXrMultiviewGolden();
void runSsaoCubeGolden();
void runSsaoPlanarGolden();
void runSsaoBlurFlatOracle();
void runSsaoBlurHalfResolutionOracle();
void runSsaoBlurXrSequentialOracle();
void runSsaoBlurXrMultiviewOracle();
void runSsaoBlurCubeOracle();
void runSsaoBlurPlanarOracle();
void runSsaoBlurCrossFamilyOracle();
void runSsaoBlurIntegrationSmoke();
void runBloomUpsampleOracle();
void runGraphRootBloom();
void runImportedRenderComponent();
void runBloomLoadSemantics();
void runBloomFeatureParameters();
void runBloomRuntimeParameter();

} // namespace Pelican::GoldenHarness
