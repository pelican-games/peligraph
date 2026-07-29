# WP233 Replaceable render algorithm package

Status: implemented and verified (2026-07-29)

## Goal

WP232 proved that the graph can generate and sample a planar-reflection mip
chain, but its standard compute shader was still selected by a literal engine
reference and registered in feature-specific C++ lines. WP233 separates the
three responsibilities:

1. the graph owns resources, typed ports, scheduling, hazards, and dispatch;
2. a feature instance selects an algorithm asset through a typed parameter;
3. the standard implementation is an optional package rather than part of
   the generic mechanism.

This is a dogfood slice for the wider rule “mechanism in the engine,
technique in a removable package.” It is not only a reflection-quality
feature.

## Typed algorithm selection

`pelican.render_feature_parameters` v1 now accepts `shader_assets`:

```json
{
  "shader_assets": [{
    "name": "prefilter_shader",
    "stage": "compute",
    "default":
      "engine://render_algorithms/planar_reflection/standard_prefilter"
  }]
}
```

An instance can replace only that asset:

```json
{
  "ref": "engine://features/planar_reflection.json",
  "parameters": {
    "prefilter_shader": "project://shaders/my_planar_prefilter",
    "prefilter_radius": 1.25
  }
}
```

The placeholder is legal only in a typed shader slot. A compute declaration
used in `shader.fragment`, a non-string instance value, or `$name` in an
untyped field fails during feature composition. The resolved reference is
recorded in `feature_instances`, so plan inspection shows which algorithm was
selected.

This deliberately does not introduce a universal algorithm-provider ABI.
Shader kernels use asset parameters; stateful CPU policy such as draw sorting
continues to use a provider registry; execution-mechanism replacement remains
a compiler/backend boundary.

## Shader-facing contract

The standard and project-owned prefilters consume the same ports:

- sampled `source_color`;
- storage `filtered_color`;
- `dispatch.groups_from.port = filtered_color`;
- `schedule = per_view`.

Generated image interfaces now expose:

```glsl
uint pelican_base_mip_filtered_color();
uint pelican_base_layer_filtered_color();
```

These are normalized graph values, not Vulkan handles or task-name
conventions. Feature scalar defines also reach compute shader compilation and
the pipeline recipe. A replacement can therefore use
`PELICAN_FEATURE_PLANAR_REFLECTION_PREFILTER_RADIUS` without a second
parameter transport.

## Standard package and purge boundary

The built-in kernel lives below:

```text
src/core/resources/render_algorithms/
  standard_algorithms.cmake
  planar_reflection/standard_prefilter.comp
```

The package manifest generates both embedding and engine-resource registry
entries. `engineresources.cpp` has no planar-prefilter-specific registration.

`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF` removes the standard asset from
the embed target and engine registry. It intentionally keeps the graph
compiler, compute runtime, typed resource ports, and planar camera provider.
Any planar-reflection feature instance in such a binary must select a
`project://` prefilter; otherwise the unavailable default engine reference
fails by name.

The standard kernel is a normalized 13-tap tent downsample whose radius grows
with destination mip. It reduces shimmer more effectively than the former
single bilinear sample, but remains a planar screen-space approximation. It
is not described as GGX or direction-space BRDF convolution.

## Verification

- feature composition tests cover defaults, project overrides, resolved
  metadata, scalar defines, and stage/type misuse;
- shader compiler/reflection tests compile the generated base-mip/base-layer
  accessors;
- the standard-ON Vulkan reflection golden compiles and runs the packaged
  kernel, then runs a second graph instance with a project-owned kernel;
- a standard-OFF build has an empty optional-resource registry and passes the
  same Vulkan reflection golden using only project kernels;
- the build-unit smoke matrix checks that OFF builds retain neither the
  standard source in build metadata nor a generated object/registry entry.

## Remaining boundaries

- the six-task, seven-level chain is still the standard feature recipe; a
  project that needs an adaptive chain must replace or author the feature
  graph, not only the shader;
- the planar `RenderViewFamily` camera provider remains core code and is not
  part of this package;
- whole-image hazard tracking stays conservative across distinct mips;
- a physically based prefilter needs direction/normal/BRDF inputs and a
  different algorithm, which the new asset boundary permits but does not
  implement;
- shader assets cover `vertex`, `fragment`, and `compute` slots only. Surface
  assets, material schemas, compiler programs, and backend-native scopes keep
  their existing dedicated extension boundaries.
