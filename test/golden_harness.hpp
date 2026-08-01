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
void runLogicalFrameStereo();
void runOpenXrTaaTransition();
void runVelocityFeature();
void runSetTimeSkinnedVelocity();
void runMorphVelocity();
void runRgba8Hashes();
void runGpuTimingIdentity();
void runGpuTimingRing();
void runGpuTimingCompute();
void runGpuTimingSprite();
void runGpuDrawBreakEvenTiming();
void runRendererTrace();
void runFullscreenRebind();

} // namespace Pelican::GoldenHarness
