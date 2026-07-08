# WP59 pelican-spv-link spike report

## Result

The SPIR-V ABI link path is viable for the B-layer surface hook.

Validated artifacts:

- `glsl_warm.final.spv`: GLSL surface, warm define, `spirv-val` OK.
- `glsl_cool.final.spv`: GLSL surface, no warm define, `spirv-val` OK.
- `slang_warm.final.spv`: Slang surface, warm define, `spirv-val` OK.
- All three final fragment modules create a `VkShaderModule` and a minimal
  graphics pipeline with a standalone Vulkan smoke test.

No `src/` files or root CMake files are changed. All generated files are under
`experiments/spvlink/out/` and are ignored by git.

## Reproduction

From the repository root:

```powershell
.\experiments\spvlink\run_spike.ps1
```

The script uses only the Vulkan SDK tools available in this environment plus
CMake/MSVC for the smoke-test executable:

- Vulkan SDK: `C:\VulkanSDK\1.4.309.0`
- glslang: 15.2.0
- SPIRV-Tools: v2025.1
- Slang: 2025.6.1

The script performs:

1. Compile `template.frag.glsl` with a stub `pelican_surface`.
2. Disassemble it, add `OpCapability Linkage`, decorate the stub as
   `LinkageAttributes "pelican_surface" Import`, and strip the stub body down
   to an imported function declaration.
3. Compile GLSL and Slang user surfaces with a dummy `main`.
4. Disassemble user modules, remove the dummy entry point/function, decorate
   `pelican_surface` as `Export`, remap user descriptors, and normalize ABI
   decorations.
5. Run `spirv-link --target-env vulkan1.2 --verify-ids`.
6. Run `spirv-opt --inline-entry-points-exhaustive --eliminate-dead-functions
   --eliminate-dead-code-aggressive --eliminate-dead-variables --compact-ids`.
7. Run `spirv-val --target-env vulkan1.2`.
8. Build and run `spvlink_verify`, which creates shader modules and graphics
   pipelines for the linked fragments.

Last observed pipeline output:

```text
pipeline ok: ...\out\glsl_warm.final.spv
pipeline ok: ...\out\glsl_cool.final.spv
pipeline ok: ...\out\slang_warm.final.spv
```

Slang emits one warning during SPIR-V disassembly/processing:

```text
(0): warning 30: the stage 'pixel' was specified more than once for entry point 'main'
```

It did not block validation or pipeline creation.

## What Was Normalized

The spike needed three concrete normalizations:

1. **Import/export injection.** glslang did not give a convenient GLSL-level
   way to emit the exact function `Import` contract. The working route is to
   compile normal GLSL, then patch SPIR-V assembly:
   `OpDecorate <function> LinkageAttributes "pelican_surface" Import/Export`.
2. **Debug-name cleanup.** After deleting the dummy user `main`, stale
   `OpName`/`OpMemberName` records can reference IDs that no longer exist.
   Leaving them in caused `spirv-opt` forward-reference failures. The prototype
   strips debug names after it has used them to locate functions/resources.
3. **Surface type layout cleanup.** Slang emitted
   `OpMemberDecorate %PelicanSurface Offset ...` for the function-local
   surface struct, while glslang did not. With those decorations present,
   `spirv-link` kept two structurally equivalent surface types and inserted an
   invalid pointer bitcast at the call site. Removing member layout decorations
   for this ABI-only function struct made the types merge cleanly. After this,
   `--allow-pointer-mismatch` was not required.

This matches the intended restriction that the B-layer ABI surface should stay
to scalars, vectors, and simple structs. Matrix layout and block layout should
remain outside the surface function ABI.

## Binding Remap

Remap is done before linking, while the user module is still isolated.

Current mapping generated in `out/bindings.json`:

- GLSL `sampler2D userTexture`: set 2 binding 0 -> set 2 binding 6.
- Slang `Texture2D userTexture`: set 2 binding 0 -> set 2 binding 6.
- Slang `SamplerState userSampler`: set 2 binding 1 -> set 2 binding 7.

Important difference: GLSL produces a combined image sampler descriptor, while
Slang naturally produced split sampled-image and sampler descriptors for this
source. The Vulkan smoke test creates the set 2 layout per final pipeline to
match that difference.

For production, the CPU bind table must include descriptor type as well as
set/binding. If Pelican wants a single material texture contract across
languages, the Slang shim should either enforce the split form explicitly or
provide a generated wrapper convention that maps one logical texture to the two
native Slang resources.

## Variant Relink

The GLSL surface is compiled twice:

- `PELICAN_VARIANT_WARM` -> `glsl_warm.final.spv`
- no warm define -> `glsl_cool.final.spv`

Both variants relink against the same imported template and validate. This is
enough to prove the variant-cache shape: compile user variant, remap, link,
optimize, validate, then cache the final linked SPIR-V.

The Slang path was validated for the warm variant. Adding the cool Slang variant
is mechanical; it was not necessary to prove a different compiler ABI issue.

## glslang and Slang Pitfalls

- glslang's `--no-link` can emit export linkage for functions, but the prototype
  still needed assembly patching to produce the exact template import shape.
- glslang requires `--keep-uncalled` for user-library style shaders; otherwise
  unreferenced hook functions can disappear.
- Slang inlined `pelican_surface` into `main` until the function was marked
  `[noinline]` and compiled with `-O0`.
- Slang preserved more struct layout decoration than glslang for the simple
  function-local struct. This must be normalized for cross-compiler linking.
- Slang generated split texture/sampler descriptors, unlike GLSL's combined
  sampler2D. This is manageable, but the material binder cannot assume one
  descriptor per logical texture for all source languages.

## Recommendation

Adopt the SPIR-V ABI-link direction for the B layer, with hardening before
product code:

1. Implement `pelican-spv-link` as a real tool using SPIRV-Tools and
   SPIRV-Reflect APIs, not text assembly rewriting.
2. Keep the surface ABI small: scalar/vector/simple struct only, no matrices,
   arrays, resource handles, or block-layout-dependent types.
3. Generate language-specific shims and test them as golden fixtures:
   GLSL combined sampler, Slang split texture/sampler, and later HLSL.
4. Treat descriptor remap output as a first-class CPU bind table:
   logical name, descriptor type, original set/binding, remapped set/binding.
5. Keep a GLSL include fallback until the production linker has permanent
   CI coverage for GLSL and Slang.

The spike does not show a fundamental blocker. The main cost is owning a small
SPIR-V normalization/link orchestration tool and keeping golden coverage around
compiler output changes.
