set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PELICAN_INSTALL_DIR})
add_executable(pelican_cli main.cpp)
set_target_properties(pelican_cli PROPERTIES CXX_STANDARD 20)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

target_sources(pelican_cli PRIVATE
  assetscommand.cpp
  distconfig.cpp
  gltfsceneextract.cpp
  importcommand.cpp
  materialcommand.cpp
  projectinit.cpp
  ../core/model/vatformat.cpp
)

target_link_libraries(pelican_cli PRIVATE argparse nlohmann_json picosha2 glm pelican_project tinygltf)
