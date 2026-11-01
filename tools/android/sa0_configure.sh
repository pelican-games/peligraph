#!/usr/bin/env bash
# SA0: configure the Android (arm64-v8a) GPU-free build in build-android/.
# Run from Git Bash at the repository root. Dependencies come from an existing
# desktop build tree's _deps (DEPS), never from the network.
set -euo pipefail
NDK=${NDK:-<作業ディレクトリ>/android/android-ndk-r27c}
DEPS=${DEPS:-<作業ディレクトリ>/rc2/build/_deps}
CMAKE=${CMAKE:-"C:/Program Files/CMake/bin/cmake.exe"}
NINJA=${NINJA:-"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe"}
BUILD=${BUILD:-build-android}

"$CMAKE" -S . -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo} \
  -DPELICAN_ANDROID_SA0=ON \
  -DPELICAN_SA0_CORE_SLICES="${SLICES:-physquery;animation}" \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN="$DEPS/nlohmann-src" \
  -DFETCHCONTENT_SOURCE_DIR_GLM="$DEPS/glm-src" \
  -DFETCHCONTENT_SOURCE_DIR_ARGPARSE="$DEPS/argparse-src" \
  -DFETCHCONTENT_SOURCE_DIR_QUILL="$DEPS/quill-src" \
  -DFETCHCONTENT_SOURCE_DIR_PICOSHA2="$DEPS/picosha2-src" \
  -DFETCHCONTENT_SOURCE_DIR_STB="$DEPS/stb-src" \
  -DFETCHCONTENT_SOURCE_DIR_CATCH2="$DEPS/catch2-src" \
  "$@"
