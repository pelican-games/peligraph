include_guard(GLOBAL)

# Run one exact CTest capability case.  A required-capability lane must fail
# when discovery selected nothing and when the selected case reports Skip/Not
# Run; both outcomes otherwise look green in common smoke wrappers.
function(pelican_run_required_capability_ctest)
    cmake_parse_arguments(
        REQUIRED
        ""
        "BUILD_DIR;CONFIG;REGEX;LABEL;REPORT_DIR;FUNCTIONAL_LANE"
        ""
        ${ARGN})
    foreach(field IN ITEMS BUILD_DIR CONFIG REGEX LABEL REPORT_DIR FUNCTIONAL_LANE)
        if(NOT DEFINED REQUIRED_${field} OR
           "${REQUIRED_${field}}" STREQUAL "")
            message(FATAL_ERROR
                "pelican.required_capability.invalid_argument@1: missing ${field}")
        endif()
    endforeach()
    if(NOT CMAKE_CTEST_COMMAND)
        get_filename_component(cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)
        set(CMAKE_CTEST_COMMAND "${cmake_bin_dir}/ctest${CMAKE_EXECUTABLE_SUFFIX}")
    endif()

    file(MAKE_DIRECTORY "${REQUIRED_REPORT_DIR}")
    string(MAKE_C_IDENTIFIER "${REQUIRED_LABEL}" report_stem)
    set(junit "${REQUIRED_REPORT_DIR}/${report_stem}.xml")
    execute_process(
        COMMAND "${CMAKE_CTEST_COMMAND}"
            --test-dir "${REQUIRED_BUILD_DIR}"
            --build-config "${REQUIRED_CONFIG}"
            --output-on-failure
            --no-tests=error
            --output-junit "${junit}"
            -V
            -R "${REQUIRED_REGEX}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${REQUIRED_LABEL} required-capability CTest failed with ${result}\n"
            "stdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(NOT EXISTS "${junit}")
        message(FATAL_ERROR
            "${REQUIRED_LABEL} required-capability CTest emitted no JUnit report")
    endif()
    file(READ "${junit}" junit_text)
    string(TOLOWER "${junit_text}\n${stdout}\n${stderr}" observed)
    if(NOT observed MATCHES "<testcase[ >]")
        message(FATAL_ERROR
            "${REQUIRED_LABEL} required-capability CTest reported no testcases")
    endif()
    if(observed MATCHES "<skipped([ >])" OR
       observed MATCHES "status=\"(notrun|skipped)\"" OR
       observed MATCHES "[*][*][*]skipped" OR
       observed MATCHES "test cases:[^\r\n]*[|][^\r\n]*skipped" OR
       observed MATCHES "[^a-z]skipped:")
        message(FATAL_ERROR
            "${REQUIRED_LABEL} required-capability CTest reported a skip; "
            "${REQUIRED_FUNCTIONAL_LANE} capability is mandatory\n"
            "stdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    message(STATUS
        "${REQUIRED_LABEL} required-capability functional guarantee: "
        "${REQUIRED_FUNCTIONAL_LANE}; skipped execution is a failure")
endfunction()
