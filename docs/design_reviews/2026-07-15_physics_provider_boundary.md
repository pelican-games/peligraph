# Physics provider boundary

Date: 2026-07-15

Status: query boundary, built-in provider, purge configurations, and optional Jolt provider implemented

## Outcome

Physics queries now cross a versioned `PhysicsServiceV1` / `ProviderV1` function-table boundary.
Pelican retains the stable game-facing query API, while the narrow-phase implementation can be
Pelican's small built-in provider, Jolt 5.5.0, or a capability-level overlay supplied by the
active game DLL. No Jolt or Vulkan type appears in the public physics ABI.

This revision is deliberately a **query boundary**, not a rigid-body simulation API. It covers
raycast and overlap for sphere, box, and capsule shapes. Fixed-step bodies, contacts, constraints,
events, state snapshots, rollback, and GPU simulation remain separate follow-up contracts.

## Responsibility split

Pelican owns behavior that must not change when a provider is replaced:

- collider identity and ECS generation identity;
- layer/mask, trigger, one-way, self, and ignore-list filtering;
- standard-shape and ray normalization plus provider result validation;
- deterministic hit ordering and legacy name-only adapters;
- game-DLL registration ownership and hot-reload lifetime.

A provider receives only the already-selected standard shapes and returns collider indices plus
geometric hit data. V1 permits at most one ray or overlap result per input collider. Ray direction
magnitude is ignored, maximum distance is measured in world units, and a ray starting inside a
closed shape reports its first forward exit surface. Touching shapes count as overlapping.

This split lets a custom implementation change algorithms, acceleration structures, allocation,
or threading without being able to silently redefine game identity, filtering, or deterministic
ordering.

## Differential provider behavior

The game provider is an overlay, not an all-or-nothing replacement. Its capability bits are
authoritative per operation:

- an advertised operation is always routed to the game provider;
- an operation not advertised by the game provider falls through to Jolt or the built-in provider;
- if no configured provider exists, a missing operation returns `unavailable`;
- an advertised callback error does not silently fall back to another implementation.

This lets a game replace raycasts while retaining the configured overlap implementation, then add
the remaining operation later. `ServiceV1::capability_bits` describes the stable operations the
service understands rather than a snapshot of a hot-reloadable provider; an individual call can
therefore return `unavailable` when no active provider supplies that operation.

## ABI and lifetime contract

`src/core/userpublic/physics/abi_v1.hpp` contains fixed-layout, standard-layout descriptors and C
function pointers. It does not pass STL containers, exceptions, Jolt objects, or Vulkan handles
across the game-DLL boundary. Every descriptor carries a byte size and version; unknown capability
bits and non-zero reserved fields are rejected.

Provider callbacks are `noexcept`, may run concurrently, and report failures with `Status`. The
registry holds a shared lifetime lock while invoking a callback, so owner release waits for
in-flight work before the loader calls `FreeLibrary`. Callbacks must therefore not recursively
register or unregister a provider.

A game provider registers while its DLL is loading, inside the loader's registration-owner scope.
The validation load is never authoritative and is released without changing the live provider.
During reload commit, teardown releases the old owner, waits for its callbacks, and unloads the old
DLL. The accepted shadow copy is then loaded again, its owner is activated before scene rebuild,
and rollback reloads the previous shadow under a fresh owner if commit fails. Provider cleanup on
DLL unload is automatic; a later unowned registration call is rejected with `wrong_owner` so it
cannot leave a callback into an unloaded image.

## Build and purge matrix

| Configuration | Compiled implementation | Active default |
|---|---|---|
| `PELICAN_WITH_PHYSICS=OFF` | public ABI compatibility header and unavailable service stub only | none |
| physics ON, built-in OFF, Jolt OFF | service, registry, and collider world only | none; game provider required per operation |
| physics ON, built-in ON | small Pelican sphere/box/capsule provider | `pelican.builtin` |
| physics ON, Jolt ON | private static Jolt 5.5.0 adapter; built-in is forced OFF | `pelican.jolt` |

The active game-DLL provider overlays the configured default only after owner activation. Releasing
that owner returns every operation to the configured default, or to unavailable in provider-only
builds.

With physics OFF, `physicsruntime.cpp`, `physicsservice.cpp`, `physworld.cpp`, all narrow-phase
sources, and both provider adapters are absent from build metadata and object artifacts. Jolt is
not fetched. A scene containing a collider produces an explicit
`This binary was built with PELICAN_WITH_PHYSICS=OFF` error rather than being silently ignored.
The ABI header and stub remain so shared game-facing code can negotiate unavailability safely.

Jolt is fetched only when `PELICAN_WITH_JOLT_PHYSICS=ON`, is pinned to `v5.5.0`, and is linked as a
private static implementation dependency. Its generic CMake options are confined to function scope.
The public game-DLL fixture has no Jolt include directory, so game code cannot accidentally acquire
a source-level dependency on Jolt through Pelican headers.

## CPU simulation and GPGPU lane

Synchronous V1 query callbacks must return before the call ends. Hiding a GPU dispatch and readback
inside them would introduce a queue flush and frame stall, so the query ABI must not be reused as a
GPU simulation interface.

Future simulation work is split into two independently removable lanes:

1. A fixed-step CPU dynamics provider for rigid bodies, contacts, constraints, snapshots, and
   deterministic/rollback policy. Jolt is a candidate implementation of this lane, but its types
   still stay behind the provider boundary.
2. An asynchronous workload provider for cloth, particles, fluids, and other GPU-friendly solvers.
   Its contract needs enqueue, dependency/resource declarations, completion tokens, latency, and
   snapshot publication rather than immediate query results. It should integrate with the frame
   graph's future asynchronous compute-queue work, not export raw Vulkan objects through
   `ProviderV1`.

The two lanes may coexist: a CPU rigid-body provider can supply gameplay contacts while a GPU solver
updates high-volume secondary motion. Neither is required by the query service. Each future backend
must have its own build target/option so a game can omit it, and a game-owned implementation must be
able to replace it without changing collider identity or query semantics.

The current frame graph remains single-queue for compute execution. This change does not claim an
asynchronous GPGPU physics solver; it establishes the boundary that prevents adding one from leaking
Jolt or Vulkan through the stable query API.

## Verification gates

- Normal built-in configuration: service, PhysWorld, and primitive-query tests pass.
- Provider-only configuration: no built-in narrow-phase object is emitted; a game-owned provider
  registers, activates, filters selected colliders, and is released by owner.
- Physics-off configuration: only the unavailable stub is linked; the feature probe succeeds and a
  collider scene fails with the named build-option diagnostic.
- Jolt-only configuration: service negotiation plus sphere, box, capsule, inside-ray, overlap,
  capability-level owner override/fallback, and owner-release cases pass without any Jolt type in
  test-facing APIs.
- Public API link fixture covers physics ABI negotiation from a game DLL.
