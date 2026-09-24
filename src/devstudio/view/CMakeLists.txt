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
    fullscreenshadertemplate.cpp
    fullscreenshadertemplate.hpp
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
    componentaddlist.cpp
    componentaddlist.hpp
    graphattachmentwidget.cpp
    graphattachmentwidget.hpp
    graphcanvaswidget.cpp
    graphcanvaswidget.hpp
    graphcanvasdrawing.cpp
    graphcanvasdrawing.hpp
    graphcanvasreadcapability.cpp
    graphcanvasreadcapability.hpp
    proceduralassetdialog.cpp
    proceduralassetdialog.hpp
    newprojectdialog.cpp
    newprojectdialog.hpp
    taskconnectiongraphics.cpp
    taskconnectiongraphics.hpp
    renderfeatureswidget.cpp
    renderfeatureswidget.hpp
)

set_target_properties(pelican_studio_view PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
)
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
