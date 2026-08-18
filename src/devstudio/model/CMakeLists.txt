add_library(pelican_studio_outliner STATIC
    project.cpp
    selection.cpp
)

set_target_properties(pelican_studio_outliner PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_studio_outliner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_outliner PUBLIC pelican_project)

add_library(pelican_studio_inspector_model STATIC
    inspectormodel.cpp
    inspectormodel.hpp
)

set_target_properties(pelican_studio_inspector_model PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_studio_inspector_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_inspector_model PUBLIC
    pelican_project
    pelican_studio_outliner
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
