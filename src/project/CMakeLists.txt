add_library(pelican_project
    assetdataformat.cpp
    assetsmanifest.cpp
    executionplan.cpp
    featurecompose.cpp
    featurejitter.cpp
    graphvariantpolicy.cpp
    gpudrawtiming.cpp
    gltfmateriallowering.cpp
    importmanifest.cpp
    importrules.cpp
    jsonrpc.cpp
    lightingdata.cpp
    logicalrendergraph.cpp
    logicalrendertype.cpp
    materialscreeninput.cpp
    materialoutput.cpp
    materiallowering.cpp
    materialformat.cpp
    openpbrmapping.cpp
    projectformat.cpp
    projectpathresolver.cpp
    rasterpass.cpp
    renderfeatureoverlay.cpp
    renderpipeline.cpp
    samplecountplanning.cpp
    sceneformat.cpp
    shaderresourceport.cpp
    surfaceformat.cpp
    targetplanning.cpp
    targetrenderplanning.cpp
    vulkancompletephysicalplan.cpp
    vulkanphysicalfragment.cpp
    vulkanviewplanning.cpp
    xrmultiviewprofile.cpp
    xrtargetpolicy.cpp
)

set_target_properties(pelican_project PROPERTIES CXX_STANDARD 20)
set_target_properties(pelican_project PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(pelican_project PUBLIC
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
)
target_include_directories(pelican_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_project PUBLIC nlohmann_json PRIVATE picosha2)
