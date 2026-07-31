# WP240b: project-owned solid sky and ambient

## Outcome

`engine://features/sky_ambient.json` now adds a solid background and a
solid-color ambient term to `hybrid_v1` without changing the preset. A project
enables both behaviors by referencing the feature. Removing that reference
removes the background pass and shader define and leaves both environment
radiances at zero.

This is deliberately the minimum visibility feature requested by WP240b. It
does not implement a cubemap, irradiance convolution, a prefiltered
environment, or a BRDF LUT.

## One owner for non-zero defaults

The five non-zero defaults live only in the feature fragment:

- linear-sRGB color `(0.12, 0.16, 0.24)`
- ambient intensity `0.5`
- sky intensity `1.0`

All five scalars use `shader_define: false`. Composition therefore stores the
resolved values in the immutable `CompiledRenderFeatureInstance` without
creating parameter-valued shader variants. A thin runtime adapter reads that
instance each frame and supplies it to `LightContainer`, which writes
`color * ambient_intensity` and `color * sky_intensity` to the LightUBO.

The adapter returns zero when the feature is absent. Engine C++, the deferred
lighting shader, and the public material-lighting include contain no non-zero
fallback. The unrelated shader-lab lighting technique retains its own
technique-local authored ambient; `sky_ambient` neither overrides nor supplies
defaults to that hand-authored renderer.

## Graph and shader integration

The feature inserts `sky_background` after `deferred_lighting`. The fullscreen
pass samples `scene_depth`, loads and preserves `lit_color`, discards pixels
whose depth is not the far clear value, and writes the solid sky radiance only
to uncovered pixels. `forward_opaque` then overwrites the background where
forward-only geometry is drawn, while transparent materials see an already
populated scene color.

The LightUBO appends two `vec4` fields after the shadow matrices:
`environmentAmbientRadiance` and `environmentSkyRadiance`. Matching C++ offset
assertions and the GLSL `std140` declaration keep the existing frame-set ABI
layout explicit. Deferred lighting and the public
`pelican_env_ambient(normal)` path consume the same ambient radiance.

The solid term intentionally multiplies metallic base color as a visibility
fallback. Treating it as diffuse-only would leave fully metallic materials
black with zero lights, which would fail the purpose of this WP. Directional
normal/roughness/view-dependent response remains the responsibility of a
future IBL feature; the existing `normal` argument remains in the public helper
so that evolution does not require a new helper signature.

## Project-space defaults and dogfood

The generated project template now selects `shadow_directional` and
`sky_ambient` alongside the unchanged `hybrid_v1` preset. The committed
`animgraph_demo` does the same and removes its directional scene light, making
the project-level process test exercise the zero-light configuration.

The verbose hand-authored renderer in `projects/example` remains unchanged.
RPC integrations that require visible transform or scene differences now use
a small project-owned `hybrid_v1` plus `sky_ambient` fixture instead of
depending on the legacy renderer's former shader constant.

## Regression evidence

CPU coverage fixes the following contracts:

- feature-off purge of the pass and define
- arbitrary-anchor ordering between deferred lighting and forward opaque
- sampled-depth and load-preserving frame-graph dependencies
- runtime-only parameter defaults and project overrides
- zero fallback, exact runtime transport, and rejection of incomplete
  compiled feature instances

The registered GPU case renders a fully metallic deferred material and a fully
metallic forward OpenPBR material with directional, point, and spot light counts
all equal to zero. It checks the exact uploaded radiances, a non-black
background, and non-black geometry on both routes.

Removing the old engine-shader ambient constant intentionally changed 24
golden images. Representative OpenPBR, shadow, and TAA cases were visually
reviewed; the graph traces were unchanged. The first review also found that the
VAT playback image had become uniformly black because that temporary project
depended solely on the removed constant. Its rendering document now explicitly
selects `hybrid_v1` and `sky_ambient`, restoring a visible, non-uniform VAT
result without copying parameter defaults into the harness. Only the same 24
PNG paths changed. They were updated through the harness' official golden
mode, the exact RGBA8 hash fixture was regenerated, and
`golden_inventory.py` regenerated and verified the inventory hashes.

`render_mechanism_coverage.md` needs no rating change. WP240b uses the existing
feature composition, fullscreen pass, runtime parameter, and LightUBO
mechanisms. The new E3/E5 evidence is recorded in
`render_evidence_ledger.md`.

## Validation

- Debug configure/build with OpenXR enabled: passed. CMake used the
  uv-managed CPython executable explicitly instead of the inactive WindowsApps
  alias.
- CPU gate (`ctest -C Debug -LE gpu`): 940 / 940 passed in 27.11 seconds;
  one existing environment-dependent symlink case skipped.
- GPU gate (`ctest -C Debug -L gpu`): 123 / 123 passed in 419.41 seconds;
  four existing environment-dependent hot-reload/RPC cases skipped.
- `devcli_project_init_command`: passed with the generated default feature set.
- `animgraph_demo_preset_headless_player`: passed with zero scene lights and
  serves as the required short `pelican_player.exe` smoke.
- Golden PNG comparison, exact RGBA8 hashes, and golden inventory verification:
  passed.
- `git diff --check`: passed.
