add_executable(pelican-spv-link main.cpp)
pelican_runtime_target(pelican-spv-link)
set_target_properties(pelican-spv-link PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican-spv-link PRIVATE pelican_core nlohmann_json)
