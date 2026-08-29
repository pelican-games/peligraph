add_library(pelican_scene_authoring_authority INTERFACE)
target_sources(pelican_scene_authoring_authority INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/authoringsceneauthority.hpp)

target_sources(pelican_core PRIVATE
    assetsverification.cpp
    engineresources.cpp
    fileio.cpp
    imageloader.cpp
    ktx2.cpp
    pathresolver.cpp
    renderconfigcandidate.cpp
    projectsrc.cpp
    authoringscenedocument.cpp
    sceneauthoringadapter.cpp
    resolvedscene.cpp
    componentcodec.cpp
    editorpreviewprojection.cpp
    editorprojectiontransaction.cpp
    editorprojectionadapters.cpp
    basicconfig.cpp
    scene.cpp
    vrmadecoder.cpp
)
target_link_libraries(pelican_core PRIVATE
    pelican_scene_authoring_authority)
