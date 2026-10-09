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

The shaderc, glslang and Vulkan-Loader texts are those of the upstream
default branch on the date above, not necessarily of the versions inside the
Vulkan SDK a build uses.
