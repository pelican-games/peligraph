# WP232 Planar reflection mip filter / material family-array ABI

Status: implemented and verified (2026-07-29)

## Goal

Close the standard planar-reflection sampling slice without coupling its
filter algorithm to Vulkan or forcing every `.surface` to describe the
physical image shape.

The slice has three deliberately separate parts:

1. **Typed capability layer**
   - `planar_reflection_color` is a 7-mip storage/sampled target.
   - compute and material ports carry access, subresource, sampling, and
     `family_array` contracts.
   - the graph owns ordering and read/write hazards.
2. **Replaceable algorithm layer**
   - `planar_reflection_filter.comp` performs one 2:1 linear low-pass step.
   - six ordinary compute tasks form mip 0 -> 1 -> ... -> 6.
   - dispatch groups are derived from each destination mip extent and the
     reflected shader local size.
3. **Physical/backend lowering**
   - explicit one-layer family subresources expand to the selected physical
     family cardinality.
   - Vulkan descriptors receive the matching 2D or 2D-array image view.
   - no planar-reflection name or filter rule is embedded in the generic
     material/compute backend.

## Standard graph

The capture passes write mip 0. The following nodes run before the canonical
`forward_transparent` pass:

```text
planar_reflection capture (mip 0)
  -> filter_mip_1
  -> filter_mip_2
  -> filter_mip_3
  -> filter_mip_4
  -> filter_mip_5
  -> filter_mip_6
  -> forward_transparent material sampling
```

Each task reads one explicit source mip and writes one explicit destination
mip through generated resource-port accessors. The standard 64 x 64 test
therefore dispatches `4x4`, `2x2`, then four `1x1` groups for an `8x8`
local size. Larger configured reflection resolutions keep the bounded
seven-level cost.

## Surface/material boundary

The existing WP207b separation remains authoritative:

- `.surface` declares the semantic resource name, kind, stage, and sampling
  algorithm.
- the material pass maps that name to a graph resource and owns
  `shared_2d` / `per_view` / `family_array`, sampler, footprint, history, and
  subresource policy.

The physical graph now feeds its normalized image-view ABI back into the
surface compiler. A `family_array` sampled material port produces
`sampler2DArray`, while the public scalar accessor keeps the authored
algorithm independent from that representation:

```glsl
vec4 current = pelican_sample_planar_reflection(uv);
vec4 explicit_view =
    pelican_sample_planar_reflection(uv, other_view_index);
```

The scalar overload selects `pelican_view_index()`. The indexed overload is
still available when an algorithm intentionally addresses another family
member.

A material route may use the same shader in both:

- a one-view secondary family physically represented by a scalar 2D image;
- a multi-view family represented by a 2D-array image.

The shader ABI is normalized to `sampler2DArray`. For the scalar case the
runtime creates a one-layer array view instead of compiling a second surface
ABI. Sampled/local-read kind, input-attachment index, and normalized image
view shape must agree across every active pass variant.

## Roughness sampling

The standard chain exposes mip count and explicit-LOD accessors:

```glsl
float lod = roughness * roughness *
    float(max(pelican_mip_count_planar_reflection(), 1u) - 1u);
vec3 reflected =
    pelican_sample_lod_planar_reflection(screen_uv, lod).rgb;
```

This is a bounded low-pass chain, not a GGX importance-sampled convolution.
Fresnel, normal distortion, BRDF-aware prefiltering, and project-specific
filter kernels remain replaceable material/compute algorithms.

## Verification

- feature composition verifies the 7-mip storage target, six ordered tasks,
  explicit mip subresources, family-array ports, and extent-derived groups;
- surface compiler tests verify sampled 2D-array generation, implicit current
  view access, reflection dimension checks, and local-read/layered ABI
  rejection;
- multiview execution verifies explicit one-layer subresources expand to a
  two-layer family descriptor;
- Vulkan planar-reflection golden verifies all six dispatches, non-zero final
  mip bytes, transparent material compilation to `sampler2DArray`, scalar
  secondary-family adaptation, and final rendering.

## Remaining boundaries

- the standard feature fixes the chain at seven levels;
- the filter is a linear box low-pass rather than BRDF-aware convolution;
- whole-image layout/hazard tracking remains conservative across disjoint
  mips;
- general consumer-owned sampled layered-multiview inputs, secondary
  multiview execution, cube providers, and multi-plane/probe selection remain
  later work.
