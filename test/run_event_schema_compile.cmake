if(NOT DEFINED PROJECT_SOURCE_DIR OR
   NOT DEFINED BUILD_DIR OR
   NOT DEFINED FIXTURE_SOURCE OR
   NOT DEFINED TARGET_NAME OR
   NOT DEFINED EXPECT_FAILURE)
    message(FATAL_ERROR
        "PROJECT_SOURCE_DIR, BUILD_DIR, FIXTURE_SOURCE, TARGET_NAME, and EXPECT_FAILURE are required")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${PROJECT_SOURCE_DIR}"
    -B "${BUILD_DIR}"
    "-DTARGET_NAME=${TARGET_NAME}"
    "-DFIXTURE_SOURCE=${FIXTURE_SOURCE}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}"
)
if(DEFINED GENERATOR AND NOT GENERATOR STREQUAL "")
    list(APPEND configure_command -G "${GENERATOR}")
endif()
if(DEFINED GENERATOR_INSTANCE AND NOT GENERATOR_INSTANCE STREQUAL "")
    list(APPEND configure_command "-DCMAKE_GENERATOR_INSTANCE=${GENERATOR_INSTANCE}")
endif()
if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_command -T "${GENERATOR_TOOLSET}")
endif()
if(DEFINED TOOLCHAIN_FILE AND NOT TOOLCHAIN_FILE STREQUAL "")
    list(APPEND configure_command "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()
if(DEFINED CXX_COMPILER AND NOT CXX_COMPILER STREQUAL "")
    list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "${TARGET_NAME} fixture configure failed\nstdout:\n${configure_stdout}\nstderr:\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${TARGET_NAME}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(EXPECT_FAILURE)
    if(result EQUAL 0)
        message(FATAL_ERROR "${TARGET_NAME} unexpectedly compiled successfully")
    endif()
else()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${TARGET_NAME} failed to compile\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
endif()
