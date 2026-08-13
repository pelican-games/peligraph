# Pelican2

game engine

## Requisites

- Vulkan SDK 1.4.x (verified with 1.4.350; 1.3.x is no longer supported since vma-hpp v3.3.0)
- Qt6

Development builds use the `shaderc` libraries shipped with the Vulkan SDK.
Pelican does not download and build shaderc as an implicit fallback. A target
that consumes host-precompiled SPIR-V instead can be configured with
`-DPELICAN_RUNTIME_SHADER_COMPILER=OFF`.

## How to build

```sh
cmake . -B build -DCMAKE_PREFIX_PATH=(Qt install path)   # ex. CMAKE_PREFIX_PATH=C:/Qt/6.10.2/msvc2022_64
cmake --build ./build
```

only player:

```sh
cmake . -B build -DSKIP_DEVSTUDIO
cmake --build ./build
```

skip tests and all test-only tooling:

```sh
cmake . -B build -DBUILD_TESTING=OFF
cmake --build ./build
```

## Debug

run Pelican Studio:
```sh
cmake --build ./build --target run_studio
```

## Test

```sh
ctest --test-dir ./build -j4
```

`-j` is not optional in practice. Serially the suite takes 674 s; at `-j4` it
takes 346 s, and it stops improving there because the `pelican_golden_gpu`
resource lock serialises 27 GPU tests into the critical path. Do not raise it
far above that for the full suite - `-j64` produced GPU segfaults. For the
CPU-only tier, `-LE gpu -j16` runs 1016 tests in about 9 s.

Python is not required by the default engine build or by production
`BUILD_TESTING=OFF` builds with the experimental linker left disabled.
With tests enabled, `PELICAN_PYTHON_TESTS=OFF` is the default and does not
search for Python. Use `AUTO` to add the Python-backed policy/RPC tests when
Python 3 is available. Complete CI uses `-DPELICAN_PYTHON_TESTS=ON`.

The experimental SPIR-V linker and its `pelican-spv-link` CLI are a separate
build unit and default to OFF. Enable them with
`-DPELICAN_WITH_SPIRV_LINK=ON`; selecting the runtime backend still requires
`PELICAN_SPV_LINK=experimental`. The pinned SPIRV-Tools build uses Python for
code generation when this unit is enabled.
