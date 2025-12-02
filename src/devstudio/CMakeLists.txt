# qt target settings
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Quick)
qt_standard_project_setup(REQUIRES 6.10)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PELICAN_INSTALL_DIR})
add_executable(pelican_studio main.cpp)

qt_add_qml_module(pelican_studio
    URI pelican_studio
    QML_FILES
        view/MainWindow.qml
)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

add_custom_command(
  TARGET pelican_studio POST_BUILD
  COMMAND ${WINDEPLOYQT_EXECUTABLE} $<TARGET_FILE:pelican_studio>
)

# external libs
target_link_libraries(pelican_studio PRIVATE pelican_core)
target_link_libraries(pelican_studio PRIVATE argparse)

# subdirs
add_subdirectory(model)
add_subdirectory(view)
