cmake_minimum_required(VERSION 3.13)

project(pelican2)

if(WIN32)
set(PLATFORM_DESKTOP TRUE)
endif()

if(MSVC)
add_compile_options("/utf-8")
add_compile_options("/MP")
endif()

include(FetchContent)

# variable definition
set(PELICAN_INSTALL_DIR ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>)
set(PELICAN_LIB_DIR ${PELICAN_INSTALL_DIR}/lib/bin)
set(PELICAN_INCLUDE_DIR ${PELICAN_INSTALL_DIR}/lib/include)
option(PELICAN_RUNTIME_SHADER_COMPILER "Enable runtime GLSL shader compilation with shaderc" ON)
option(PELICAN_WITH_VAT "Enable pelican.vat GLB playback support" ON)
option(PELICAN_WITH_EXR "Enable EXR texture loading with tinyexr" ON)
option(PELICAN_WITH_RPC "Enable stdio JSON-RPC control server" ON)
option(PELICAN_WITH_SEQPLAYER "Enable transform sequence playback" ON)

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
FetchContent_Declare(
  spirv_reflect
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Reflect.git
  GIT_TAG        vulkan-sdk-1.4.309.0
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
add_subdirectory(src/core)
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
