# WP231b Remaining-mip material resource ports

## Outcome

Shader resource ports can expose a mip range whose concrete count follows the
current render target:

```json
"subresource": {
  "mip": 0,
  "mip_count": "remaining",
  "layer": 0,
  "layer_count": 1
}
```

`remaining` is resolved to `target.mip_levels - mip` immediately before a
Vulkan image view is cached or created. A target authored with
`mip_levels: "full"` therefore remains correct when a resize changes its
physical mip count.

The contract is shared by fullscreen, compute, and material sampled image
ports. Material resources now carry the selected subresource into their set-1
descriptor. Existing base-view behavior remains the default.

## Invariants

- Storage image views still require one fixed mip.
- Buffer ports cannot declare image subresources.
- A range outside the current mip/layer capacity is rejected.
- `remaining` participates conservatively in overlap checks.
- Material subresource ports remain sampler reads; they cannot request the
  `same_pixel` input-attachment optimization.
- Sequential per-view descriptors adjust the selected array layer per
  invocation while retaining the authored mip range.
- Material resource samplers no longer clamp LOD to zero. Their mip filter
  follows the declared nearest/linear sampling policy.
- Descriptor cache keys include mip, count mode, layer, and layer count.

## Verification

- `pelican_test_renderingpass_helpers_test`: 540 assertions / 69 test cases.
- `pelican_test_headless_render_test "[material-resource]"`: 40 assertions /
  1 Vulkan test case.
- The Vulkan fixture renders mip 0 red, writes mip 1 green with a compute task,
  binds both mips through `mip_count: "remaining"`, queries
  `pelican_mip_count_*()` in a material surface, explicitly samples the last
  mip, and verifies the resulting displaced geometry is green.
- The same fixture recreates render targets and performs a graph hot reload,
  verifying that the remaining-mip view is rebound to the new generation.

## Follow-up

WP231 and WP231b now provide the generic dispatch and sampling contracts needed
for a replaceable planar-reflection mip filter chain. The next slice can add
the standard compute filter without hard-coding resolution or material BRDF
policy into the Vulkan backend.
