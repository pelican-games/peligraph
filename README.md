# Pelican2

game engine

Documentation (manual and source-code guide): https://pelican-games.github.io/peligraph/

## License

The engine is licensed under the [Mozilla Public License 2.0](LICENSE) with
the [Pelican Game Exception](LICENSE-EXCEPTION):

- Games made with Pelican may be sold, and their code, assets and changes to
  the engine kept closed, as long as they are shipped as games.
- Features, packages and other add-ons written in your own files may be
  sold under any license you choose.
- Distributing the engine itself, or an engine, editor or tool built from
  it, requires its source, including your changes, to be available under
  the MPL-2.0.

Third-party components keep their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Documentation / 日本語資料

- [利用マニュアル](docs/manual/00_index.md)
- [ソースコード読解ガイド](docs/source-code-guide/README.md)
- [用語集](docs/manual/14_glossary.md)
- [HTML で読む・GitHub Pages で公開する](docs/documentation_site.md)

Local documentation preview: `uv run tools/docs_site.py serve`

## Requisites

- Windows with MSVC and C++20. CI and the examples below use the Qt kit
  `msvc2022_64` (built for Visual Studio 2022). CMake does not check the
  compiler version, and the minimum working MSVC version has not been verified.
- CMake 3.20 or newer.
- Vulkan SDK 1.4.309 or newer. Configure stops with
  `Vulkan SDK 1.4.309 or newer is required` on an older SDK. Verified with 1.4.350;
  1.3.x is no longer supported since vma-hpp v3.3.0.
- Qt 6.10 or newer (Studio only).
- A GPU with Vulkan 1.3 and the features `multiDrawIndirect`,
  `drawIndirectFirstInstance`, `shaderDrawParameters` and `dynamicRendering`
  (plus `VK_KHR_swapchain` for a window). A discrete GPU is preferred;
  `pelican_player --gpu <index|name>` picks another one, but never one that lacks
  these features. Ray tracing features are optional.
- Python is not needed to build or run the engine. The OpenXR SDK is pinned to
  `KhronosGroup/OpenXR-SDK` release-1.1.61, which ships its generated sources.
  Python is needed only for `PELICAN_WITH_SPIRV_LINK=ON` (default OFF) and for
  Python-backed tests (`PELICAN_PYTHON_TESTS`, default OFF).
- Git: the source tree itself does not have to be a Git checkout (a ZIP download
  configures; crash-symbol archives then record the commit as `unknown`).
  The third-party dependencies are still fetched by CMake `FetchContent` from Git
  repositories, so configure needs the `git` command and network access.
  Bundling the dependencies in the source tree is planned after the first public
  release.
- Paths with Japanese (non-ASCII) characters: every Pelican executable carries a
  UTF-8 `activeCodePage` manifest (Windows 10 version 1903 or later). On an older
  Windows a warning says the process code page is not UTF-8.

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

### Shipping a game

`pelican_cli dist-config` derives the build options from the project and writes
a CMake cache preset. By default the preset turns Studio off
(`SKIP_DEVSTUDIO=ON`), RPC off, and builds the player without a console window
(`PELICAN_PLAYER_GUI_SUBSYSTEM=ON`); `--with-studio` keeps Studio and RPC.

```sh
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake -S . -B build-dist -C build-dist/preset.cmake -DPELICAN_PROJECT=projects/mygame
cmake --build build-dist --config Release --target pelican_runtime
cmake --install build-dist --config Release --component Runtime --prefix "$PWD/build-dist/package"
```

`PELICAN_PROJECT` builds the project's `code/` into `pelican_game_logic.dll`,
which is installed beside `pelican_player.exe`. The player looks for the DLL
(and writes `pipeline_cache.bin`) beside its own executable, not in the working
directory; engine resources are embedded in the binaries.
Copy the project (its `project.json` and the files it names) into the package,
then start the player with `--project <dir>`, or put `project.json` beside the
executable (the player then opens it and logs `implicit project root = ...`).
When the player is started by hand without a console and fails to start, it
shows the error in a message box as well as on stderr.

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
A configuration without Python keeps the defaults (`PELICAN_WITH_SPIRV_LINK=OFF`,
`PELICAN_PYTHON_TESTS=OFF`); OpenXR stays enabled because the pinned OpenXR SDK
ships generated sources. Python is not an engine runtime dependency.

The experimental SPIR-V linker and its `pelican-spv-link` CLI are a separate
build unit and default to OFF. Enable them with
`-DPELICAN_WITH_SPIRV_LINK=ON`; selecting the runtime backend still requires
`PELICAN_SPV_LINK=experimental`. The pinned SPIRV-Tools build uses Python for
code generation when this unit is enabled.
