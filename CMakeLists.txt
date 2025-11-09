cmake_minimum_required(VERSION 3.13)

project(pelican2)

if(WIN32)
set(PLATFORM_DESKTOP TRUE)
endif()

if(MSVC)
add_compile_options("/utf-8")
endif()

include(FetchContent)

# Logging
FetchContent_Declare(
  quill
  GIT_REPOSITORY https://github.com/odygrd/quill.git
  GIT_TAG        v10.2.0
)
FetchContent_MakeAvailable(quill)

# embed
set(B_PRODUCTION_MODE ON)
FetchContent_Declare(
  battery-embed
  GIT_REPOSITORY https://github.com/batterycenter/embed.git
  GIT_TAG        v1.2.19
)
FetchContent_MakeAvailable(battery-embed)

find_package(Vulkan)

add_subdirectory(src/core)
add_subdirectory(src/player)

if(NOT SKIP_DEVSTUDIO)
add_subdirectory(src/devstudio)
endif()
