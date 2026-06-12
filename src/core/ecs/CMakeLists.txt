target_sources(pelican_core PRIVATE
    componentinfo.cpp
    predefined.cpp
)

add_subdirectory(predefined)

# [BENCHMARK_ONLY] Start
add_executable(ecs_benchmark benchmark.cpp)
target_link_libraries(ecs_benchmark PRIVATE pelican_core)
set_target_properties(ecs_benchmark PROPERTIES CXX_STANDARD 20)
target_link_libraries(ecs_benchmark PRIVATE glm)
target_link_libraries(ecs_benchmark PRIVATE nlohmann_json)
# [BENCHMARK_ONLY] End
