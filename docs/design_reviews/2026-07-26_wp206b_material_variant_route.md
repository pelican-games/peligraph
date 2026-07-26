# WP206b small design review: pass-local named material variants

Status: implemented and verified (2026-07-26)

## Problem

An authored object must be able to participate in its normal base pass and in a
second material pass with another `.surface` and its `render_state`, without
duplicating the entity, mesh, draw identity, or author-visible material.
Inverted-hull outlines are the first dogfood, but `outline` must not become an
engine pass kind, shader name, or material flag.

A shader-only override is not sound. Generated `.surface` parameter accessors
index `MaterialBuffer` with `pelicanPush.materialIndex`; two surfaces may have
different std140 layouts, values, textures, and screen-input descriptors.
Reusing the base material index would silently reinterpret bytes.

## Public vocabulary

`pelican.material` v1 adds a generic `variants` object to a material:

```json
{
  "name": "hero",
  "tags": ["outlined"],
  "surface": "project://surfaces/hero.surface",
  "variants": {
    "silhouette": {
      "surface": "project://surfaces/silhouette.surface",
      "render_path": "forward",
      "values": {"width": 0.015, "color": [0.02, 0.02, 0.03, 1.0]}
    }
  }
}
```

A material pass selects a user-defined name:

```json
{
  "name": "silhouette_overlay",
  "type": "material",
  "material_contract": "forward_opaque_v1",
  "material_filter": {"include": ["outlined"]},
  "material_variant": "silhouette"
}
```

The variant name is opaque engine data. It has no technique semantics.
The variant reuses the base PBR factors and fixed texture slots, but owns its
surface, defines, custom values, custom textures, route, shader pair, pipeline,
material descriptor, and pass-input descriptors. Variant declarations cannot
add tags, another variant tree, raw shader, routing aliases, or an exact pass.

`material_variant` requires an explicit `material_contract` and a non-empty
`material_filter.include`. This prevents an accidental global overlay and makes
missing-variant diagnostics attributable to a bounded selected set.

### Version policy

This is an optional addition to the one current `pelican.material` version,
which remains version 1 while the format is pre-release. The parser still
requires the version field and accepts exactly `1`; this work does not add a
legacy branch, version range, implicit upgrade, or missing-version fallback.
When the current material version is advanced, acceptance of version 1 must be
removed in the same change, following `implementation_plan.md` §0.

`material_variant` is a field in the already-versioned rendering configuration,
not a new independently versioned envelope. Runtime named variant resources are
internal typed state and therefore do not invent a second schema version.

## Lowering and runtime identity

The project layer lowers every named variant as a complete `LoweredMaterial`
using the existing surface parser, route resolver, capability checks, std140
packer, and texture binding logic. No render-state parser or Vulkan-state table
is duplicated.

At runtime:

- the base keeps its `GlobalMaterialId`, GPU record, descriptor, and pipeline
  unchanged;
- each named variant receives an internal material resource and GPU record;
- registration copies the base's fixed PBR factors, four fixed texture slots,
  and deformation identity into every internal resource, so a loader cannot
  accidentally turn inheritance into caller convention;
- a material pass resolves `(base material id, variant name)` to that internal
  resource before binding;
- the draw list, indirect command, mesh, instance identity, source-material
  index, and sort/filter result remain those of the base draw;
- the material push constant uses the resolved internal GPU record index.

The internal resource is an implementation detail, not a second scene material.
It is retired with its base generation.

WP206b permits route changes within the base draw's phase, notably
`deferred_geometry` to `forward_opaque`. A variant may not cross
opaque/transparent phase boundaries yet: that would require compiling its
draws with the variant's own sort policy instead of reusing the base draw
queue. Registration rejects such a declaration rather than rendering it in an
incorrect order. A later variant-aware queue slice can remove this restriction
without changing the material/pass vocabulary.

## Validation and diagnostics

- duplicate/invalid variant names and duplicate registration are hard errors;
- a variant pass whose selected draw lacks the named variant is a hard error
  naming both pass and variant;
- the variant's resolved route/shader contract must be accepted by the pass's
  explicit material contract, so deferred/forward/transparent mismatches fail
  through the existing route validator;
- frame-graph resource hazards and named `after`/`before` edges remain the
  ordering authority; no numeric outline order is added;
- the frame-plan dump carries the selected variant name alongside the stable
  tag-filter identity.

## Reload and ownership

Variant shaders are ordinary reloadable surface shader bundles. Existing
ShaderLibrary/PipelineFactory candidate publication therefore owns their Vulkan
pipeline generation and keeps shader/pipeline failure atomic.

Reloadable material-value bindings may name either the base or one of its
variants. The existing HR1-M/HR2-S transaction validates and publishes the
variant's independent layout/value payload. A failed surface, route, layout, or
pipeline candidate leaves both base and variant live resources unchanged.

## Compatibility

Base-only materials take the old registration and render paths. They allocate
no variant resource, perform no additional descriptor update, and retain their
GPU record bytes. Flat, preview, sequential XR, and multiview all consume the
same `PassDefinition::material_variant`; view handling remains in the existing
pass-input descriptor path.

## Verification

- project-owned inverted-hull dogfood renders a deferred base and a front-cull
  forward overlay from one entity, mesh, base material, and draw identity;
- parser, lowering, pass validation, frame-plan, graph-variant, hot-reload, and
  GPU-image regression suites pass;
- the complete Debug build and all 906 CTest entries pass;
- the pre-existing Windows symlink-permission PathResolver case is the only
  skipped test.
