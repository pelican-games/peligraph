# Low-write build artifact report

Date: 2026-07-25

## Outcome

The source tests are not the storage problem: `test/` is about 3.2 MiB.
The large footprint came from MSVC generating a linker PDB and an
incremental-link database for every Debug test executable, plus many old build
trees configured with the full test suite.

Routine Windows test builds now keep Debug code generation and assertions but
omit per-test linker PDB and ILK files by default. The embedded-resource
generator was also made content-aware, so an unchanged configure/build no
longer rewrites support files or reevaluates every resource-generation rule.

No source tests were removed.

## Measured write amplification

At the time of the audit, build and distribution directories occupied about
155.72 GiB. The main `build/test` tree alone occupied about 83.23 GiB:

| Artifact in `build/test` | Size | Count |
| --- | ---: | ---: |
| Linker PDB | 47.27 GiB | 371 |
| Incremental-link ILK | 30.10 GiB | 178 |
| Test EXE | 4.00 GiB | 304 |
| Object files | 1.58 GiB | - |

There were 155 Debug test executables. Each one links much of
`pelican_core.lib`, so separate linker databases duplicated the same symbol
information many times.

For one representative target,
`pelican_test_deterministicrng_test`, the old linker outputs were about
923 MiB in total. With lean test artifacts enabled, the executable plus
remaining object/compile-symbol files occupy about 23 MiB. This is a measured
single-target reduction of about 97.5%; it is not a projection of the exact
size of a clean full-suite rebuild.

## Test artifact policy

`PELICAN_LEAN_TEST_ARTIFACTS` defaults to `ON`. On MSVC, Debug and
RelWithDebInfo test executables use non-incremental linking without a linker
PDB. Compile-time PDBs remain available, while the test code, assertions, and
CTest registration are unchanged.

Use a separate build tree when full debugger symbols are needed:

```powershell
cmake -S . -B build-debug-symbols `
  -DPELICAN_LEAN_TEST_ARTIFACTS=OFF
```

Feature-probe, packaging, demo, and other auxiliary build trees that do not
run tests should be configured without the test graph:

```powershell
cmake -S . -B build-feature-probe -DBUILD_TESTING=OFF
```

This avoids creating the 155 test projects in every configuration variant.

## SDK staging

The old `pelican_core` post-build step copied the Debug core archive and its
static dependencies into `dist_debug` after every core relink. The core
archive alone is about 1 GiB, duplicating data already present in the active
build tree.

Automatic SDK staging is now opt-in:

```powershell
cmake -S . -B build-sdk -DPELICAN_AUTO_STAGE_SDK=ON
```

Routine engine and test builds leave it disabled. This does not change the
configured output location of the CLI or Studio executables; it only gates
the copied SDK libraries and public headers.

## Embedded-resource generation

Pelican pins `battery-embed` v1.2.19. Its configure logic unconditionally
rewrote generator templates and a shared generated header. That changed
dependency timestamps and caused up to 87 resource custom commands to be
reevaluated after an otherwise unchanged configure.

The FetchContent patch now:

- writes generator support files only when their contents change;
- generates each target's aggregate header once and updates it only when its
  contents change;
- removes the aggregate header from a generated resource source rule whose
  output does not depend on that header.

The patch is deliberately strict: if the pinned upstream CMake structure
changes, configuration stops with an instruction to review the patch rather
than applying a silent partial edit.

After migration, two consecutive no-op builds ran zero resource-generation
rules and left the test executable, `pelican_core.lib`, and
`pelican_resources.lib` timestamps unchanged.

## Build-tree cleanup

On 2026-07-26, fourteen ignored legacy build/distribution directories were
removed after checking that every target was a workspace-root child, ignored
by Git, and contained no reparse points. The current `build-off-openxr` tree
was retained.

An additional 3.236 GiB of legacy per-test linker PDB/ILK files was removed
from the retained tree. Compile PDBs and the core library's debugger symbols
were preserved. In total, about 150.7 GiB was reclaimed. The workspace fell
from about 155.72 GiB of build/distribution data to roughly 5 GiB for the
active build tree.
