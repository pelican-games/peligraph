# Pelican2

game engine

## Requisites

- Vulkan SDK 1.4.x (verified with 1.4.350; 1.3.x is no longer supported since vma-hpp v3.3.0)
- Qt 6.10 or newer (Studio only)

Development builds use the `shaderc` libraries shipped with the Vulkan SDK.
Pelican does not download and build shaderc as an implicit fallback. A target
that consumes host-precompiled SPIR-V instead can be configured with
`-DPELICAN_RUNTIME_SHADER_COMPILER=OFF`.

## How to build

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64
cmake --build build --config Debug
```

only player:

```sh
cmake -S . -B build-player -DSKIP_DEVSTUDIO=ON
cmake --build build-player --config Debug --target pelican_player
```

skip tests and all test-only tooling:

```sh
cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64
cmake --build build --config Debug --target pelican_runtime
```

Applications and the project game DLL share `build/dist/<Config>/`.
Each build tree has its own output directory; `PELICAN_OUTPUT_ROOT` can
override its root. Embedded engine/Studio resources travel in the binaries.

## Release bundle (Windows)

```sh
cmake -S . -B build-release -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64
cmake --build build-release --config Release --target pelican_runtime
cmake --install build-release --config Release --component Runtime --prefix "$PWD/build-release/package"
```

Qt's deployment step requires an absolute install prefix, as above.
`build-release/package/bin` contains the applications, the Release MSVC
redistributable runtime, and Qt's deployed runtime/plugins when Studio is
enabled. Debug CRTs are not distributed. Projects and their assets are supplied separately. Keep
the runtime shader compiler enabled for source-driven projects; disabling
it requires complete precompiled shader variants for that project.

## Debug

run Pelican Studio:
```sh
cmake --build build --config Debug --target run_studio
```

## Test

```sh
ctest --test-dir build -C Debug -j4 --output-on-failure
```

GPU tests share a lease and the `pelican_gpu` resource lock. Use `-LE gpu`
for GPU-free tests; CI gate commands and exact skip policies are in
[docs/ci.md](docs/ci.md).

`PELICAN_PYTHON_TESTS=OFF` (the default) omits Python-backed policy, RPC, and Studio E2E tests.
Use `AUTO` to add them when Python 3.11 or newer is available; complete CI and release gates use `ON`.
The OpenXR SDK source build can still require Python to generate dispatch
headers, independently of this test option. A configuration without Python
uses `-DPELICAN_WITH_OPENXR=OFF -DPELICAN_WITH_SPIRV_LINK=OFF` and keeps
Python-backed tests disabled. Python is not an engine runtime dependency.

The experimental SPIR-V linker and its `pelican-spv-link` CLI are a separate
build unit and default to OFF. Enable them with
`-DPELICAN_WITH_SPIRV_LINK=ON`; selecting the runtime backend still requires
`PELICAN_SPV_LINK=experimental`. The pinned SPIRV-Tools build uses Python for
code generation when this unit is enabled.
