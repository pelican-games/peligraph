add_executable(pelican_cli main.cpp)
pelican_runtime_target(pelican_cli)
set_target_properties(pelican_cli PROPERTIES CXX_STANDARD 20)

target_sources(pelican_cli PRIVATE
  assetscommand.cpp
  audiocommand.cpp
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

# AB-1: `audio analyze` decodes with the engine's decoders and bakes with the
# engine's bake, compiled in (the CLI does not link the engine), with
# miniaudio's implementation in a unit of its own.
if(PELICAN_WITH_AUDIO)
  target_sources(pelican_cli PRIVATE
    miniaudioimpl.cpp
    ../core/audio/sounddecode.cpp
    ../core/audio/audioanalysis.cpp
  )
  target_link_libraries(pelican_cli PRIVATE miniaudio)
endif()
