foreach(required IN ITEMS PROJECT_SOURCE_DIR BUILD_DIR FIXTURE_SOURCE
                          GAME_LOGIC_ENTRY USERPUBLIC_INCLUDE CORE_INCLUDE
                          NLOHMANN_INCLUDE HOST_IMPORT_LIBRARY CONFIG TARGET_NAME
                          EXPECTED_DIAGNOSTIC)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${PROJECT_SOURCE_DIR}"
    -B "${BUILD_DIR}"
    "-DTARGET_NAME=${TARGET_NAME}"
    "-DFIXTURE_SOURCE=${FIXTURE_SOURCE}"
    "-DGAME_LOGIC_ENTRY=${GAME_LOGIC_ENTRY}"
    "-DUSERPUBLIC_INCLUDE=${USERPUBLIC_INCLUDE}"
    "-DCORE_INCLUDE=${CORE_INCLUDE}"
    "-DNLOHMANN_INCLUDE=${NLOHMANN_INCLUDE}"
    "-DHOST_IMPORT_LIBRARY=${HOST_IMPORT_LIBRARY}")
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
if(DEFINED CXX_COMPILER AND NOT CXX_COMPILER STREQUAL "")
    list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "${TARGET_NAME} fixture configure failed\n${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}"
            --target "${TARGET_NAME}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr)
if(build_result EQUAL 0)
    message(FATAL_ERROR
        "${TARGET_NAME} unexpectedly linked: the game DLL recovered a host owner capability")
endif()

set(build_log "${build_stdout}\n${build_stderr}")
string(FIND "${build_log}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
if(diagnostic_offset EQUAL -1)
    message(FATAL_ERROR
        "${TARGET_NAME} failed for an unrelated reason; expected diagnostic '${EXPECTED_DIAGNOSTIC}'\n${build_log}")
endif()

message(STATUS
    "${TARGET_NAME} rejected at the named game-DLL boundary: ${EXPECTED_DIAGNOSTIC}")
