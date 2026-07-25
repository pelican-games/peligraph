# Upscale resolution contract vertical slice

Date: 2026-07-25

## Outcome

The render pipeline now treats scene render resolution and final output
resolution as separate typed values. A fixed spatial upscaler remains an
ordinary replaceable fullscreen pass; the engine supplies contracts and does
not own the upscale algorithm.

This slice adds:

- a physical `ResourceExtentPlan` on image resources (`output_relative` or
  `fixed`);
- a compiled `VulkanRenderResolutionPlan` naming the scene render source and
  output source;
- automatic pass resolution domains, with an override for unusual graphs;
- a frame-resolution UBO containing both render and output extent;
- projection jitter normalized against the scene render extent;
- per-image fullscreen sampler policy;
- a headless 16x16 to 32x32 nearest-neighbor GPU regression.

## Authoring

Normal material and velocity passes are classified as `scene` automatically.
Shadow passes are `independent`; UI and output-transform passes are `output`.
Fullscreen and debug passes are unclassified unless the graph author labels
them:

```json
{
  "name": "produce_low_resolution_scene",
  "type": "fullscreen",
  "resolution_domain": "scene",
  "output": {"color": "low_color", "depth": null}
}
```

Allowed override values are `scene`, `output`, `independent`, and
`unclassified` (`none` is accepted as an alias). Every image written by
`scene` passes must resolve to the same physical extent. A mismatch is a
compile-time error naming both resources.

Fullscreen image inputs can opt into a sampling policy:

```json
{
  "name": "upscale",
  "type": "fullscreen",
  "input": ["low_color"],
  "input_sampling": [
    {"filter": "nearest", "address": "clamp_to_edge"}
  ]
}
```

`input_sampling` maps one-to-one to image inputs, not buffer inputs. Filters
are `linear` and `nearest`; address modes are `repeat`, `mirrored_repeat`, and
`clamp_to_edge`. Omitting the field preserves the previous `linear/repeat`
behavior.

## Shader contract

`pelican_frame.glsl` exposes:

```glsl
pelicanResolution.render_resolution // (width, height, 1/width, 1/height)
pelicanResolution.output_resolution // (width, height, 1/width, 1/height)
```

The block is set 0, binding 4 and supports scalar and multiview layouts. The
existing `pelicanFrame.resolution` remains output-relative for compatibility
with current UI and sprite shaders.

## Compiler/runtime boundary

`extent_scale` and fixed target extents are copied into the disposable target
lowering graph and the final Vulkan physical-resource plan. The runtime
resolves the compiled scene source through the immutable frame-graph target
bindings, checks its actual image extent against the plan, and uploads both
extents before any pass executes.

The projection-jitter provider receives the compiled scene render extent. A
-0.5 pixel sample on a 16-pixel render target therefore becomes -0.0625 NDC
even when the output is 32 pixels wide.

## Deliberate limits

This is the foundation for, not an implementation of, temporal or
vendor-specific upscalers. Dynamic resolution, mip-bias policy, reactive and
transparency masks, exposure, history reset rules for scale changes, and
vendor SDK capability negotiation remain later work. Those additions can use
the same resolution plan and replaceable pass/provider boundaries without
hard-coding an upscale algorithm into the renderer.

## Verification

- CPU parser/domain/physical-plan tests
- shader reflection test for set 0 binding 4
- existing multiview execution test
- headless GPU test:
  - materializes `low_color` at 16x16 for a 32x32 output;
  - verifies the compiled source and uploaded render/output UBO;
  - verifies jitter uses 16x16;
  - verifies nearest/clamp sampling produces exact 2x texel replication.
