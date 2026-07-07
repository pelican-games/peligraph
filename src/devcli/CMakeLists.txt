set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PELICAN_INSTALL_DIR})
add_executable(pelican_cli main.cpp)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

target_sources(pelican_cli PRIVATE
  distconfig.cpp
  importcommand.cpp
  ../core/loader/importmanifest.cpp
  ../core/model/vatformat.cpp
)

add_subdirectory(resources)

FetchContent_Populate(
  mustache
  GIT_REPOSITORY https://github.com/kainjow/Mustache.git
  GIT_TAG        v4.1
)

target_link_libraries(pelican_cli PRIVATE argparse nlohmann_json picosha2 glm)
target_include_directories(pelican_cli PRIVATE ${mustache_SOURCE_DIR})
