# WP234 Runtime ViewFamily provider package

Status: implemented and verified (2026-07-29)

## Goal

WP233 made the planar-reflection filter shader replaceable and build-purgeable,
but the standard camera transform and oblique-projection implementation still
lived in the renderer core. That left two problems:

1. `Renderer` knew the stable IDs of individual secondary-view techniques;
2. disabling the standard algorithm package still retained its C++ policy and
   object code.

WP234 moves runtime ViewFamily creation behind a generic provider registry and
places the standard planar implementation in the same optional package as its
shader. The graph scheduler, resources, temporal identity, and backend remain
engine mechanisms.

## Runtime boundary

Providers are keyed by stable `family_id` and receive immutable engine inputs:

- the compiled frame graph;
- already-authored families, including `$main`;
- current light data;
- render-target metadata.

A provider returns logical `RenderViewFamily` data only. It does not record
Vulkan commands, allocate graph resources, advance temporal state, or control
the scheduler. `Renderer` now asks one generic resolver for every graph-required
family instead of branching on `$shadow/directional` and
`$reflection/planar`.

Resolution order is deliberate:

1. copy caller-authored `$main` and secondary families;
2. for each still-missing graph-required family, invoke the registered
   provider for that stable ID;
3. reject unavailable families by name;
4. validate the resolved family contract before execution.

Caller-authored families therefore remain the simplest replacement path.
`Renderer::render(const RenderViewFamilies&)` exposes that path for flat
window/headless rendering, while `renderLogicalFrame` continues to serve custom
logical frame targets.

## Package split

The always-built mechanism lives under:

```text
src/core/renderer/
  viewfamilyproviderregistry.*
  directionalshadowviewprovider.*
```

Directional shadow now uses the registry too, removing the technique-specific
branch from `Renderer`, but its standard implementation remains engine-owned in
this slice.

The optional standard package owns:

```text
src/core/render_algorithms/
  standardrenderalgorithms.*
  planar_reflection/
    planarreflectionview.*
    planarreflectionviewprovider.*
```

`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF` omits that directory from the
target. The binary then contains neither the standard reflection shader nor
the standard reflection matrix/oblique-projection/provider objects. It retains
the generic registry and all logical/physical execution mechanisms.

To use planar reflection in such a build, a project supplies both:

- a project compute asset through `prefilter_shader`;
- `$reflection/planar`, either explicitly in `RenderViewFamilies` or through a
  source-built host/provider integration.

This is intentionally not a game-DLL hot-reload ABI yet. A future dynamic
provider surface must add ownership, generation publication, leases, and
rollback rather than exposing raw callbacks across an unload boundary.

## Shared parameter access

Feature parameter decoding moved from `Renderer` into generic typed accessors.
Directional shadow and optional planar providers now use the same checked
unsigned, float, and boolean conversion rules. This avoids giving each
algorithm package a slightly different interpretation of compiled feature
values.

## Verification

- registry tests cover builtin presence, standard-package absence, project
  registration, and duplicate-family rejection;
- standard-ON ViewFamily and reflection tests exercise the packaged provider;
- standard-OFF reflection rendering supplies a project-authored camera family
  and project shader through the unchanged Vulkan graph;
- directional-shadow Vulkan goldens pass in both standard-ON and standard-OFF
  builds through the generic provider resolver;
- build-unit smoke checks CMake metadata and artifacts for all standard
  reflection shader/provider/object names.

## Remaining boundaries

- provider registration is source-level and process-lifetime only;
- directional shadow policy is routed through the generic registry but is not
  yet part of the removable standard package;
- provider-to-provider dependencies are not a declared graph. Current standard
  providers depend only on `$main`;
- secondary multiview, point/spot cube families, and multiple planar
  reflection instances remain later vertical slices.
