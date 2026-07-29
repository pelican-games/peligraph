# WP235 Raster attachment subresource views

Status: implemented and verified (2026-07-29)

## Goal

WP209b and WP231b exposed mip/layer ranges for fullscreen, compute, and
material image inputs, but raster output still meant only “this render
target.” A pass could not render directly into a non-zero mip or an authored
array-layer interval. That blocked raster mip generation and made future
cube-face capture depend on an implicit layer convention.

WP235 carries one typed attachment-view value through authoring, planning, and
Vulkan execution while preserving the existing string shorthand.

## Public contract

An output attachment is now:

```text
(GlobalRenderTargetId, optional ImageSubresourceRange)
```

Legacy JSON remains valid:

```json
"output": {"color": "scene_color", "depth": null}
```

An exact raster view uses the shared image-subresource vocabulary:

```json
"output": {
  "color": [{
    "target": "capture",
    "subresource": {
      "mip": 1,
      "layer": 2,
      "layer_count": 2
    }
  }],
  "depth": null
}
```

Raster output selects exactly one fixed mip. Its `layer_count` must equal the
logical view count of the pass. Sequential execution selects
`base_array_layer + view_index` as a 2D view; multiview execution binds the
whole interval as a 2D-array view. The selected mip determines render area,
viewport, and scissor.

Startup validation rejects:

- a swapchain subresource;
- an out-of-range or multi-mip range;
- attachment extents that differ after mip selection;
- the same target in more than one attachment slot;
- a layer count that differs from the pass view count;
- a non-zero mip on an MSAA render target.

The MSAA restriction is structural: the separate multisample attachment image
has mip zero only, even when its single-sample resolve image has a mip chain.

## Compiler and physical-plan boundary

`RasterAttachmentView` is retained in `PassDefinition` and
`CompiledPassRenderingContract`. The frame graph and
`VulkanPhysicalAttachmentPlan` carry the same optional range. Physical
attachment operation lookup, materialized-scope fusion, scope contracts, and
automatic-plan fingerprints compare the full `(resource, range)` identity.
Plan JSON serializes the range, so inspection and ejected-plan compatibility
do not silently collapse distinct views.

The direct frame-graph JSON path accepts the same object form. Read/write
dependencies intentionally remain resource-wide: distinct non-overlapping
mips or layers do not gain parallel scheduling in this slice.

Explicit raster views also remain materialized. The current same-pixel input
contract identifies a logical resource but cannot identify the exact raster
range. Excluding these attachments from tile-local selection avoids fusing a
consumer with the wrong producer view. A later subresource-aware local-read
port can remove this conservative restriction.

## Vulkan runtime

The render-target container now caches attachment image views by typed range.
Single-sample output reuses the existing general subresource-view cache;
multisample output has a corresponding cache over the attachment image.
Legacy attachments keep their existing layer/layered cached views so handle
identity and the old implicit view mapping remain unchanged.

The runtime resolves:

- sequential output to one 2D image view per invocation;
- multiview output to one 2D-array image view;
- resolve output to the identical range in the single-sample image;
- render extent from the chosen mip.

Layout transitions remain whole-image and conservative.

## Verification

- parser tests cover legacy and object authoring, unknown fields, swapchain
  rejection, and multi/remaining-mip rejection;
- frame-planner tests prove direct JSON and `PassDefinition` preserve the
  range;
- runtime-compiler tests prove physical attachment operations retain exact
  subresource identity and reject view-count mismatches;
- target-planning tests validate and serialize physical ranges and reject
  bounds/MSAA violations;
- a headless Vulkan test creates a three-mip, four-layer target and verifies
  mip-one sequential layer views, a mip-one multiview array view, and the
  halved render extent.

## Remaining boundaries

- runtime targets are still 2D/2D-array images; cube-compatible and 3D target
  creation/view dimensions are a later slice;
- hazards, layouts, and lifetimes are resource-wide rather than
  subresource-wide;
- explicit raster views are not tile-local until logical local-read inputs
  can name the matching range;
- depth/stencil aspect slicing is not separately authored;
- non-zero-mip MSAA would require a different attachment allocation contract,
  not a relaxed validation check.
