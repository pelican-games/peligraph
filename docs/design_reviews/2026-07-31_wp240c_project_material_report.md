# WP240c: project material declaration and runtime loading

## Outcome

`pelican.asset_data` v1 now indexes project-owned `pelican.material` v1
documents through a path-only `materials[]` array. Startup parses the referenced
surface documents, lowers each material, compiles its shaders, registers the
base material and named variants on the GPU, and publishes one project-wide
named-material resolution domain.

Primitive assignment reuses `pelican.material_bindings` v1. No scene material
component, patch syntax, material-format version bump, or new binding version
was introduced.

## glTF producer convergence

Implementation range 1 landed first and independently as `4fa1586`. glTF core
materials no longer construct a partial `MaterialInfo` beside the normal
material compiler. They are adapted to a surface document and run through
`lowerGltfMaterial` and `applyLoweredMaterialForRoute`.

The six routing states map one-to-one to
`{opaque,mask,blend}_{single,double}`. Consequently `alphaMode`,
`alphaCutoff`, and `doubleSided` come from the shared lowering result rather
than new interpretation branches in `gltf.cpp`. MASK uses the wrapper discard
path; BLEND selects `forward_transparent`.

The glTF adapter preserves the established metallic/roughness lighting and
packed core textures. Compatible opaque glTF materials remain eligible for the
standard deferred model. The only intended golden change was six backface
pixels in each of `taa_static` and `taa_object_motion`: the shared route now
honors `doubleSided: true` in the committed ground fixture.

## Asset index and runtime owner

`asset_data.json` is now strict:

```json
{
  "schema": "pelican.asset_data",
  "version": 1,
  "models": [],
  "materials": [
    {"path": "materials/coat.material.json"}
  ]
}
```

The current version is the only accepted form. Unknown top-level keys and
unknown material-entry keys fail, and the old unversioned envelope is not
retained as a compatibility branch.

`ProjectMaterialAssetContainer` is initialized after the rendering pipeline
and before model assets. This order lets material shader compilation consume
the active pipeline defines and makes the project material registry available
when model binding resolution begins. The runtime chain is:

1. parse `pelican.asset_data` and material documents;
2. resolve and parse every referenced `.surface`;
3. `lowerMaterial` / `lowerMaterialVariants`;
4. compile the surface shaders with the active material pipeline defines;
5. apply the lowered route and resolve textures;
6. register base materials, variants, values reload, and texture reload;
7. publish name, GPU material ID, and routing metadata to model binding.

## Texture and reload path

The permanent resolver recognizes the standard semantic textures
`engine://textures/{white,black,flat_normal,flat_gray}`. Other project
references go through `PathResolver` and `registerReloadableTextureFile`, so a
logical texture is registered once and participates in the existing
generation-safe texture reload path.

Material documents register their base and named-variant value bindings with
`registerReloadableMaterialValuesFile`. Same-layout value changes and
referenced texture changes therefore update the live project material without
test-only callbacks. Surface, routing, or document-topology changes remain
outside the values-only reload contract and continue to fail rather than
silently changing a live pipeline layout.

## Binding resolution and failures

Binding resolution now searches two explicit domains: the model's glTF named
materials and the project material registry. It validates the domains before
applying a binding, so a glTF/project collision fails even if the binding
document does not happen to mention the colliding name.

The following are named hard errors:

- duplicate project material names across documents;
- duplicate names within either resolution domain;
- a name present in both glTF and project domains;
- a missing binding target;
- missing routing metadata;
- disagreement between `binding.routing` and the resolved material route.

When `materials[]` is absent, the project domain is empty and the existing
glTF-only resolution result is unchanged.

## Runtime and regression evidence

The U-USD0c `usd0c_openpbr_material` golden now stages the real strict asset
index, material document, texture, GLB, and binding document. It obtains the
material through `ProjectMaterialAssetContainer`; the former harness-side
material registration and rebinding shortcut is gone. The rendered result and
execution trace therefore cover project declaration through actual OpenPBR
draw.

Focused tests additionally cover:

- all six glTF alpha/double-sided routing states and MASK shader discard;
- BLEND selection of the transparent phase;
- strict `pelican.asset_data` v1 parsing;
- duplicate project names and glTF/project collisions;
- exact binding-routing consumption;
- project material values and referenced texture hot reload;
- projects with no `materials[]`.

The Windows WSI fault-injection integration previously used
`projects/example`, whose hand-authored legacy pipeline has no forward material
route. Correct glTF BLEND lowering exposed that accidental fixture dependency
before any WSI fault could run. The WSI test now uses the self-contained
`animgraph_demo` hybrid project; its six ordered surface/swapchain faults and
all original assertions are unchanged.

## Validation

- Debug all-target build: passed.
- CPU gate (`ctest -C Debug -LE gpu`): 944 / 944 passed; one existing
  environment-dependent symlink case skipped.
- GPU gate (`ctest -C Debug -L gpu`): 125 / 125 passed; four pre-existing
  reload/RPC cases skipped and remain tracked by WP241.
- Golden image comparison, exact RGBA8 hashes, renderer execution traces, and
  golden inventory: passed.
- Short player process integration: `animgraph_demo_preset_headless_player`
  and the live WSI fault player both passed.
- `git diff --check`: passed.

`render_mechanism_coverage.md` gains the project material asset mechanism but
no technique rating changes: WP240c exposes an existing material/surface and
binding language to project runtime rather than adding a new shading hook.
The corresponding E3 +E5 evidence is recorded in
`render_evidence_ledger.md`.
