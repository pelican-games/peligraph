# Rendering Phase 1 Review Notes

## Status

Phase 1 is closed on the `codex/rendering-phase1-refactor` branch without a PR.
The branch is intended to stay reviewable as a commit series: first the
behavior-preserving rendering split, then small correctness fixes, tests, and
cleanup.

## Scope

Phase 1 keeps the current rendering behavior, but separates the rendering pass
configuration and execution code into smaller units.

## Main Changes

- Split rendering pass JSON loading into helpers, target parsing, pass definition
  parsing, validation, and top-level registration.
- Split render pass execution into frame setup and pass dispatch helpers.
- Replaced special render target, rendering pass, and material id literals with
  named helper functions.
- Added fail-fast checks for invalid UI pass dispatch and unsupported dynamic
  pass dispatch.
- Added lightweight Catch2 coverage for rendering pass helper parsing and
  special id helpers.
- Added capacity checks for polygon instance and indirect draw buffers.
- Implemented polygon instance draw command removal.

## Behavior Changes To Review

- Invalid UI pass dispatch now throws instead of silently doing nothing.
- Unsupported dynamic pass dispatch now throws instead of silently doing nothing.
- Polygon instance placement now throws when fixed buffer capacity is exceeded.
- Removing a polygon instance now removes its draw commands.
- Invalid model instance transforms now throw.

## Validation Run

- `cmake --build build --config Debug --target pelican_player`
- `ctest --test-dir build -C Debug --output-on-failure`
- Short hidden launch of `pelican_player.exe`
- `git diff --check`

## Known Follow-Ups

- Separate JSON parsing, graph validation, and runtime pipeline compilation more
  clearly in Phase 2.
- Decide the intended material-pass filtering model; `isRenderRequired` still
  renders every material.
- Replace fixed polygon instance and indirect draw capacities with growable
  buffers or explicit config.
- Clean up remaining renderer comments and naming while avoiding behavior churn.

## Phase 2 Entry Point

Start Phase 2 by moving runtime rendering-pass compilation out of
`RenderingPassContainer`. The container should become mostly a storage and lookup
module, while compilation owns pass-id assignment, fullscreen pipeline creation,
and descriptor binding setup.
