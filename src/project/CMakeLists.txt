add_library(pelican_project
    assetsmanifest.cpp
    featurecompose.cpp
    featurejitter.cpp
    importmanifest.cpp
    importrules.cpp
    jsonrpc.cpp
    logicalrendergraph.cpp
    logicalrendertype.cpp
    materialscreeninput.cpp
    materiallowering.cpp
    materialformat.cpp
    openpbrmapping.cpp
    renderpipeline.cpp
    samplecountplanning.cpp
    sceneformat.cpp
    surfaceformat.cpp
    targetplanning.cpp
    targetrenderplanning.cpp
)

set_target_properties(pelican_project PROPERTIES CXX_STANDARD 20)
set_target_properties(pelican_project PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(pelican_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_project PUBLIC nlohmann_json PRIVATE picosha2)
