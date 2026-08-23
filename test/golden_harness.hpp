#pragma once

namespace Pelican::GoldenHarness {

void runProjectionJitterEquivalence();
void runProjectionJitterShadow();
void runTaaDeterminism();
void runBLayerShadowEquivalence();
void runMultiLightDirectionalShadows();
void runPlanarReflection();
void runGpuDrawIndirect();
void runGpuOcclusionCulling();
void runGpuSegmentedOcclusionCulling();
void runGpuSegmentedOcclusionXr();
void runGpuSegmentedOcclusionHotReload();
void runGoldenImages();
void runEditorRuntimeBinding();
void runLogicalFrameStereo();
void runOpenXrTaaTransition();
void runVelocityFeature();
void runSetTimeSkinnedVelocity();
void runMorphVelocity();
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

} // namespace Pelican::GoldenHarness
