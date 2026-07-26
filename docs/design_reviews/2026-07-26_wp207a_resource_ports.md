# WP207a design and implementation report: named image resource ports

Status: implemented and verified (2026-07-26)

## Problem

Compute shaders previously had two unrelated limitations:

- set 0 was present in every pipeline layout, but compute dispatch did not bind
  the active `FrameResources` descriptor set;
- set 1 render-target descriptors were storage images only, so a compute shader
  could not consume an ordinary sampled color/depth result without pretending
  it was storage.

Raw fullscreen shaders also had to keep JSON input order and numeric
`layout(set = 1, binding = N)` declarations synchronized by hand. Reordering an
input could therefore preserve compilation while changing its meaning.

WP207a adds one named image-port vocabulary shared by fullscreen and compute.
It does not rename `input` or `reads`/`writes`, and it does not make the port
declaration a second dependency graph.

## Public authoring contract

`resource_ports` is an optional object on a fullscreen pass or compute task:

```json
{
  "reads": ["scene_color"],
  "writes": ["filtered_color"],
  "resource_ports": {
    "source": {
      "resource": "scene_color",
      "access": "sampled",
      "view": "shared_2d",
      "sampling": {
        "filter": "linear",
        "address": "clamp_to_edge"
      }
    },
    "destination": {
      "resource": "filtered_color",
      "access": "storage"
    }
  }
}
```

The object key is a GLSL identifier and becomes the accessor suffix. A string
value is shorthand for `{"resource": "<value>"}`. `resource` must already
appear in fullscreen `input` or compute `reads`/`writes`; a port never creates
an edge, producer, lifetime, or ordering constraint.

`access: automatic` is the default. For image ports it resolves to `sampled`
for a read-only resource and `storage` for a written resource. `sampling` is
legal only for sampled ports. Fullscreen ports are sampled-only and replace,
rather than combine with, legacy `input_sampling`.

The shader includes:

```glsl
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
```

Generated sampled accessors are:

```glsl
vec4 pelican_sample_<port>(vec2 uv);
ivec2 pelican_size_<port>();
```

For a per-view array consumed once by compute or by multiview graphics, sampling
also takes `uint view_index` and exposes
`uint pelican_view_count_<port>()`. Storage ports expose the corresponding
`pelican_load_<port>`, `pelican_store_<port>`, size, and optional view-count
functions.

The generated descriptor variable is deliberately private-looking:
`pelican_resource_<port>`. Pipeline reflection verifies its exact set, binding,
descriptor kind, count, name, and 2D/2D-array dimension. Diagnostics name both
the authored port and logical resource.

## View contract

`view` describes the shader-visible meaning, not a Vulkan image-view command:

| Port view | Physical view | Sequential fullscreen | Multiview fullscreen | Compute dispatched once |
|---|---|---:|---:|---:|
| `shared_2d` | `shared_2d` | `sampler2D` | `sampler2D` | `sampler2D` / `image2D` |
| `per_view` | `sequential_2d` | per-invocation `sampler2D` | reject | `sampler2DArray` / `image2DArray` |
| `per_view` | `layered_2d_array` | per-invocation `sampler2D` | `sampler2DArray` | `sampler2DArray` / `image2DArray` |

A shared port cannot silently consume a per-view resource, and a per-view port
cannot silently collapse a shared resource. This keeps the logical declaration
portable while allowing the physical target compiler to choose sequential or
layered execution.

## Lowering and runtime ownership

- `reads`, `writes`, `input`, `after`, and `before` remain the scheduling
  authority.
- The frame-planner shadow graph carries only the port's sampled/storage access
  intent beside those existing resources.
- Target plans remain the authority for physical shared/sequential/layered
  views. A compute task used by more than one plan must receive compatible
  physical views.
- The runtime assigns bindings in the existing descriptor order and generates
  a virtual include from the resolved logical names.
- `PipelineFactory` stores the generated interface in its pipeline descriptor,
  so every hot-reload rebuild repeats reflection validation before publication.
- Descriptor rebind after render-target recreation uses the stored typed
  interface and selects the current/history surface and 2D/array view again.

Storage image generation currently accepts the render-target formats supported
by the engine's normal data targets (`R8_UNORM`, `R16G16_SFLOAT`,
`R8G8B8A8_UNORM`, and `R16G16B16A16_SFLOAT`) plus the corresponding R32 float
forms available through lower-level physical authoring. Unsupported formats
fail with the port and resource names instead of emitting an invalid GLSL
layout qualifier.

## Frame and synchronization contract

`FrameResources::bindCompute` binds the same active set 0 used by graphics.
Project compute source can include the reserved in-memory
`pelican_frame.glsl` contract and read `pelicanFrame`, `pelicanResolution`, and
`pelicanLights`. The virtual include closure is engine-owned, so project source
does not depend on the engine repository's physical include path.

Sampled compute images use `eShaderReadOnlyOptimal` and combined image samplers;
storage and raw compute images keep `eGeneral`. Shader-read-only transitions
cover fragment and compute stages. Samplers are created lazily for the six
filter/address combinations, so a configuration without typed sampled compute
ports creates none.

## Compatibility and intentional limits

- An empty `resource_ports` declaration takes the old raw ABI unchanged.
- Raw fullscreen storage-buffer inputs and raw compute buffer/image layouts
  remain the C-layer escape hatch.
- A feature that does not declare ports performs no additional descriptor
  allocation/update and no sampled-compute sampler creation.
- Typed frame-graph buffers and material vertex/fragment consumers are
  intentionally deferred to WP207b, where element schema and stage visibility
  can be designed together.
- Dispatch counts remain authored constants. Indirect dispatch belongs to
  WP210.
- Generated ports require GLSL source compilation. Offline shaderc-OFF delivery
  remains WP211 rather than adding a second runtime ABI here.

## Verification

- Parser and logical-lowering tests cover name validation, sampled/storage
  intent, and the rule that ports do not create graph edges.
- Shader tests compile the generated include and verify descriptor type, exact
  name, 2D/array reflection, mismatch diagnostics, and shared/sequential/
  multiview resolution.
- Shader-library tests prove generated virtual includes survive hot reload.
- The existing compute headless GPU scenario now executes
  graphics color -> typed fullscreen sample -> typed compute sampled image
  (using Frame/Light/time) -> typed storage image -> raw storage buffer ->
  raw fullscreen buffer presentation. It emits no Vulkan validation error.
- The legacy fullscreen-buffer portion of that scenario is unchanged.
- The complete Debug build and all 911 CTest entries pass. The one
  Windows-symlink-privilege-dependent PathResolver case remains the existing
  Catch2 skip.
