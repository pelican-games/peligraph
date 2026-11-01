#!/usr/bin/env bash
# SA1: configure the Android (arm64-v8a) player build in build-android/.
# Run from Git Bash at the repository root; then build the player with
#   "$NINJA" -C build-android pelican_player
#
# Dependency sources are local, never fetched by the configure:
#   DEPS         the desktop build tree's _deps (quill, glm, vma-hpp, ...)
#   ANDROID_DEPS checkouts of the sources only Android needs, each at the
#                tag/commit pinned in cmake/pelican_android.cmake:
#                  shaderc        https://github.com/google/shaderc            v2025.1
#                  glslang        https://github.com/KhronosGroup/glslang      vulkan-sdk-1.4.309.0
#                  SPIRV-Tools    https://github.com/KhronosGroup/SPIRV-Tools  v2025.1
#                  Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers vulkan-sdk-1.4.309.0
#                SPIRV-Headers (vulkan-sdk-1.4.309.0) comes from DEPS.
# VULKAN_SDK's host glslang compiles the engine's embedded shaders, as on the
# desktop. PYTHON is needed by SPIRV-Tools / glslang / shaderc's generators.
set -euo pipefail
NDK=${NDK:-<作業ディレクトリ>/android/android-ndk-r27c}
DEPS=${DEPS:-<作業ディレクトリ>/rc2/build/_deps}
ANDROID_DEPS=${ANDROID_DEPS:-<作業ディレクトリ>/android/deps}
CMAKE=${CMAKE:-"C:/Program Files/CMake/bin/cmake.exe"}
NINJA=${NINJA:-"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"}
PYTHON=${PYTHON:-<home>/AppData/Roaming/uv/python/cpython-3.12-windows-x86_64-none/python.exe}
BUILD=${BUILD:-build-android}
export VULKAN_SDK=${VULKAN_SDK:-C:/VulkanSDK/1.4.309.0}

"$CMAKE" -S . -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-33 \
  -DANDROID_STL=c++_static \
  -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
  -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo} \
  -DBUILD_TESTING=OFF \
  -DSKIP_DEVSTUDIO=ON \
  -DPython_EXECUTABLE="$PYTHON" \
  -DPython3_EXECUTABLE="$PYTHON" \
  -DPYTHON_EXECUTABLE="$PYTHON" \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_QUILL="$DEPS/quill-src" \
  -DFETCHCONTENT_SOURCE_DIR_BATTERY-EMBED="$DEPS/battery-embed-src" \
  -DFETCHCONTENT_SOURCE_DIR_GLM="$DEPS/glm-src" \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN="$DEPS/nlohmann-src" \
  -DFETCHCONTENT_SOURCE_DIR_ARGPARSE="$DEPS/argparse-src" \
  -DFETCHCONTENT_SOURCE_DIR_PICOSHA2="$DEPS/picosha2-src" \
  -DFETCHCONTENT_SOURCE_DIR_SPIRV_REFLECT="$DEPS/spirv_reflect-src" \
  -DFETCHCONTENT_SOURCE_DIR_SPIRV_HEADERS="$DEPS/spirv_headers-src" \
  -DFETCHCONTENT_SOURCE_DIR_STB="$DEPS/stb-src" \
  -DFETCHCONTENT_SOURCE_DIR_TINYEXR="$DEPS/tinyexr-src" \
  -DFETCHCONTENT_SOURCE_DIR_TINYGLTF="$DEPS/tinygltf-src" \
  -DFETCHCONTENT_SOURCE_DIR_VMA-HPP="$DEPS/vma-hpp-src" \
  -DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS="$ANDROID_DEPS/Vulkan-Headers" \
  -DFETCHCONTENT_SOURCE_DIR_SHADERC="$ANDROID_DEPS/shaderc" \
  -DFETCHCONTENT_SOURCE_DIR_GLSLANG="$ANDROID_DEPS/glslang" \
  -DFETCHCONTENT_SOURCE_DIR_SPIRV_TOOLS="$ANDROID_DEPS/SPIRV-Tools" \
  "$@"
