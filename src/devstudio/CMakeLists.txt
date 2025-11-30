find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PELICAN_INSTALL_DIR})
qt_add_executable(pelican_studio main.cpp mainwindow.cpp)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

add_custom_command(
  TARGET pelican_studio POST_BUILD
  COMMAND ${WINDEPLOYQT_EXECUTABLE} $<TARGET_FILE:pelican_studio>
)

target_link_libraries(pelican_studio PRIVATE Qt6::Core Qt6::Widgets)
target_link_libraries(pelican_studio PRIVATE pelican_core)
target_link_libraries(pelican_studio PRIVATE argparse)

