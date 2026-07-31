# WP240a: engine default rendering preset

## Outcome

New projects now inherit
`engine://render_pipelines/hybrid_v1.json` and enable the standard directional
shadow feature. The generated rendering document contains only the preset and
feature references; it does not own `render_targets` or `rendering_passes`.
`default_rendering_pass` remains `main_render`.

This required no runtime engine change. `pelican_cli project init` changed only
its distributed project template.

## This is a feature upgrade

The previous 111-line generated configuration was not an expanded form of
`hybrid_v1`. It had four passes: deferred G-buffer, SSAO, SSAO blur, and a
fullscreen pass that combined lighting with presentation. It had no
`forward_opaque`, `forward_transparent`, or opaque color/depth snapshots.

The preset adds automatic material routing across deferred and forward paths,
separate deferred lighting and presentation, deterministic opaque/transparent
draw sorting, forward depth reuse, and the snapshots required by transparent
refraction. New projects therefore gain forward-only materials, transparency,
and refraction instead of merely receiving a shorter file. They also follow
future compatible improvements to the engine preset.

## Project-space dogfood

`projects/animgraph_demo` is the first committed project migrated to the preset.
Its product contract is the animation graph and embedded skeletal fixture, not
a custom rendering algorithm. Its copied legacy bloom chain was removed; the
demo now uses the same hybrid default and directional-shadow feature as a newly
generated project. Its UI document is an empty root panel, so removing the
legacy `ui` feature reference does not remove a demo interaction or rendering
contract.

The other committed projects were deliberately not selected:

- `projects/example` remains the verbose hand-authored bloom/refract/toon
  example required by WP240a.
- `projects/sprite_demo` owns a Generic Raster Pass dogfood implementation.
- `projects/vrm_xr_demo` intentionally excludes TAA, projection jitter,
  velocity, and history for its flat/XR activation contract.

## Regression coverage

`devcli_project_init_command` now verifies the exact preset and feature
references, rejects generated ownership of `render_targets` or
`rendering_passes`, and renders the untouched generated project headlessly.

`animgraph_demo_preset_headless_player` separately reads the committed project
configuration, verifies the same ownership boundary and `main_render` entry
point, then inspects the runtime frame-plan dump for the directional shadow,
deferred, forward, opaque snapshot, transparent, and presentation nodes. It
renders three headless frames, rejects Vulkan validation errors, and requires a
non-empty PNG. This is the project-space execution evidence for the preset
path; the generated-project test remains the distribution-default evidence.

`projects/example` and its verbose rendering configuration are byte-unchanged.
The full GPU gate also exercises that document through the existing golden and
player integrations, preserving the non-preset route.

`render_mechanism_coverage.md` needs no rating change: WP240a exposes no new
rendering mechanism and instead gives the existing preset resolver its first
committed-project consumer. The corresponding evidence change is recorded in
`render_evidence_ledger.md`.

## Validation

- Debug build: passed.
- CPU gate (`ctest -C Debug -LE gpu`): 942 / 942 passed; one existing
  environment-dependent case skipped.
- GPU gate (`ctest -C Debug -L gpu`): 122 / 122 passed; four existing
  environment-dependent cases skipped.
- `devcli_project_init_command`: passed with an untouched generated project.
- `animgraph_demo_preset_headless_player`: passed with the committed project.
- Vulkan validation errors in both new process integrations: zero.
