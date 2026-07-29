# WP236 Runtime cube render targets

Status: implemented and verified (2026-07-30)

## Goal

Static KTX2 cubemaps could already be sampled, and WP235 made individual
runtime image layers valid raster attachments. Runtime render targets,
however, were still always ordinary 2D images. A six-layer target could be
rendered face by face, but it could not legally expose a Vulkan cube view to
fullscreen, compute, or material shaders.

WP236 adds a cube-compatible runtime image shape without coupling it to
ViewFamily scheduling. The same physical image exposes 2D face views for
raster output and a cube view for direction-space sampling.

## Public contract

A single runtime cubemap is declared with a fixed square extent:

```json
{
  "name": "environment_capture",
  "dimension": "cube",
  "width": 256,
  "height": 256,
  "extent_scale": 1.0,
  "format": "R16G16B16A16_SFLOAT",
  "mip_levels": "full",
  "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
}
```

`layers` may be omitted and then means six. If present, it must be exactly
six. Output-relative extents and non-square fixed extents are rejected because
one cube requires six square faces with one stable size.

A raster pass writes one face with the WP235 attachment vocabulary:

```json
"output": {
  "color": {
    "target": "environment_capture",
    "subresource": {"mip": 0, "layer": 3}
  },
  "depth": null
}
```

A fullscreen, compute, or material sampled resource port consumes all six
faces as a cubemap:

```json
"resource_ports": {
  "environment": {
    "resource": "environment_capture",
    "access": "sampled",
    "view": "cube",
    "subresource": {
      "mip": 0,
      "mip_count": "remaining",
      "layer": 0,
      "layer_count": 6
    }
  }
}
```

The generated object is `samplerCube`. Its primary accessors are
`pelican_sample_environment(vec3 direction)` and
`pelican_sample_lod_environment(vec3 direction, float lod)`. The port-local
view count is six faces; the global `pelican_view_count()` remains the
ViewFamily execution cardinality.

Startup validation rejects:

- a cube target without a fixed square extent;
- any cube target layer count other than six;
- a cube resource port bound to an ordinary 2D target;
- storage or same-pixel/input-attachment access through a cube view;
- a cube descriptor subresource that does not select all six aligned faces;
- aliasing or runtime publication that changes the declared resource shape.

Cube arrays and runtime 3D images remain outside this slice.

## Type and compiler boundary

`ImageResourceDimension` describes physical image shape (`two_d` or `cube`).
`ImageSubresourceViewDimension` describes the concrete descriptor or
attachment view (`two_d`, `two_d_array`, or `cube`). These are intentionally
separate from `VulkanResourceViewLayout`, which describes how a logical
ViewFamily is represented (`shared_2d`, sequential, or family array).

This separation prevents a cube from becoming a fake six-view camera family.
Face scheduling remains an ordinary user-authored pass/family decision, while
descriptor lowering can select a cube view independently.

The target dimension is retained through:

- render-target definition and metadata;
- resource-pattern and target lowering;
- Vulkan physical resource plans and canonical JSON;
- sample-count/device format queries;
- alias compatibility and automatic-plan fingerprints;
- runtime target registration, resize, and image-view caching.

Cross-variant target merge requires one dimension. Two otherwise identical 2D
and cube targets are not alias-compatible.

## Vulkan runtime

Cube targets are Vulkan 2D images with `arrayLayers = 6` and
`vk::ImageCreateFlagBits::eCubeCompatible`. Raster output requests a typed
`e2D` view of one selected face. Sampled resource lowering requests an
`eCube` view spanning six faces and the selected mip interval.

The cube shape participates in device image-format-property queries, so an
unsupported format/usage/sample combination fails during physical planning
rather than during descriptor creation.

Fullscreen, compute, and material paths keep descriptor shape separately from
their scheduler view layout. Surface compilation emits a compiler-owned cube
physical define, reflects `samplerCube`, and hot reload revalidates the same
ABI before publication.

## Verification

- parser tests cover the default six layers, explicit cube spelling, unknown
  dimensions, non-square/dynamic extents, invalid layers, and sampled-only
  cube ports;
- shader compiler tests compile and reflect generated `samplerCube`
  accessors and reject resource-shape mismatches;
- surface compiler tests cover cube physical variants and reject cube plus
  local-read or layered-array combinations;
- target-planning tests prove dimension propagation, JSON serialization,
  alias identity, and invalid physical shapes;
- runtime bridge tests prove cube device queries and publication preserve
  shape while a pass targets one face;
- a headless Vulkan test creates a three-mip cube target, verifies
  `eCubeCompatible`, binds its exact cube view to a compute descriptor, checks
  view-cache identity, and resolves a non-zero-mip face attachment with the
  correct extent.

## Remaining boundaries

- face camera generation, point/spot shadow policy, reflection-probe
  inventory, and prefilter quality remain replaceable algorithm/provider work;
- cube arrays and runtime 3D images are not public;
- cube storage-image views are not public;
- hazards, layouts, and lifetimes remain resource-wide;
- cube face attachment selection is explicit; this slice does not introduce
  an engine-owned six-pass capture loop.
