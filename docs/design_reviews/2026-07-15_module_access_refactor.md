# Module access refactor implementation report (2026-07-15)

## Decision

`GET_MODULE` remains the engine's lazy module lookup. The refactor does not replace it with
constructor injection everywhere. Instead, it limits lookup to explicit composition boundaries and
passes references or dependency structs through execution code.

The intended rules are:

1. Startup, frame, feature and worker-prepare boundaries may resolve modules with `GET_MODULE`.
2. Optional observation uses `FastModuleContainer::tryGet<T>()`; it must not create a module.
3. Worker jobs consume dependencies prepared on the module owner thread.
4. Shutdown may read initialized modules but may not create new ones.
5. Constructor-time `GET_MODULE` remains valid and is recorded as a lifetime dependency.

## Implemented

### Module container safety and observability

- Added `booting`, `running` and `shutting_down` runtime phases.
- Added owner-thread enforcement for first module construction. Already initialized modules remain
  readable from workers.
- Added construction-cycle detection and exception-safe publication. A module is not observable until
  its constructor and cleanup registration both succeed.
- Added `tryGet<T>()` for side-effect-free optional access.
- Added constructor dependency edges, initialized-module inventory and runtime-late initialization
  inventory through `graphSnapshot()`.
- Added strict `freezeCreation()` support and shutdown-time creation rejection.
- Preserved reverse-construction destruction and rejected overlapping container lifetime scopes.

Dependency edges in `graphSnapshot()` intentionally describe lookups observed while a module
constructor is active. Runtime resolver calls are represented by the runtime-late inventory rather
than being mislabeled as constructor dependencies.

### Explicit execution boundaries

- `Renderer` now resolves frame, sprite and shader-reload dependencies at named boundaries. Render
  helpers receive references through `RenderFrameModules` instead of performing scattered lookups.
- Appflow resolves loop, frame-state, interactive-window and GPU-drain dependencies before executing
  phase bodies.
- `prepareFrameStateModules()`, `prepareInputActionsRuntime()` and
  `Renderer::prepareRuntimeModules()` explicitly construct runtime dependencies before the graph is
  frozen. Public `GameContext` services (physics, RNG, debug text, persistence and optional audio)
  are prepared at the same composition root.
- Teardown and inspection paths use `tryGet<T>()`, preventing accidental module resurrection.
- Runtime calls `freezeCreation()` immediately before the main loop and enters `shutting_down`
  before explicit teardown. Existing `GET_MODULE` calls remain valid reads; a missed first
  construction is now a deterministic startup-boundary error.

### RPC composition boundary

- `runEngineRpcServer()` resolves its engine services once through `EngineRpcModules`.
- RPC handlers and transform helpers consume those references rather than returning to the module
  container for each command.
- Optional reload and sprite diagnostics use `tryGet<T>()` pointers captured at composition time.
- `get_status.modules` exports runtime phase, freeze state, initialized/dependency counts and the
  runtime-late inventory.

### ECS worker boundary

- Added the optional `prepareEcsWorkerDependencies(...)` hook, executed on the ECS owner thread before
  jobs in each dependency level are scheduled.
- Both the compatibility `bool has_matching_chunks` form and a query-aware `span<ChunkView<...>>`
  form are supported.
- Predefined animation, camera, transform, model-view and sprite systems resolve module references in
  that hook rather than in worker `process` bodies.
- Query-aware preparation preloads dirty model assets and sprite atlas pages on the owner thread.
  This closes transitive lazy-initialization leaks such as `MaterialContainer` or
  `AtlasAssetResource` first being requested by a worker.

### Reload routing

- Replaced the material-specific branch in `ReloadService` with named `ReloadParticipant` adapters.
- Participants declare `claims`, `enqueue` and optional `retire` callbacks.
- Duplicate names and ambiguous claims are rejected; ambiguous requests enqueue nothing.
- The existing material texture/value handlers are registered through the same participant contract.
- Reload status now exposes the active participant names.

## Enforcement status

Strict creation freeze is enabled by default. The owner thread prepares frame, input, renderer,
feature-gated and public-facade services, then calls `freezeCreation()` before any headless, RPC or
windowed frame can run. The normal runtime therefore has an empty
`initialized_after_runtime_start` inventory by construction.

`GET_MODULE` itself remains part of the design. It is appropriate in constructors, startup/feature
resolvers and public facade adapters; after freeze those calls are reads of already initialized
modules. Worker bodies and command handlers receive prepared references instead.

Two source-level regression gates enforce the highest-risk rules: every RPC `GET_MODULE` lookup must
remain inside `resolveEngineRpcModules()`, and the main loop must prepare dependencies before
`freezeCreation()` without falling back to `enterRunningPhase()`.

## Verification

- Release and Debug builds completed successfully.
- The final complete Debug CTest suite discovers 374 tests: 373 passed, 0 failed and the Windows
  symlink escape case was the one expected platform skip.
- Renderer trace, RGBA8 hash, sprite, headless render, model loading, reload transaction, public API
  fixture, RPC creation-freeze status and ECS lifecycle coverage are included in that suite.
- The example player passed a two-frame 64x64 headless smoke test with the graph frozen at 61 modules
  and 72 constructor-dependency edges.
- The sprite demo passed a two-frame 960x540 headless smoke test with the graph frozen at 60 modules
  and 61 constructor-dependency edges, matching its configured attachment extent.

The sprite demo's headless path currently rejects a different requested extent (and the generic
headless default) because its selected sprite color and depth attachments no longer match. This is a
demo/render-target sizing follow-up, not a module-owner-thread failure.

## Future extensions (not blockers for this refactor)

1. Demangle implementation-defined type names when exporting the full module graph to developer
   tooling.
2. Add a registration-based bootstrap capability if third-party engine plugins need to introduce new
   runtime modules without editing the central composition root.
3. Shader and game-logic runtime adapters were added after this review; their safe-boundary and
   transaction ownership contract is recorded in
   `docs/design_reviews/2026-07-15_runtime_reload_participants.md`.
