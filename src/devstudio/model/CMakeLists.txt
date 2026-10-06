add_library(pelican_studio_outliner STATIC
    project.cpp
    selection.cpp
    outlineredit.cpp
    outlineredit.hpp
)

set_target_properties(pelican_studio_outliner PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_studio_outliner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_outliner PUBLIC pelican_project)

add_library(pelican_studio_inspector_model STATIC
    inspectormodel.cpp
    inspectormodel.hpp
    graphattachmentmodel.cpp
    graphattachmentmodel.hpp
    graphcanvasmodel.cpp
    graphcanvasmodel.hpp
    # UX-8: a curve's keys, a playback's fields, buffer ports, packed scalars.
    graphcanvasvalues.cpp
    # AB-1s: baked analysis tables -- read from the project, placed for a
    # shader, read again after a Re-bake.
    audioanalysistables.cpp
    audioanalysistables.hpp
    graphcanvasaudioanalysis.cpp
    # GL-3a: fold groups, read and written from the Graph tab.
    graphfoldgroups.cpp
    graphfoldgroups.hpp
    renderedithistory.cpp
    renderedithistory.hpp
    # RSX: the one reading of a render config ticket's last answer.
    rendereditcompletion.cpp
    rendereditcompletion.hpp
    # RT-A: one part follows a render edit's ticket for every panel that sends one.
    rendereditticket.cpp
    rendereditticket.hpp
    # RT-C: the one reading of a synchronous write's applied / restart_required.
    appliedstate.cpp
    appliedstate.hpp
    # RL-1s: render documents rewritten on disk, taken in by reload_render_documents.
    renderdocumentreload.cpp
    renderdocumentreload.hpp
    # BS-1s: the engine on its fallback generation, and the way back.
    renderfallback.cpp
    renderfallback.hpp
    playstatemodel.cpp
    playstatemodel.hpp
    # PM-3c: reads the play state through playstatemodel's parsePlayStatus.
    runtimeobjects.cpp
    runtimeobjects.hpp
    # UX-6b: the frame's GPU time and the plan's size, per generation.
    planperfmodel.cpp
    planperfmodel.hpp
    # UX-12b: the refusals FR-1 names with what is missing, and their fixes.
    featurerefusalfix.cpp
    featurerefusalfix.hpp
    # HW-2s / AV-1s: the Assets dock's watch switches and contents check.
    assetsdockmodel.cpp
    assetsdockmodel.hpp
    # MT-1s: the material editor's reads, writes and Undo (engine MT-1).
    materialsmodel.cpp
    materialsmodel.hpp
    # RTL-2s: the ray group layout, its bindings and the plans that change them.
    raylayoutmodel.cpp
    raylayoutmodel.hpp
    # RTL-2s-c: the one set of shader templates of a project with a ray layout.
    raylayouttemplate.cpp
    raylayouttemplate.hpp
    # PB-1s: the Problems window's list (engine PB-1's get_problems and the
    # contents check).
    problemsmodel.cpp
    problemsmodel.hpp
    # MI-1s: an outside model placed in the project and registered (engine MI-1).
    modelimport.cpp
    modelimport.hpp
)

set_target_properties(pelican_studio_inspector_model PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_studio_inspector_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_inspector_model PUBLIC
    pelican_project
    pelican_studio_outliner
    # Q-19: the Graph tab reads a pass's pass_inputs the way the Frame Plan does.
    pelican_studio_frame_plan
)

add_library(pelican_studio_gizmo_model STATIC
    gizmomodel.cpp
    gizmomodel.hpp
)

set_target_properties(pelican_studio_gizmo_model PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_studio_gizmo_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_gizmo_model PUBLIC
    nlohmann_json
    pelican_studio_outliner
)

add_library(pelican_studio_frame_plan STATIC
    frameplanmodel.cpp
    frameplanmodel.hpp
    frameplanresourcekind.cpp
    frameplanresourcekind.hpp
)

set_target_properties(pelican_studio_frame_plan PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_studio_frame_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_frame_plan PUBLIC
    nlohmann_json
    pelican_project
)

target_link_libraries(pelican_studio PRIVATE pelican_studio_outliner)
target_link_libraries(pelican_studio PRIVATE pelican_studio_inspector_model)
target_link_libraries(pelican_studio PRIVATE pelican_studio_gizmo_model)
target_link_libraries(pelican_studio PRIVATE pelican_studio_frame_plan)
