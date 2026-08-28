# Golden image color semantics

`expected.png` is the color contract 2 terminal result: encoded-sRGB RGB bytes and straight,
untransferred alpha. PNG files intentionally contain no color-management chunks; every consumer
must interpret RGB as sRGB. The renderer and RPC capture paths use the same RGBA8 contract.

Golden updates are allowed only through the six-step procedure in
`docs/design_color_pipeline.md` section 3. `stem_fullscreen` and `vat_playback` retain their
pre-existing non-zero tolerances; all other cases use exact post-baseline comparison.

Each case may set integer `width` and `height` fields in `case.json`; omitted dimensions default
to 16x16. The WP106 sprite cases use this to cover odd/even viewports and strict integer zooms
1, 2, and 3 without relaxing exact RGBA8 comparison.

## Inventory gate and updates

`inventory.json` is the committed exact-set contract for golden fixtures. For every case it
records the case name and mode, complete file list, `expected.png` SHA-256, whether
`tolerance.json` exists, the VAT build condition (`on_and_off` or `on_only`), and membership in
the RGBA8, renderer-execution, and canonical-frame-plan trace fixtures. The inventory check is
GPU-free and is part of `ctest -LE gpu` and the CPU CI gate.

Run the local inventory gate from the repository root:

```powershell
python -B test/golden_inventory.py --repo-root .
```

After an intentional case/file/trace membership change, build and pass the WP357a bloom oracle,
then regenerate the manifest and review its diff explicitly. Update mode discovers the standard
`build/test/Debug` executable; `--oracle-executable` or
`PELICAN_WP357_ORACLE_EXECUTABLE` selects another build explicitly:

```powershell
python -B test/golden_inventory.py --repo-root . --update
git diff -- test/golden/inventory.json
python -B test/golden_inventory.py --repo-root .
```

The oracle is executed inside the inventory updater before `inventory.json` is opened for writing.
Generating the inventory never changes `expected.png`. Image and trace rebaselining still follows
the six-step procedure above. `PELICAN_UPDATE_GOLDEN`,
`PELICAN_UPDATE_RGBA8_HASH_FIXTURES`, and `PELICAN_UPDATE_RENDERER_TRACE_FIXTURES` iterate only
the cases committed in `inventory.json`; an unregistered directory is not admitted by update
mode and makes the CPU inventory gate fail by name. The two aggregate hash/trace update modes
require a `PELICAN_WITH_VAT=ON` build so that an OFF build cannot silently drop the VAT-only
membership; ordinary comparison supports both VAT configurations.
