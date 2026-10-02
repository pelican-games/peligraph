add_executable(pelican_cli main.cpp)
pelican_runtime_target(pelican_cli)
set_target_properties(pelican_cli PROPERTIES CXX_STANDARD 20)

target_sources(pelican_cli PRIVATE
  assetscommand.cpp
  bakecameracommand.cpp
  canvaslowercommand.cpp
  distconfig.cpp
  gltfsceneextract.cpp
  graphcatalogcommand.cpp
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
)

target_link_libraries(pelican_cli PRIVATE argparse nlohmann_json picosha2 glm pelican_project tinygltf)

# IL-1 intro link: `pelican_cli intro-link bake|serve`. OFF compiles only the
# stub that says the build left it out; nothing else refers to these sources.
if(PELICAN_WITH_INTRO_LINK)
  target_sources(pelican_cli PRIVATE
    introlinkcommand.cpp
    introlinkproject.cpp
    introlinkserve.cpp
  )
  if(WIN32)
    target_link_libraries(pelican_cli PRIVATE ws2_32)
  endif()
else()
  target_sources(pelican_cli PRIVATE introlinkdisabled.cpp)
endif()
