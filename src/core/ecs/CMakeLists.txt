target_sources(pelican_core PRIVATE
    core.cpp
    chunk.cpp
    componentinfo.cpp
    predefined.cpp
)

# [BENCHMARK_ONLY] Start
add_executable(ecs_benchmark benchmark.cpp)
target_link_libraries(ecs_benchmark PRIVATE pelican_core)
set_target_properties(ecs_benchmark PROPERTIES CXX_STANDARD 20)
# [BENCHMARK_ONLY] End
