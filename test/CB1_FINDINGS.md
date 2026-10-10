# CB-1 findings

Ticketed product failures, following QD-1's known_red policy. A skipped CTest
case is not a successful rendering invariant. The test still executes the
reproduction; only the specific observed failure below is marked `KNOWN_RED`.
Other exceptions, empty captures and failed controls remain failures. When the
invariant starts passing, the same test runs its normal assertions without a skip.

## CB-1/02 graph.loop_variant_missing for a skinned subject

- Reproduce: run CTest with `-R '^CB1 02 ' -j1`. The fixture declares Paint's
  `outline` variant, with a front-cull surface, then installs a draw loop matching
  `when.skinned` and `when.material = paint.material.json#Paint` before renderer
  initialization. The glTF control has no variant and is excluded from this branch.
- Observed: `graph.loop_variant_missing@1` with exactly
  `{"material":"paint.material.json#Paint","material_variant":"outline","node":"cb_skinned_outline"}`.
  No image is produced because initialization fails. The exact refusal is saved
  in `2/known_red.json` below the artifact directory.
- Cause candidate: `src/core/material/projectmaterialasset.cpp:705` registers the
  base before attaching variants at line 738. `materialcontainer.cpp:1204` notifies
  live draw-loop tables during base registration. `materialdrawlooptable.cpp:227`
  constructs the material-by-vertex-mode subject and line 235 validates the named
  step before `materialcontainer.cpp:1281` publishes the variant map. The current
  `drawLoopSubject` copies the map at `materialcontainer.cpp:2476`; it does not
  intentionally drop the skinned subject's variants. This points to registration
  timing, rather than a missing fixture declaration.
- Proposed WP: SK-1 fix round 3 / LP-2 atomic base-plus-variants publication. Also
  review the Studio aggregation at `src/core/communication/renderconfigeditor.cpp:3579`
  (the first material row supplies the variant list). Do not work around the
  product bug by delaying the fixture's loop installation or requiring has_variant.

## CB-1/08 vertex_displace_v1 absent from auxiliary passes (VD-1)

- Reproduce: run CTest with `-R '^CB1 08 ' -j1`. Render a static Paint subject,
  then reload its surface with `vertex.position.x += 0.7`. Use the same view for
  main/shadow/picking and x=-0.1 to x=0 previous/current object motion for velocity.
- Observed: without the hook, all auxiliary masks equal main (1872 pixels, bbox
  `[52,25,75,102]`). With the hook, main moves to `[79,25,102,102]`; shadow,
  velocity and picking remain at the original bbox. Each comparison differs by
  3744 / 16384 pixels. The test retains all three comparison JSONs and PPMs, then
  skips with the ticket and failing pass names in `8/known_red.json`.
- Cause candidate: engine pass shaders transform the original/morphed position:
  `src/core/resources/shadow_depth.vert:19`, `velocity.vert:22`, `picking.vert:14`.
  They do not compose the project surface's vertex hook. Pipeline construction is
  pass-specific (`src/core/renderer/shadowdepthpasscontainer.cpp:37`,
  `velocitypasscontainer.cpp:34`, `src/core/renderingpass/passinfojsonparser.cpp:123`).
- Proposed WP: VD-1, propagate material vertex displacement into shadow, velocity
  (including previous-frame deformation), and picking pipelines and their reloads.
  CB-1 changes no product code.

## Fixture corrections (not product tickets)

- 03: forward_transparent already exists; the baseline and blend color masks both
  contain 1641 pixels and match exactly. Comparing opaque written depth against
  blend read-only depth was invalid. Main now compares color support, with separate
  assertions for opaque depth/color agreement and zero blend depth writes.
- 04 and 05: both baseline and overridden captures were empty, while the resolved
  primitive material checks passed. `Camera::makeDefaultPreparedSceneState`
  (`src/core/renderer/camera.cpp:304`) resets a camera-less scene's camera to the
  origin. Set the fixture camera after each scene load. Keep override count checks.
- 12: both before-save and after-reopen captures were empty. Assets/material links
  survive serialization and the material-ID checks pass; the same camera reset
  explains both captures. Set the camera after loading in each new runtime. Keep
  actual save_scene, project write, runtime destruction and reload.

Artifacts default to the CTest working directory's `cb1_artifacts/<row>`;
`PELICAN_CB1_ARTIFACT_DIR` can select a separate reproduction directory.
