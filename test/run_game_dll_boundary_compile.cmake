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
    # Newer MSVC (19.5x) reports a throw inside a consteval function only as
    # "failure was caused by evaluating a throw sub-expression" at file(line,column), without
    # quoting the thrown text. The named diagnostic is the text of that throw statement, so read
    # the source line each reported location points at and look for it there.
    string(REGEX MATCHALL "[A-Za-z]:[^\n\r()]+\\([0-9]+(,[0-9]+)?\\)" reported_locations "${build_log}")
    list(REMOVE_DUPLICATES reported_locations)
    foreach(location IN LISTS reported_locations)
        string(REGEX REPLACE "^(.*)\\(([0-9]+)(,[0-9]+)?\\)$" "\\1" location_file "${location}")
        string(REGEX REPLACE "^(.*)\\(([0-9]+)(,[0-9]+)?\\)$" "\\2" location_line "${location}")
        string(STRIP "${location_file}" location_file)
        if(EXISTS "${location_file}" AND NOT IS_DIRECTORY "${location_file}")
            # Walk to the line by newlines: a list of lines would drop empty lines and split on ';'.
            file(READ "${location_file}" location_rest)
            set(location_reached TRUE)
            math(EXPR location_skips "${location_line} - 1")
            if(location_skips GREATER 0)
                foreach(unused RANGE 1 ${location_skips})
                    string(FIND "${location_rest}" "\n" location_newline)
                    if(location_newline EQUAL -1)
                        set(location_reached FALSE)
                        break()
                    endif()
                    math(EXPR location_next "${location_newline} + 1")
                    string(SUBSTRING "${location_rest}" ${location_next} -1 location_rest)
                endforeach()
            endif()
            if(location_reached)
                string(FIND "${location_rest}" "\n" location_newline)
                if(NOT location_newline EQUAL -1)
                    string(SUBSTRING "${location_rest}" 0 ${location_newline} location_rest)
                endif()
                string(FIND "${location_rest}" "${EXPECTED_DIAGNOSTIC}" diagnostic_offset)
                if(NOT diagnostic_offset EQUAL -1)
                    break()
                endif()
            endif()
        endif()
    endforeach()
endif()
if(diagnostic_offset EQUAL -1)
    message(FATAL_ERROR
        "${TARGET_NAME} failed for an unrelated reason; expected diagnostic '${EXPECTED_DIAGNOSTIC}'\n${build_log}")
endif()

message(STATUS
    "${TARGET_NAME} rejected at the named game-DLL boundary: ${EXPECTED_DIAGNOSTIC}")
