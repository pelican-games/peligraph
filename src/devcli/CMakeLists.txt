set(PELICAN_INSTALL_DIR ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PELICAN_INSTALL_DIR})
add_executable(pelican_cli main.cpp)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

add_subdirectory(resources)

FetchContent_Populate(
  mustache
  GIT_REPOSITORY https://github.com/kainjow/Mustache.git
  GIT_TAG        v4.1
)

target_link_libraries(pelican_cli PRIVATE argparse)
target_include_directories(pelican_cli PRIVATE ${mustache_SOURCE_DIR})
