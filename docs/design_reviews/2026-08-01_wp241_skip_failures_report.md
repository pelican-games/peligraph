# WP241: GPU failures hidden as skips

## Outcome

GPU tests no longer turn arbitrary engine exceptions into environment skips.
The four reported tests execute on a Vulkan device, and the GPU gate completes
with an empty allowlist and zero observed skips.

Implementation range 1 landed first as `b623fe3`. Before either fixture was
fixed, all four tests were run and reproduced the two documented exceptions as
real failures. The shared test helper now skips only the exact physical-device
enumeration failure; every other initialization, shader, pipeline, reload, and
assertion failure propagates.

The repository-wide audit also removed the same broad handler from the other
GPU fixtures. Large tests that retain a catch solely for temporary-directory
cleanup call the exact device-unavailable predicate and then rethrow.

## Exposed failures

The RPC glTF transaction fixture authored a fullscreen-only pipeline while a
live deferred standard material already existed. The runtime compatibility
check was correct to reject it. Commit `d25aa7a` moves that fixture to the
standard `hybrid_v1` preset and its `main_render` program, preserving the
material-pass contract exercised by the RPC operation.

The three material-values reload fixtures lowered `wp76.surface`, including
its custom textures, but paired the result with the standard fragment shader.
That shader has no binding 7 for `albedo_detail`, so reflection validation was
correct to reject it. Commit `b5bd5c4` compiles one shader bundle from the
declared surface and shares it across the base and named-variant records.
WP206b reload atomicity therefore runs rather than silently skipping.

No engine fail-fast check was weakened, and no GPU skip was allowlisted.

## Acceptance evidence

- OpenXR-enabled Debug build: passed.
- `ctest --test-dir build -C Debug -LE gpu --output-on-failure --parallel 4`:
  944/944 passed; the existing CPU directory-symlink capability case skipped.
- `python -B test/ci/run_gpu_gate.py --build-dir build --config Debug
  --artifacts-dir build/test_artifacts/wp241_gpu_gate`: 125/125 passed,
  `SKIP exact policy: PASS`, observed allowlisted skips 0.
- The four WP241 tests passed as ordinary executed tests; WP206b completed all
  reload assertions.
- Three-frame headless `animgraph_demo` player smoke: exit 0.
- Unsafe whole-test catch-to-SKIP search and `git diff --check`: clean.
