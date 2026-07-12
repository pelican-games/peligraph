cmake_minimum_required(VERSION 3.13)

project(pelican2)

if(WIN32)
set(PLATFORM_DESKTOP TRUE)
endif()

if(MSVC)
add_compile_options("/utf-8")
add_compile_options("/MP")
add_compile_options("/FS")
endif()

include(FetchContent)

# variable definition
set(PELICAN_INSTALL_DIR ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>)
set(PELICAN_LIB_DIR ${PELICAN_INSTALL_DIR}/lib/bin)
set(PELICAN_INCLUDE_DIR ${PELICAN_INSTALL_DIR}/lib/include)
option(PELICAN_RUNTIME_SHADER_COMPILER "Enable runtime GLSL shader compilation with shaderc" ON)
option(PELICAN_WITH_AUDIO "Enable WAV sound-effect playback with miniaudio" ON)
option(PELICAN_WITH_VAT "Enable pelican.vat GLB playback support" ON)
option(PELICAN_WITH_EXR "Enable EXR texture loading with tinyexr" ON)
option(PELICAN_WITH_RPC "Enable stdio JSON-RPC control server" ON)
option(PELICAN_WITH_SEQPLAYER "Enable transform sequence playback" ON)
option(PELICAN_WITH_IMGUI "Enable the interactive engine developer UI" ON)
option(PELICAN_ENABLE_ASAN "Build targets with AddressSanitizer (intended for test builds)" OFF)
set(PELICAN_PROJECT "" CACHE PATH "Project directory whose code/ sources are linked into pelican_player")

if(PELICAN_ENABLE_ASAN)
  if(MSVC)
    add_compile_options("/fsanitize=address" "/Oy-")
    add_link_options("/INCREMENTAL:NO")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options("-fsanitize=address" "-fno-omit-frame-pointer")
    add_link_options("-fsanitize=address")
  else()
    message(FATAL_ERROR "PELICAN_ENABLE_ASAN is unsupported by this compiler")
  endif()
endif()

# external dependencies
## Logging
FetchContent_Declare(
  quill
  GIT_REPOSITORY https://github.com/odygrd/quill.git
  GIT_TAG        v10.2.0
)
FetchContent_MakeAvailable(quill)

## embed
set(B_PRODUCTION_MODE ON)
FetchContent_Declare(
  battery-embed
  GIT_REPOSITORY https://github.com/batterycenter/embed.git
  GIT_TAG        v1.2.19
)
FetchContent_MakeAvailable(battery-embed)

## vector operation
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.2
)
FetchContent_MakeAvailable(glm)

## miniaudio
if(PELICAN_WITH_AUDIO)
  FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.25
  )
  FetchContent_GetProperties(miniaudio)
  if(NOT miniaudio_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(miniaudio)
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
  endif()
  if(NOT TARGET miniaudio)
    add_library(miniaudio INTERFACE)
    target_include_directories(miniaudio INTERFACE ${miniaudio_SOURCE_DIR})
  endif()
endif()

## json
FetchContent_Declare(
  nlohmann
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
)
FetchContent_MakeAvailable(nlohmann)

## argparse
FetchContent_Declare(
  argparse
  GIT_REPOSITORY https://github.com/p-ranav/argparse.git
  GIT_TAG        v3.2
)
FetchContent_MakeAvailable(argparse)

## sha256
FetchContent_Declare(
  picosha2
  GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
  GIT_TAG        v1.0.1
)
FetchContent_GetProperties(picosha2)
if(NOT picosha2_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(picosha2)
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()
endif()
if(NOT TARGET picosha2)
  add_library(picosha2 INTERFACE)
  target_include_directories(picosha2 INTERFACE ${picosha2_SOURCE_DIR})
endif()

## shader tool dependencies
# WP80 owns a reproducible SPIR-V linker toolchain.  These revisions are
# deliberately independent of the installed Vulkan SDK; changing them is a
# reviewed cache/ABI event rather than an implicit SDK upgrade.
FetchContent_Declare(
  spirv_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
  GIT_TAG        09913f088a1197aba4aefd300a876b2ebbaa3391 # vulkan-sdk-1.4.309.0
)
FetchContent_GetProperties(spirv_headers)
if(NOT spirv_headers_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(spirv_headers)
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()
endif()

set(SPIRV-Headers_SOURCE_DIR ${spirv_headers_SOURCE_DIR} CACHE PATH "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  spirv_tools
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
  GIT_TAG        f289d047f49fb60488301ec62bafab85573668cc # v2025.1
)
FetchContent_MakeAvailable(spirv_tools)
# SPIRV-Tools always declares a shared library and auxiliary libraries as ALL
# targets.  Pelican owns only the pinned static parser/link/optimizer surface;
# keep unrelated distro/fuzzer tooling out of the normal engine build.
foreach(PELICAN_UNUSED_SPIRV_TARGET
    SPIRV-Tools-shared
    SPIRV-Tools-diff
    SPIRV-Tools-lint
    SPIRV-Tools-reduce
)
  if(TARGET ${PELICAN_UNUSED_SPIRV_TARGET})
    set_target_properties(${PELICAN_UNUSED_SPIRV_TARGET} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
endforeach()

FetchContent_Declare(
  spirv_reflect
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
  GIT_TAG        c637858562fbce1b6f5dc7ca48d4e8a5bd117b70 # vulkan-sdk-1.4.309.0
)
set(SPIRV_REFLECT_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(SPIRV_REFLECT_STATIC_LIB ON CACHE BOOL "" FORCE)
set(SPIRV_REFLECT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spirv_reflect)

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
)
FetchContent_MakeAvailable(stb)
if(NOT TARGET stb)
  add_library(stb INTERFACE)
  target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})
endif()

if(PELICAN_WITH_EXR)
  FetchContent_Declare(
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG        v3.1.0
  )
  FetchContent_GetProperties(tinyexr)
  if(NOT tinyexr_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(tinyexr)
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
  endif()
  if(NOT TARGET tinyexr)
    add_library(tinyexr INTERFACE)
    target_include_directories(tinyexr INTERFACE
      ${tinyexr_SOURCE_DIR}
      ${tinyexr_SOURCE_DIR}/deps/miniz
    )
  endif()
endif()

## vulkan
find_package(Vulkan)

if(PELICAN_RUNTIME_SHADER_COMPILER)
  set(PELICAN_VULKAN_SDK_LIB_DIR "")
  set(PELICAN_VULKAN_SDK_BIN_DIR "")
  if(DEFINED ENV{VULKAN_SDK})
    set(PELICAN_VULKAN_SDK_LIB_DIR "$ENV{VULKAN_SDK}/Lib")
    set(PELICAN_VULKAN_SDK_BIN_DIR "$ENV{VULKAN_SDK}/Bin")
  endif()

  find_library(PELICAN_SHADERC_COMBINED_RELEASE
    NAMES shaderc_combined
    HINTS ${PELICAN_VULKAN_SDK_LIB_DIR}
  )
  find_library(PELICAN_SHADERC_COMBINED_DEBUG
    NAMES shaderc_combinedd
    HINTS ${PELICAN_VULKAN_SDK_LIB_DIR}
  )
  find_library(PELICAN_SHADERC_SHARED_IMPLIB
    NAMES shaderc_shared
    HINTS ${PELICAN_VULKAN_SDK_LIB_DIR}
  )
  find_file(PELICAN_SHADERC_SHARED_DLL
    NAMES shaderc_shared.dll
    HINTS ${PELICAN_VULKAN_SDK_BIN_DIR}
  )

  set(PELICAN_USE_FETCHCONTENT_SHADERC OFF)
  if(NOT PELICAN_SHADERC_COMBINED_RELEASE)
    set(PELICAN_USE_FETCHCONTENT_SHADERC ON)
  endif()
  if(MSVC AND NOT PELICAN_SHADERC_COMBINED_DEBUG)
    if(PELICAN_SHADERC_SHARED_IMPLIB AND PELICAN_SHADERC_SHARED_DLL)
      message(STATUS "shaderc_combinedd.lib was not found; using shaderc_shared.dll for Debug builds")
    else()
      message(WARNING "shaderc_combinedd.lib was not found; falling back to FetchContent shaderc for Debug builds")
      set(PELICAN_USE_FETCHCONTENT_SHADERC ON)
    endif()
  endif()

  if(PELICAN_USE_FETCHCONTENT_SHADERC)
    set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
    set(SHADERC_ENABLE_WGSL_OUTPUT OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      shaderc
      GIT_REPOSITORY https://github.com/google/shaderc.git
      GIT_TAG        v2026.2
    )
    FetchContent_MakeAvailable(shaderc)
  endif()

  add_library(pelican_shaderc INTERFACE)
  if(PELICAN_SHADERC_COMBINED_RELEASE)
    add_library(pelican_shaderc_sdk_release STATIC IMPORTED)
    set_target_properties(pelican_shaderc_sdk_release PROPERTIES
      IMPORTED_LOCATION ${PELICAN_SHADERC_COMBINED_RELEASE}
      INTERFACE_INCLUDE_DIRECTORIES ${Vulkan_INCLUDE_DIRS}
    )
  endif()
  if(PELICAN_SHADERC_COMBINED_DEBUG)
    add_library(pelican_shaderc_sdk_debug STATIC IMPORTED)
    set_target_properties(pelican_shaderc_sdk_debug PROPERTIES
      IMPORTED_LOCATION ${PELICAN_SHADERC_COMBINED_DEBUG}
      INTERFACE_INCLUDE_DIRECTORIES ${Vulkan_INCLUDE_DIRS}
    )
  endif()
  if(PELICAN_SHADERC_SHARED_IMPLIB AND PELICAN_SHADERC_SHARED_DLL)
    add_library(pelican_shaderc_sdk_shared SHARED IMPORTED)
    set_target_properties(pelican_shaderc_sdk_shared PROPERTIES
      IMPORTED_IMPLIB ${PELICAN_SHADERC_SHARED_IMPLIB}
      IMPORTED_LOCATION ${PELICAN_SHADERC_SHARED_DLL}
      INTERFACE_INCLUDE_DIRECTORIES ${Vulkan_INCLUDE_DIRS}
    )
  endif()

  if(PELICAN_USE_FETCHCONTENT_SHADERC)
    target_link_libraries(pelican_shaderc INTERFACE
      $<$<CONFIG:Debug>:shaderc_combined>
    )
    if(PELICAN_SHADERC_COMBINED_RELEASE)
      target_link_libraries(pelican_shaderc INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:pelican_shaderc_sdk_release>
      )
    else()
      target_link_libraries(pelican_shaderc INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:shaderc_combined>
      )
    endif()
  elseif(PELICAN_SHADERC_COMBINED_DEBUG)
    target_link_libraries(pelican_shaderc INTERFACE
      $<$<CONFIG:Debug>:pelican_shaderc_sdk_debug>
      $<$<NOT:$<CONFIG:Debug>>:pelican_shaderc_sdk_release>
    )
  elseif(PELICAN_SHADERC_SHARED_IMPLIB AND PELICAN_SHADERC_SHARED_DLL)
    # Vulkan SDK static shaderc is Release CRT only on some Windows installs.
    # The shared import lib keeps Debug builds out of that CRT mismatch.
    target_link_libraries(pelican_shaderc INTERFACE
      $<$<CONFIG:Debug>:pelican_shaderc_sdk_shared>
      $<$<NOT:$<CONFIG:Debug>>:pelican_shaderc_sdk_release>
    )
  else()
    target_link_libraries(pelican_shaderc INTERFACE pelican_shaderc_sdk_release)
  endif()

  function(pelican_copy_shaderc_runtime target_name)
    if(TARGET pelican_shaderc_sdk_shared)
      add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:pelican_shaderc_sdk_shared>
          $<TARGET_FILE_DIR:${target_name}>
      )
    endif()
  endfunction()
endif()

# main
## pelican lib
add_subdirectory(src/project)
add_subdirectory(src/core)
add_subdirectory(src/spvlink)
add_subdirectory(src/player)
add_subdirectory(src/devcli)

## pelican studio
if(NOT SKIP_DEVSTUDIO)
add_subdirectory(src/devstudio)
endif()

## test
if(NOT SKIP_TEST)
enable_testing()
add_subdirectory(test)
endif()
