# Third-party license texts kept in this repository

These are verbatim copies of license texts that are not on a build machine
(they are not in a dependency CMake fetches, nor in the Vulkan SDK or the Qt
installation). `tools/third_party_notices.py` copies them into
`THIRD_PARTY_NOTICES.md`. Do not edit them; replace a file with a newer
upstream copy instead and update this table.

Fetched on 2026-10-09.

| File | Source | SHA-256 |
|---|---|---|
| `shaderc-LICENSE` | https://raw.githubusercontent.com/google/shaderc/main/LICENSE | `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4` |
| `glslang-LICENSE.txt` | https://raw.githubusercontent.com/KhronosGroup/glslang/main/LICENSE.txt | `17e70c676e1521ff3e4686f04a2053d93a7e28a33be8de7ec37ab0ff72feb677` |
| `Vulkan-Loader-LICENSE.txt` | https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/main/LICENSE.txt | `43c0a37e6a0fa7ff3c843b3ec5a4fac84b712558ddac103fbd4c1649662a9ece` |
| `pcg-c-basic-LICENSE.txt` | https://raw.githubusercontent.com/imneme/pcg-c-basic/master/LICENSE.txt | `b40930bbcf80744c86c46a12bc9da056641d722716c378f5659b9e555ef833e1` |
| `lgpl-3.0.txt` | https://www.gnu.org/licenses/lgpl-3.0.txt | `e3a994d82e644b03a792a930f574002658412f62407f5fee083f2555c5f23118` |
| `icu-LICENSE` | https://raw.githubusercontent.com/unicode-org/icu/main/LICENSE | `e09827667e0bb3c808b2a463548ba814a5e6249b67353234d70c8dfc2bcc417d` |
| `DirectXShaderCompiler-LICENSE.TXT` | https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/main/LICENSE.TXT | `27a49e35d1da96eba18fba54bc882667ff0ff8c0254f16f2b6e165d605ba7df8` |
| `mesa-license.rst` | https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/license.rst | `0d1a0472ecc81830e75c20d59b0ea02841e3db21255e0ebad97ab682c54d6615` |
| `mesa-MIT` | https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/licenses/MIT | `323c587d0ccf10e376f8bf9a7f31fb4ca6078105194b42e0b1e0ee2bc9bde71f` |

The shaderc, glslang and Vulkan-Loader texts are those of the upstream
default branch on the date above, not necessarily of the versions inside the
Vulkan SDK a build uses.
