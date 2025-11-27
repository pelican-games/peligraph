set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>)
add_executable(pelican_studio main.cpp)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

target_link_libraries(pelican_studio PRIVATE pelican_core)

add_subdirectory(resources)

FetchContent_Declare(
  argparse
  GIT_REPOSITORY https://github.com/p-ranav/argparse.git
  GIT_TAG        v3.2
)
FetchContent_MakeAvailable(argparse)

FetchContent_Populate(
  mustache
  GIT_REPOSITORY https://github.com/kainjow/Mustache.git
  GIT_TAG        v4.1
)

target_link_libraries(pelican_studio PRIVATE argparse)
target_include_directories(pelican_studio PRIVATE ${mustache_SOURCE_DIR})
