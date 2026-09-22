if(NOT DEFINED TEST_BINARY_DIR)
    message(FATAL_ERROR "TEST_BINARY_DIR is required")
endif()
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(discovery "${TEST_BINARY_DIR}/sample-123_tests.cmake")
file(WRITE "${discovery}" [====[
add_test([==[CPU case]==] [==[C:/with spaces/test.exe]==] [==[a;b]==])
set_tests_properties([==[CPU case]==] PROPERTIES LABELS cpu)
add_test([==[GPU case]==] [==[C:/with spaces/test.exe]==] [==[a;b]==])
set_tests_properties([==[GPU case]==] PROPERTIES LABELS [==[gpu;render]==] TIMEOUT 180)
set(sample_TESTS [==[CPU case]==] [==[GPU case]==])
]====])
set(patcher "${CMAKE_CURRENT_LIST_DIR}/patch_catch_gpu_lease_tests.cmake")
function(patch_discovery)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DDIRECTORY=${TEST_BINARY_DIR}" -DTARGET=sample
        "-DLAUNCHER=C:/with spaces/lease.exe" -P "${patcher}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "GPU discovery patch failed: ${result}")
    endif()
endfunction()
patch_discovery()
file(READ "${discovery}" first)
foreach(expected IN ITEMS
    "add_test([==[CPU case]==] [==[C:/with spaces/test.exe]==] [==[a;b]==])"
    "add_test([==[GPU case]==] [==[C:/with spaces/lease.exe]==] [==[--]==] [==[C:/with spaces/test.exe]==] [==[a;b]==])"
    "[==[LABELS]==] [==[gpu;render]==]"
    "set(sample_TESTS [==[CPU case]==] [==[GPU case]==])")
    string(FIND "${first}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Patched discovery lost ${expected}\n${first}")
    endif()
endforeach()
patch_discovery()
file(READ "${discovery}" second)
if(NOT first STREQUAL second)
    message(FATAL_ERROR "GPU discovery patch is not idempotent")
endif()
