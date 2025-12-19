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

## vulkan
find_package(Vulkan)

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
