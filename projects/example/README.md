# Pelican Example Project

This project is the default local example for `pelican_player --project`.

## Read it in this order

`shaders/toon.surface` and `materials/toon.material.json` are the B-layer
source-material example. The surface declares its interface and versioned
hooks; the material only references it and overrides authored values.

`materials/openpbr_coat.material.json` dogfoods the bundled OpenPBR wrapper-B
surface with the exact OpenPBR 1.1.1 tag/commit recorded as material defines.
The `openpbr_coat_sphere` golden applies those values to an analytic sphere so
the base/specular response and coat highlight stay visible and deterministic.

`passes/main_rendering_config.json` is a **`pelican.graph` root**: the five
keys `schema` / `version` / `definitions` / `nodes` / `outputs` belong to the
common graph, and the whole rendering config (features, render targets,
rendering passes) sits under the single member `rendering`. Both shapes are
supported for the life of v0 -- the other four shipped projects are still
legacy roots, where the same keys sit at the top level. See
[the manual, 6.16](../../docs/manual/06_rendering.md) for what each root owns.

The example ships with `nodes` empty, so the rendered pixels are identical to
the legacy form. What the root buys is that a graph can now be attached
without moving the file again.

## What to change, and what happens

### Driving `bloom.intensity` from a curve

The bundled bloom feature declares `intensity` as `binding: "buffer"`, which
means the value is backed by the frame graph buffer `bloom.intensity` and can
be rewritten every frame from the common graph (see the manual, 6.5). A legacy
root cannot carry a graph at all, which is what this project moved for.

Studio's Inspector edits *instances* of a graph definition, not loose nodes,
so the definition comes first. Add one to `definitions` in
`passes/main_rendering_config.json`:

```json
"definitions": {
  "bloom_drive": {
    "nodes": [
      {"id": "amount", "op": "graph.input", "default": 2.0},
      {"id": "sink", "op": "render.scalar",
       "parameters": {"target": "bloom.intensity"},
       "inputs": {"value": {"node": "amount", "port": "value"}}}
    ],
    "outputs": {}
  }
}
```

Then, in Studio:

1. Open the project, select a scene object, and use **Attach...** in the
   Inspector's *Attached graphs* section. Pick `bloom_drive` and give the
   instance a name.
2. The instance's `amount` port appears under *Instance inputs* with its
   stored value. Press **Control with a curve**: the Inspector writes a
   `value.input{scene_time}` clock, a `value.playback` and a `value.curve`
   seeded with the current value, and connects the curve to `amount`.
3. Edit the curve's keys and save. Everything lands in the root `nodes` array;
   `rendering` is untouched.

`render.scalar` resolves `target` against the declared frame graph buffers, so
a misspelled name is a hard error at compile time (`unbound frame graph
output: ...`) rather than a silently dead curve.

### Binary assets

Binary assets are intentionally not tracked in git. Their expected project-relative inventory is recorded in
`assets.manifest.json`. The acquisition procedure, redistribution rules, and per-file provenance/license ledger are
maintained in [`docs/example_assets.md`](../../docs/example_assets.md). After cloning, use the manifest-aware commands
to see where the asset store resolves and which files still need to be supplied:

```sh
pelican_cli assets status --project projects/example
pelican_cli assets verify --project projects/example
```

Use `assets verify --full` when content corruption is suspected. After an intentional asset update, regenerate the
sorted manifest and review its git diff before committing:

```sh
pelican_cli assets verify --full --project projects/example
pelican_cli assets manifest --project projects/example
```

Large binary assets should normally use Git LFS or an external asset store configured through
`.pelican/local.json`. The manifest records content and relative structure; the local override changes only the
resolved store location.
