if(NOT DEFINED PELICAN_SOURCE_DIR OR
   NOT DEFINED PELICAN_TEST_BINARY_DIR)
    message(FATAL_ERROR "WP367a contrast requires source and binary directories")
endif()

set(root "${PELICAN_TEST_BINARY_DIR}/wp367a_required_capability_contrast")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}/pass" "${root}/skip" "${root}/reports")
file(TO_CMAKE_PATH "${CMAKE_COMMAND}" cmake_path)
file(WRITE "${root}/pass/CTestTestfile.cmake"
    "add_test(\"required pass\" \"${cmake_path}\" -E true)\n")
file(WRITE "${root}/skip/CTestTestfile.cmake"
    "add_test(\"required skip\" \"${cmake_path}\" -E echo \"WP367A_REQUIRED_SKIP\")\n"
    "set_tests_properties(\"required skip\" PROPERTIES SKIP_REGULAR_EXPRESSION \"WP367A_REQUIRED_SKIP\")\n")
file(WRITE "${root}/invoke.cmake" [=[
include("${PELICAN_SOURCE_DIR}/test/required_capability_ctest.cmake")
pelican_run_required_capability_ctest(
    BUILD_DIR "${BUILD_DIR}"
    CONFIG Debug
    REGEX "${TEST_REGEX}"
    LABEL "${LABEL}"
    REPORT_DIR "${REPORT_DIR}"
    FUNCTIONAL_LANE "flat/headless production RT2a")
]=])

function(run_probe name build_dir regex expect_success expected_error)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPELICAN_SOURCE_DIR=${PELICAN_SOURCE_DIR}"
            "-DBUILD_DIR=${build_dir}"
            "-DTEST_REGEX=${regex}"
            "-DLABEL=${name}"
            "-DREPORT_DIR=${root}/reports/${name}"
            -P "${root}/invoke.cmake"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(expect_success)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR
                "${name} unexpectedly failed\n${stdout}\n${stderr}")
        endif()
    else()
        if(result EQUAL 0)
            message(FATAL_ERROR "${name} unexpectedly succeeded")
        endif()
        string(TOLOWER "${stdout}\n${stderr}" combined)
        if(NOT combined MATCHES "${expected_error}")
            message(FATAL_ERROR
                "${name} did not expose '${expected_error}'\n${stdout}\n${stderr}")
        endif()
    endif()
endfunction()

run_probe(pass "${root}/pass" "^required pass$" TRUE "")
run_probe(zero "${root}/pass" "^missing required case$" FALSE
    "required-capability ctest failed with")
run_probe(skip "${root}/skip" "^required skip$" FALSE "reported a skip")
