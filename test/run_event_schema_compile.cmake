if(NOT DEFINED BUILD_DIR OR NOT DEFINED TARGET_NAME OR NOT DEFINED EXPECT_FAILURE)
    message(FATAL_ERROR "BUILD_DIR, TARGET_NAME, and EXPECT_FAILURE are required")
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
