# PhysQuery foundation refactor report (2026-07-15)

## Decision

This is a behavior-preserving foundation change before S2D-P. It does not implement
`shapeCast`/sweep or MTD. The existing name-only raycast and overlap entry points remain available,
while new detailed entry points establish the identity, filtering and ordering contract that those
features will use.

## Identity contract

- `ColliderId` is the stable collider-level key. `PhysWorld` allocates a non-zero monotonic id when
  a binding is accepted and does not reuse it when a stale entity binding is replaced.
- `ColliderIdentity` also carries the display name, the complete ECS `(index, generation)` identity
  when one exists, and a shape/subshape ordinal.
- Pure legacy `Collider{id, shape}` values remain source-compatible. They receive a deterministic
  compatibility id derived from the name and shape ordinal. Runtime `PhysWorld` bindings use their
  allocated id instead.
- Detailed result order uses `(ColliderId, entity identity, shape ordinal, name)` after the distance
  bucket. The name is only the last tie-break and is not the collider key.

## Filter contract

`QueryFilter` and `ColliderQueryMetadata` now define one shared filter path for raycast and overlap:

- reciprocal query/collider layer and mask checks;
- trigger and one-way inclusion flags;
- collider-id self/ignore filtering;
- full ECS entity self/ignore filtering.

The metadata defaults preserve the old all-collider behavior. Scene/component schema fields for
these values are deliberately deferred to S2D-P; current bindings may supply metadata directly.

## Compatibility boundary

The original `ObjectRaycastHit`, `GameContext::raycastClosest(Ray)` and
`GameContext::overlapAll(Shape)` signatures and layouts are unchanged. Their exact legacy behavior
also remains unchanged: closest equal-distance ties use the lexicographic name and overlap names
are sorted lexicographically.

Detailed identity is returned by the additive `RaycastQueryHit`, `raycastAll`, filtered
`raycastClosest`, and `overlapAllHits` APIs. The filtered closest result is derived from the same
ordered all-hit result. The public game-DLL link fixture references both the old and new symbols, so
this refactor does not require a game-logic ABI version increment.

## Responsibility split

- `physquery.cpp` contains primitive math, narrow-phase raycast and overlap algorithms.
- `physqueryaggregate.cpp` owns identity normalization, filters, deterministic multi-collider
  ordering and compatibility adapters.
- `PhysWorld` owns persistent runtime ids and attaches complete entity generations while resolving
  live transforms.

## Verification

- PhysQuery: 9 cases / 106 assertions passed.
- PhysWorld: 5 cases / 49 assertions passed.
- The public API game-DLL fixture built while linking both legacy and detailed query methods.
- The full Debug build completed successfully.
- Full Debug CTest: 377 discovered, 376 passed, 0 failed, and the existing Windows symlink test
  skipped as expected.

## Deliberate S2D-P remainder

S2D-P still owns shape sweep/shapeCast, all shape-pair TOI implementation, initial-overlap MTD,
directional one-way continuation, collider scene schema fields, and the platformer CPU fixture set.
This refactor only makes those additions local and deterministic.

## Closure update (WP107, 2026-07-15)

WP107 now closes the shapeCast/sweep, all-pair TOI, initial-overlap MTD,
collider schema, Provider ABI V2 and ordered filter-continuation items above.
Directional one-way movement policy and the `moveAndSlide`/platformer scenario
library deliberately remain in S2D-2; WP107 provides the metadata and ordered
all-hit mechanism they consume.
