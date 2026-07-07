add_library(pelican_project
    featurecompose.cpp
    importmanifest.cpp
    jsonrpc.cpp
    sceneformat.cpp
)

set_target_properties(pelican_project PROPERTIES CXX_STANDARD 20)
set_target_properties(pelican_project PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(pelican_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_project PUBLIC nlohmann_json)
