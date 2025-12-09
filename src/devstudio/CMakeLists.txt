include(view/ui_depended_setup.cmake)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PELICAN_INSTALL_DIR})
add_executable(pelican_studio main.cpp)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

# external libs
target_link_libraries(pelican_studio PRIVATE pelican_core)
target_link_libraries(pelican_studio PRIVATE argparse)

# subdirs
add_subdirectory(model)
add_subdirectory(view)
