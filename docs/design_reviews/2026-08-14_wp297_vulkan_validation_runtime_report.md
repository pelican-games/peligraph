# WP297: Vulkan validation runtime selection report

## Outcome

- Public CLI `--vulkan-validation=off|on` selects the Khronos validation layer
  and synchronization validation together.
- Omitting the option preserves the previous default exactly: `_DEBUG` is on,
  and every other build is off.
- Explicit `on` is a hard requirement. If the Vulkan loader does not enumerate
  `VK_LAYER_KHRONOS_validation`, startup fails with
  `pelican.vulkan.validation_layer_unavailable@1`; it never silently falls
  back to an unvalidated instance.
- `get_status.vulkan_validation` reports the state finalized after successful
  instance creation. The report is derived from the layer list and validation
  feature chain used by the accepted `VkInstanceCreateInfo`, not merely from
  the launch option.
- Studio needs no private control. Its existing public player-argument path can
  pass the option. The choice of player binary remains outside this WP.

Synchronization validation deliberately stays coupled to the layer. This
preserves the existing `_DEBUG` behavior and avoids an apparently validated
mode that omits the most valuable and most expensive checks.

## Running-state contrast

The single `vulkan_validation_runtime_player` integration test routes every
launch through the production `studioPlayerArguments()` assembly and then
starts the real player. It asks the running process for `get_status` and checks
both `enabled` and `synchronization`.

| Build and invocation | Reported `enabled` | Reported `synchronization` | Reported `reason` |
|---|---:|---:|---|
| Debug, option absent | true | true | `enabled_by_debug_build_default` |
| Debug, `--vulkan-validation=off` | false | false | `disabled_by_launch_option` |
| RelWithDebInfo, option absent | false | false | `disabled_by_non_debug_build_default` |
| RelWithDebInfo, `--vulkan-validation=on` | true | true | `enabled_by_launch_option` |

Thus the non-`_DEBUG` requested and non-requested launches are observably
different in the state owned by the running Vulkan core.

## Missing-layer hard error

The integration test uses the Vulkan loader's layer filter in the child
process only:

```text
VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation
```

Under that same filter it first starts with
`--vulkan-validation=off`; this control must succeed and report
`available=false`, `enabled=false`, and `synchronization=false`. It then starts
with `--vulkan-validation=on`; this must exit nonzero, and the isolated process
log must contain all three of:

```text
pelican.vulkan.validation_layer_unavailable@1
--vulkan-validation=on
VK_LAYER_KHRONOS_validation
```

The check can be repeated with:

```powershell
ctest --test-dir build -C Debug -R "^vulkan_validation_runtime_player$" --output-on-failure -V
```

This verifies both that the filter actually hid the layer (the explicit-off
control runs and reports it unavailable) and that an explicit request fails
hard rather than falling back.

## Measured cost

Measurement environment: RelWithDebInfo player, `projects/example`, headless
RPC, 1280x720, NVIDIA GeForce RTX 5080, driver 591.86, Vulkan loader 1.4.350.
The no-validation side omitted the option, so it exercised the non-`_DEBUG`
default; the validation side used `--vulkan-validation=on`. Before timing each
process, `get_status` was checked for the expected running validation and
synchronization state.

Six runs per side were interleaved with alternating order. Each run warmed up
30 frames and timed 120 sequential `step_frame` calls. Startup is
`get_status.startup.total_ms`; frame cost is wall-clock time per synchronous
`step_frame` round trip. The latter includes the identical local RPC overhead
on both sides, so the delta is the product-level cost seen by a controlling
tool rather than a GPU-only timestamp.

| Metric | Validation absent (mean / median / sd) | Validation requested (mean / median / sd) | Requested overhead |
|---|---:|---:|---:|
| Startup | 866.842 / 868.251 / 5.096 ms | 913.145 / 913.788 / 7.020 ms | +46.303 ms (+5.34%) |
| One frame | 1.2921 / 1.2836 / 0.0248 ms | 3.5702 / 3.5679 / 0.1125 ms | +2.2781 ms (+176.31%, 2.763x) |

No `Validation Error` or `VUID-` marker occurred in any measured validation-on
run. Processes were shut down through their own stdin/PID; no process-name kill
was used.

## Verification

- `cmake --build build --config Debug --parallel`: passed.
- Debug runtime contrast: default on, explicit off, missing-layer explicit on
  rejected.
- RelWithDebInfo runtime contrast: default off, explicit on, missing-layer
  explicit on rejected.
- `ctest --test-dir build -C Debug -j4 --output-on-failure`: 1182 / 1182
  passed, 0 failed, 1 environment-dependent test skipped. The full golden test
  set passed.
- `uv run tools/doclink.py check`: 2315 links ok, 0 stale, 0 unresolved.
- `uv run tools/doclink.py audit`: 0 misaimed links, 0 absent identifiers.
- `git diff --check`: passed. No tracked golden asset changed.
