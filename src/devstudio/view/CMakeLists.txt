add_library(pelican_studio_view STATIC
    uimain.cpp
    mainwindow.cpp
    frameplanwidget.cpp
    frameplanwidget.hpp
    frameplangraphics.cpp
    frameplangraphics.hpp
    frameplanedit.cpp
    frameplanedit.hpp
    frameplaneditcapability.cpp
    frameplaneditcapability.hpp
    renderconfigdigest.hpp
    # UX-6b: the frame's GPU time and the last edit's before and after.
    planperfhub.cpp
    planperfhub.hpp
    rendertargetform.cpp
    rendertargetform.hpp
    materialpassedit.cpp
    materialpassedit.hpp
    materialpassform.cpp
    materialpassform.hpp
    nodegraphics.cpp
    nodegraphics.hpp
    nodelayout.cpp
    nodelayout.hpp
    panelhelp.hpp
    studiotheme.cpp
    studiotheme.hpp
    frameplanreadcapability.cpp
    frameplanreadcapability.hpp
    renderpassauthoringcapability.cpp
    renderpassauthoringcapability.hpp
    fullscreenpasswidget.cpp
    fullscreenpasswidget.hpp
    fullscreenpasssettings.cpp
    fullscreenpasssettings.hpp
    # UX-4a: the forms that edit parts of a pass declaration (Pass Editor now,
    # the Graph's detail panel next).
    passforms/passform.cpp
    passforms/passform.hpp
    passforms/attachmentopsform.cpp
    passforms/attachmentopsform.hpp
    passforms/materialroutingform.cpp
    passforms/materialroutingform.hpp
    passforms/materialsettingsform.cpp
    passforms/materialsettingsform.hpp
    passforms/rasterstateform.cpp
    passforms/rasterstateform.hpp
    passforms/shaderdefinesform.cpp
    passforms/shaderdefinesform.hpp
    fullscreenshadertemplate.cpp
    fullscreenshadertemplate.hpp
    shaderinputcontract.cpp
    shaderinputcontract.hpp
    shadercompilelogpanel.cpp
    shadercompilelogpanel.hpp
    shaderdiagnostics.cpp
    shaderdiagnostics.hpp
    shaderreloadfollower.cpp
    shaderreloadfollower.hpp
    inspectorwidget.cpp
    inspectorwidget.hpp
    outlinertree.cpp
    outlinertree.hpp
    runtimeoutliner.cpp
    runtimeoutliner.hpp
    componentaddlist.cpp
    componentaddlist.hpp
    graphattachmentwidget.cpp
    graphattachmentwidget.hpp
    graphcanvaswidget.cpp
    graphcanvaswidget.hpp
    graphcanvasdrawing.cpp
    graphcanvasdrawing.hpp
    graphcanvasloopfields.cpp
    graphcanvasloopfields.hpp
    # UX-11: the tag picker (the Loop's and the material pass's Tags) and a
    # material pass's settings in the Pass Editor's forms.
    graphcanvastagpicker.cpp
    graphcanvastagpicker.hpp
    graphcanvasmaterialfields.cpp
    graphcanvasmaterialfields.hpp
    graphcanvasrenderfields.cpp
    graphcanvasrenderfields.hpp
    # UX-8: a curve's keys, a playback's map, a scalar's numbers, a pass's buffers.
    graphcanvasvaluefields.cpp
    graphcanvasvaluefields.hpp
    # EL-5s: a compute node's Shader and what the engine could not read.
    graphcanvascomputefields.cpp
    graphcanvascomputefields.hpp
    # GL-3a: fold groups on the Graph canvas, and their Decisions.
    graphcanvasfoldgroups.cpp
    graphcanvasfoldgroups.hpp
    graphcanvasreadcapability.cpp
    graphcanvasreadcapability.hpp
    proceduralassetdialog.cpp
    proceduralassetdialog.hpp
    newprojectdialog.cpp
    newprojectdialog.hpp
    projectmigrationwidget.cpp
    projectmigrationwidget.hpp
    taskconnectiongraphics.cpp
    taskconnectiongraphics.hpp
    renderfeatureswidget.cpp
    renderfeatureswidget.hpp
    # RSX: the rendering section's own keys (EL-3).
    renderingsettingswidget.cpp
    renderingsettingswidget.hpp
    viewselectfield.cpp
    viewselectfield.hpp
    playcontrols.cpp
    playcontrols.hpp
    studioscreen.cpp
    studioscreen.hpp
)

set_target_properties(pelican_studio_view PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
)
# graphcanvaswidget.cpp grew past the object format's section limit (C1128).
if(MSVC)
    target_compile_options(pelican_studio_view PRIVATE /bigobj)
endif()
target_include_directories(pelican_studio_view PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(pelican_studio_view
    PUBLIC
        Qt6::Core
        Qt6::Widgets
    PRIVATE
        pelican_project
        pelican_studio_automation
        pelican_studio_layout
        pelican_studio_viewport
        pelican_studio_outliner
        pelican_studio_inspector_model
        pelican_studio_gizmo_model
        pelican_studio_frame_plan
)

# Production and integration tests both link this exact compiled view target.
target_link_libraries(pelican_studio PRIVATE pelican_studio_view)

# custom target for debug
if(WIN32)
add_custom_target(deploy_studio
  COMMAND $<TARGET_FILE:Qt6::windeployqt> $<TARGET_FILE:pelican_studio>
  DEPENDS pelican_studio
  VERBATIM
)
add_custom_target(run_studio
    COMMAND $<TARGET_FILE:pelican_studio>
    DEPENDS deploy_studio
)
else()
add_custom_target(run_studio
    COMMAND $<TARGET_FILE:pelican_studio>
    DEPENDS pelican_studio
)
endif()
