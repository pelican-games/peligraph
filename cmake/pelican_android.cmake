# SA1: the Android (NDK clang, arm64-v8a) build of pelican_player.
#
# The root CMakeLists stays the single place that declares dependencies; on
# Android it calls into this file at three points:
#   pelican_android_feature_defaults()  before the feature options are declared
#   pelican_android_vulkan()            instead of find_package(Vulkan)
#   pelican_android_shaderc()           instead of the Vulkan SDK's shaderc
# A desktop configure never includes this file.
#
# Sources are pinned here and fetched by tag/commit; tools/android/
# sa1_configure.sh points FETCHCONTENT_SOURCE_DIR_<NAME> at local checkouts so
# the configure itself never reaches the network.
#
# SA0's PELICAN_ANDROID_SA0 path (GPU-free logic and tests) is separate and
# unchanged; it returns from the root before this file is read.

include_guard(GLOBAL)

if(NOT ANDROID)
  message(FATAL_ERROR "cmake/pelican_android.cmake is for the Android NDK toolchain only")
endif()

# Vulkan 1.3 core entry points (vkCmdBeginRendering, vkQueueSubmit2) are
# linked statically from libvulkan.so, which exports them from API 33 on.
if(ANDROID_PLATFORM_LEVEL AND ANDROID_PLATFORM_LEVEL LESS 33)
  message(FATAL_ERROR
    "The Android player needs ANDROID_PLATFORM android-33 or newer "
    "(found ${ANDROID_PLATFORM_LEVEL}); see sa1_spec.md 0.1")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# NDK r27c's libc++ (LLVM 18) keeps std::jthread / std::stop_token behind the
# experimental-library switch; MSVC's STL ships them as stable C++20.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)

# Features that the phone build leaves out (sa1_spec.md 0.10). These are cache
# defaults only: an explicit -D on the configure line still wins.
macro(pelican_android_feature_defaults)
  foreach(pelican_android_off
      PELICAN_WITH_IMGUI
      PELICAN_WITH_OPENXR
      PELICAN_WITH_RPC
      PELICAN_WITH_HOT_RELOAD
      PELICAN_WITH_RENDERDOC
      PELICAN_WITH_AUDIO
      PELICAN_WITH_SPIRV_LINK
      PELICAN_WITH_INTRO_LINK)
    if(NOT DEFINED CACHE{${pelican_android_off}})
      set(${pelican_android_off} OFF CACHE BOOL
          "${PELICAN_FEATURE_${pelican_android_off}_DESCRIPTION}")
    endif()
  endforeach()
  if(NOT DEFINED CACHE{SKIP_DEVSTUDIO})
    set(SKIP_DEVSTUDIO ON CACHE BOOL "Skip the Qt-based Pelican Studio targets")
  endif()
endmacro()

# Vulkan-Headers pinned to the SDK the desktop build is verified with
# (vma-hpp v3.3.0+3 is known to work with it). The loader is the device's
# libvulkan.so. Sets the same Vulkan_* variables FindVulkan sets on the desktop
# and the Vulkan::Headers target vma-hpp looks for.
macro(pelican_android_vulkan)
  FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        952f776f6573aafbb62ea717d871cd1d6816c387 # vulkan-sdk-1.4.309.0
  )
  FetchContent_GetProperties(vulkan_headers)
  if(NOT vulkan_headers_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(vulkan_headers)
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
  endif()

  set(Vulkan_FOUND TRUE)
  set(Vulkan_INCLUDE_DIR "${vulkan_headers_SOURCE_DIR}/include")
  set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
  set(Vulkan_LIBRARY vulkan)
  set(Vulkan_LIBRARIES vulkan)
  file(STRINGS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan_core.h" pelican_android_vk_patch
       REGEX "^#define VK_HEADER_VERSION [0-9]+")
  string(REGEX REPLACE ".*VK_HEADER_VERSION ([0-9]+).*" "\\1"
         pelican_android_vk_patch "${pelican_android_vk_patch}")
  set(Vulkan_VERSION "1.4.${pelican_android_vk_patch}")

  if(NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}")
  endif()
  if(NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan INTERFACE IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES vulkan)
  endif()

  # The NDK sysroot carries its own, older Vulkan C headers (1.3.275).  Put
  # the pinned headers ahead of it for every target so that no translation
  # unit mixes the two.
  include_directories(BEFORE "${Vulkan_INCLUDE_DIR}")
endmacro()

# The player as a NativeActivity library: android_native_app_glue from the
# NDK, entered at android_main (src/platform/android/android_main.cpp).
function(pelican_configure_android_player target_name)
  # Give the existing player entry an ordinary function name on Android.
  target_compile_definitions(${target_name} PRIVATE main=pelican_player_main)
  if(NOT TARGET pelican_android_native_app_glue)
    set(glue_dir "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(pelican_android_native_app_glue STATIC
      "${glue_dir}/android_native_app_glue.c")
    target_include_directories(pelican_android_native_app_glue PUBLIC "${glue_dir}")
    target_link_libraries(pelican_android_native_app_glue PUBLIC android log)
  endif()
  # Plain signature: the player's own target_link_libraries calls use it.
  target_link_libraries(${target_name} pelican_android_native_app_glue)
  # The glue's ANativeActivity_onCreate is referenced only by the framework,
  # so keep the linker from dropping it out of the static library.
  target_link_options(${target_name} PRIVATE "-Wl,-u,ANativeActivity_onCreate")
endfunction()

# shaderc, glslang and SPIRV-Tools of the Vulkan SDK 1.4.309 generation, built
# with shaderc's own CMake (sa1_spec.md 0.2). The SDK's glslc reports exactly
# this set: shaderc v2025.1, spirv-tools v2025.1 (f289d047), glslang 8b822ee8,
# which the glslang tag vulkan-sdk-1.4.309.0 contains with build-only changes
# on top. SPIRV-Headers is the revision the root pins (vulkan-sdk-1.4.309.0).
set(PELICAN_ANDROID_SHADERC_TAG "v2025.1")
macro(pelican_android_shaderc)
  FetchContent_Declare(
    shaderc
    GIT_REPOSITORY https://github.com/google/shaderc.git
    GIT_TAG        0968768c61d4eb7dd861114412e904bb3d59b7b6 # v2025.1
  )
  FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        7200bc12a8979d13b22cd52de80ffb7d41939615 # vulkan-sdk-1.4.309.0
  )
  FetchContent_Declare(
    spirv_tools
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
    GIT_TAG        f289d047f49fb60488301ec62bafab85573668cc # v2025.1
  )
  FetchContent_Declare(
    spirv_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG        09913f088a1197aba4aefd300a876b2ebbaa3391 # vulkan-sdk-1.4.309.0
  )
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  foreach(pelican_android_dep shaderc glslang spirv_tools spirv_headers)
    FetchContent_GetProperties(${pelican_android_dep})
    if(NOT ${pelican_android_dep}_POPULATED)
      FetchContent_Populate(${pelican_android_dep})
    endif()
  endforeach()
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()

  set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
  set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
  set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
  # Keep shaderc's install rules: with SHADERC_SKIP_INSTALL=ON shaderc hands
  # glslang GLSLANG_ENABLE_INSTALL="$<NOT:ON>" (a non-empty, hence true,
  # string) while skipping SPIRV-Tools' install, and glslang's export set then
  # fails to generate. Nothing here is ever installed.
  set(SHADERC_SKIP_INSTALL OFF CACHE BOOL "" FORCE)
  set(SHADERC_ENABLE_WERROR_COMPILE OFF CACHE BOOL "" FORCE)
  set(SHADERC_SPIRV_TOOLS_DIR "${spirv_tools_SOURCE_DIR}" CACHE STRING "" FORCE)
  set(SHADERC_SPIRV_HEADERS_DIR "${spirv_headers_SOURCE_DIR}" CACHE STRING "" FORCE)
  set(SHADERC_GLSLANG_DIR "${glslang_SOURCE_DIR}" CACHE STRING "" FORCE)
  set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
  set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
  set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_OPT ON CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  # Only the static libshaderc (and what it links) is built; glslc, the
  # shared library and SPIRV-Tools' extra libraries stay out of "all".
  add_subdirectory("${shaderc_SOURCE_DIR}" "${shaderc_BINARY_DIR}" EXCLUDE_FROM_ALL)

  add_library(pelican_shaderc INTERFACE)
  target_link_libraries(pelican_shaderc INTERFACE shaderc)
  function(pelican_copy_shaderc_runtime target_name)
  endfunction()
endmacro()
