add_library(pelican_project
    assetdataformat.cpp
    audioanalysisformat.cpp
    assetsmanifest.cpp
    authoringlowering.cpp
    featurecompose.cpp
    featurejitter.cpp
    filereplace.cpp
    frameresolutionwire.cpp
    graphvariantpolicy.cpp
    graphcomposition.cpp
    graphfold.cpp
    graphloopexpression.cpp
    graphmodule.cpp
    graphparameterschema.cpp
    graphpartition.cpp
    graphtemplate.cpp
    rendergraphcomposition.cpp
    gpudrawtiming.cpp
    gltfmateriallowering.cpp
    attachmentoperations.cpp
    framegraphbufferdefinition.cpp
    generationresources.cpp
    imageformat.cpp
    renderresolutiondomain.cpp
    imageusage.cpp
    importmanifest.cpp
    importrules.cpp
    inputactionoverlay.cpp
    jsondocument.cpp
    jsonrpc.cpp
    lightingdata.cpp
    logicalrendergraph.cpp
    logicalframegraphshadow.cpp
    frameplan.cpp
    imagesource.cpp
    gpudrawsource.cpp
    rendergraphir.cpp
    rendergraphfingerprint.cpp
    rendergraphparse.cpp
    passshaderdefines.cpp
    rayrecordabi.cpp
    nodeschedule.cpp
    logicalrendertype.cpp
    materialdrawloop.cpp
    materialpassdecompose.cpp
    materialscreeninput.cpp
    materialoutput.cpp
    materiallowering.cpp
    materialformat.cpp
    openpbrmapping.cpp
    packagemanifest.cpp
    passfieldownership.cpp
    passfusion.cpp
    passshapepolicy.cpp
    physicaltargetplanwire.cpp
    prefab.cpp
    projectdocumenttransaction.cpp
    projectformat.cpp
    projectmigration.cpp
    projectpathresolver.cpp
    projecttemplate.cpp
    rasterpass.cpp
    rendercomponent.cpp
    renderconfigdocument.cpp
    renderfeatureoverlay.cpp
    renderingjsonnormalize.cpp
    renderpipeline.cpp
    renderpipelinerecipe.cpp
    samplecountplanning.cpp
    schemawire.cpp
    schemawire.hpp
    sceneformat.cpp
    sha256stream.cpp
    shadersourceresolver.cpp
    shaderreference.cpp
    computetaskdefinition.cpp
    shaderresourceport.cpp
    surfaceformat.cpp
    targetplanning.cpp
    targetrenderplanning.cpp
    targetrendermemory.cpp
    vulkancompletephysicalplan.cpp
    vulkanphysicalfragment.cpp
    vulkanviewplanning.cpp
    xrmultiviewprofile.cpp
    xrtargetpolicy.cpp
)

set_target_properties(pelican_project PROPERTIES CXX_STANDARD 20)
set_target_properties(pelican_project PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_definitions(pelican_project PUBLIC
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
    PELICAN_WITH_ANIMATION=$<BOOL:${PELICAN_WITH_ANIMATION}>
    # Every option projectrequirements.def names, and the audio module's
    # conditional registration in graphmodule.cpp.
    PELICAN_WITH_AUDIO=$<BOOL:${PELICAN_WITH_AUDIO}>
    PELICAN_WITH_PHYSICS=$<BOOL:${PELICAN_WITH_PHYSICS}>
    PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
    PELICAN_WITH_SEQPLAYER=$<BOOL:${PELICAN_WITH_SEQPLAYER}>
    PELICAN_WITH_CANVAS=$<BOOL:${PELICAN_WITH_CANVAS}>
)
if(PELICAN_WITH_ANIMATION)
target_sources(pelican_project PRIVATE posegraphmodule.cpp)
endif()
# The canvas module and its lowering; an OFF build compiles none of it.
if(PELICAN_WITH_CANVAS)
target_sources(pelican_project PRIVATE
    canvas/canvasmodule.cpp
    canvas/canvaslowering.cpp
    canvas/canvasreference.cpp
    # CV-5: the ship policy's static runs, folded into images.
    canvas/canvasship.cpp
    # CV-2b: blend modes and colour filters, shared by the shader and the reference.
    canvas/canvasblend.cpp
    # CV-6a: a placed stack's corners on the screen.
    canvas/canvasplace.cpp
    # CV-4: shape.text -- the font file, the distance fields and the atlas.
    canvas/canvastext.cpp
    canvas/distancefield.cpp
    canvas/fontfile.cpp
)
# stb_truetype reads the fonts, stb_image_write encodes the atlas.
target_link_libraries(pelican_project PRIVATE stb)
endif()
target_include_directories(pelican_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_project PUBLIC nlohmann_json pelican_schema_leaf PRIVATE picosha2)
if(WIN32)
    # Sha256Stream (assetFileSha256, the file watcher) hashes with CNG (BCrypt) on Windows.
    target_link_libraries(pelican_project PRIVATE bcrypt)
endif()
