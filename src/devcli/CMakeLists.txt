add_executable(pelican_cli main.cpp)
pelican_runtime_target(pelican_cli)
set_target_properties(pelican_cli PROPERTIES CXX_STANDARD 20)

target_sources(pelican_cli PRIVATE
  assetscommand.cpp
  bakecameracommand.cpp
  distconfig.cpp
  gltfsceneextract.cpp
  graphcatalogcommand.cpp
  imagecommand.cpp
  importcommand.cpp
  materialcommand.cpp
  packagecommand.cpp
  processrunner.cpp
  projectinit.cpp
  projectmigratecommand.cpp
  rulesimport.cpp
  vrmcommand.cpp
  ../core/model/vatformat.cpp
  ../core/model/vrmsemantic.cpp
  ../core/loader/ktx2.cpp
)

target_link_libraries(pelican_cli PRIVATE argparse nlohmann_json picosha2 glm pelican_project tinygltf stb)
