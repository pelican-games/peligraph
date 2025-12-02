# qt target settings
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Quick)
qt_standard_project_setup(REQUIRES 6.10)

target_sources(pelican_studio PRIVATE
    uimain.cpp
)

qt_add_library(pelican_studio_qml STATIC)
qt_add_qml_module(pelican_studio_qml
    URI pelican_studio
    QML_FILES
        MainWindow.qml
)

target_link_libraries(pelican_studio PRIVATE pelican_studio_qmlplugin)
target_link_libraries(pelican_studio PRIVATE Qt6::Core Qt6::Widgets Qt6::Quick)

# custom target for debug
if(WIN32)
add_custom_target(deploy_studio
  COMMAND ${WINDEPLOYQT_EXECUTABLE} $<TARGET_FILE:pelican_studio>
  DEPENDS pelican_studio
)
add_custom_target(run_studio
    COMMAND set QML2_IMPORT_PATH=${QT_DIR}/qml
    COMMAND $<TARGET_FILE:pelican_studio>
    DEPENDS deploy_studio
)
else()
add_custom_target(run_studio
    COMMAND env QML2_IMPORT_PATH=${QT_DIR}/qml $<TARGET_FILE:pelican_studio>
    DEPENDS pelican_studio
)
endif()
