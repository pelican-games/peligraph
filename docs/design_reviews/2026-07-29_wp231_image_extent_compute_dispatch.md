# WP231 Image-extent compute dispatch

## Outcome

Compute tasks can derive direct dispatch groups from a typed image resource
port:

```json
{
  "resource_ports": {
    "reduced_depth": {
      "resource": "depth_pyramid",
      "access": "storage",
      "subresource": {"mip": 1}
    }
  },
  "dispatch": {
    "groups_from": {"port": "reduced_depth"}
  }
}
```

The runtime resolves the port to a render target and selected mip, reads the
compute workgroup size from shader reflection, and emits:

```text
groups.x = ceil(max(1, extent.width  >> mip) / local_size.x)
groups.y = ceil(max(1, extent.height >> mip) / local_size.y)
groups.z = 1
```

The values are recomputed when render targets are rebound. This covers output
resize and compute shader hot reload without an authored duplicate of the
workgroup size.

## Contract decisions

- `groups`, image `groups_from`, and `indirect` are mutually exclusive.
- `groups_from.port` must name an existing typed image resource port.
- The port's `subresource.mip` is authoritative; the dispatch declaration
  does not repeat the mip.
- The compute shader's `layout(local_size_*=...)` is authoritative.
  `dispatch.local_size` is rejected.
- Image-derived dispatch is 2D and requires `local_size_z == 1`.
- `setDispatchGroups()` cannot override a derived task because that override
  would become stale at the next resize.
- Scalar-count dispatch remains a separate problem. GPU-produced variable
  counts use the existing typed indirect dispatch contract.

## Verification

- `pelican_test_renderingpass_helpers_test`: 533 assertions / 69 test cases.
- `pelican_test_headless_render_test "[depth-pyramid]"`: 57 assertions / 1
  Vulkan test case.
- The Vulkan test verifies initial mip-derived groups, rejects a manual
  override, resizes from 32x32 to 64x32, verifies recomputed groups, renders the
  expected mip result, and keeps the existing shader hot-reload path working.

## Follow-up

This is the dispatch foundation for the planar-reflection roughness mip chain.
The next slice must expose an explicitly requested full mip chain to sampled
material ports, then add replaceable compute filter tasks to the standard
planar reflection feature.
