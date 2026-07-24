# Pelican2

game engine

## Requisites

- Vulkan SDK 1.4.x (verified with 1.4.350; 1.3.x is no longer supported since vma-hpp v3.3.0)
- Qt6

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
ctest --test-dir ./build
```

Python is not required by the default engine build or by production
`BUILD_TESTING=OFF` builds with the experimental linker left disabled.
With tests enabled, `PELICAN_PYTHON_TESTS=AUTO` (the default) registers the
Python-backed policy/RPC tests only when Python 3 is available. CI uses
`-DPELICAN_PYTHON_TESTS=ON`; use `OFF` to build and run only the C++/CMake
test suite.

The experimental SPIR-V linker and its `pelican-spv-link` CLI are a separate
build unit and default to OFF. Enable them with
`-DPELICAN_WITH_SPIRV_LINK=ON`; selecting the runtime backend still requires
`PELICAN_SPV_LINK=experimental`. The pinned SPIRV-Tools build uses Python for
code generation when this unit is enabled.
