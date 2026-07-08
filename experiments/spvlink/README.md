# WP59 pelican-spv-link spike

This directory is intentionally self-contained. It does not participate in the
root build and does not modify `src/`.

Prerequisites:

- Vulkan SDK on `PATH`, including `glslangValidator`, `slangc`, `spirv-dis`,
  `spirv-as`, `spirv-link`, `spirv-opt`, and `spirv-val`.
- CMake and a C++20 compiler for the optional Vulkan pipeline smoke test.

Run the full spike:

```powershell
.\experiments\spvlink\run_spike.ps1
```

The script writes generated files under `experiments/spvlink/out/` and builds
the standalone verifier under `experiments/spvlink/build/`. Generated files are
ignored by git.

Core outputs:

- `out/glsl_warm.final.spv`
- `out/glsl_cool.final.spv`
- `out/slang_warm.final.spv`
- `out/bindings.json`
- `REPORT.md`
