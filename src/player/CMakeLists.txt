add_executable(pelican_player main.cpp)
set_target_properties(pelican_player PROPERTIES CXX_STANDARD 20)

target_link_libraries(pelican_player pelican_core)
