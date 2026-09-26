# Each Debug CRT failure mode must end the probe at once, with exit code 3 and
# the report on stderr. A modal dialog would instead hold the process until
# the TIMEOUT below kills it, which fails the test.
if(NOT DEFINED PROBE)
    message(FATAL_ERROR "PROBE is required")
endif()

set(expected_assert "CRT assertion \\(test build\\): [^\n]*crt_report_probe\\.cpp[^\n]*pelicanCrtProbeAssert")
set(expected_stl "CRT assertion \\(test build\\): [^\n]*vector erase iterator outside range")
set(expected_abort "abort\\(\\) has been called \\(test build\\)")

foreach(mode IN ITEMS assert stl abort)
    string(TIMESTAMP started "%s" UTC)
    execute_process(
        COMMAND "${PROBE}" ${mode}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 60
    )
    string(TIMESTAMP finished "%s" UTC)
    math(EXPR elapsed "${finished} - ${started}")

    if(stdout MATCHES "CRT_PROBE_SKIPPED")
        message(STATUS "CRT_PROBE_SKIPPED: ${stdout}")
        return()
    endif()
    set(report "mode=${mode} result=${result} elapsed=${elapsed}s\n"
               "stdout:\n${stdout}\nstderr:\n${stderr}")
    if(NOT result STREQUAL "3")
        message(FATAL_ERROR "expected exit code 3 from the probe\n${report}")
    endif()
    if(stdout MATCHES "CRT_PROBE_CONTINUED")
        message(FATAL_ERROR "the probe ran on past the failure\n${report}")
    endif()
    if(NOT stderr MATCHES "${expected_${mode}}")
        message(FATAL_ERROR
            "stderr does not carry the report (${expected_${mode}})\n${report}")
    endif()
    if(elapsed GREATER 20)
        message(FATAL_ERROR "the probe took too long to end\n${report}")
    endif()
    message(STATUS "mode=${mode}: exit 3 in ${elapsed}s, stderr: ${stderr}")
endforeach()
