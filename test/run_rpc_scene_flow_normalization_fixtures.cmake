include("${CMAKE_CURRENT_LIST_DIR}/rpc_scene_flow_normalize.cmake")

set(fixture_root "${CMAKE_CURRENT_LIST_DIR}/fixtures/rpc_scene_flow")
foreach(fixture IN ITEMS baseline measured_changed structure_changed driver_changed logical_changed)
    file(READ "${fixture_root}/${fixture}.ndjson" ${fixture})
    pelican_normalize_rpc_stdout("${${fixture}}" normalized_${fixture})
endforeach()

if(NOT normalized_baseline STREQUAL normalized_measured_changed)
    message(FATAL_ERROR
        "usage/budget-only fixture did not normalize to the deterministic baseline\n"
        "baseline:\n${normalized_baseline}\nchanged:\n${normalized_measured_changed}")
endif()

if(normalized_baseline STREQUAL normalized_structure_changed)
    message(FATAL_ERROR "memory heap structure change was incorrectly normalized away")
endif()

if(normalized_baseline STREQUAL normalized_driver_changed)
    message(FATAL_ERROR "driver availability change was incorrectly normalized away")
endif()

if(normalized_baseline STREQUAL normalized_logical_changed)
    message(FATAL_ERROR "engine logical memory change was incorrectly normalized away")
endif()

message(STATUS "rpc scene flow normalization fixtures: PASS")
