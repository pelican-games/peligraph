# IL-1: pelican_cli intro-link serve and the development-form client.
# The server's stdout (first line "port <N>") is the client's stdin; the
# server's stdin is a file with "seek 2.5" and "pause". The client's values
# must equal the release-form probe's (built over the bake of the same
# fixture), and the release probe must not import the socket library.
foreach(required IN ITEMS CLI GLSLC CLIENT BAKED_PROBE FIXTURE OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
set(project "${OUT_DIR}/project")
file(COPY "${FIXTURE}/" DESTINATION "${project}")

# The rewrite the client applies: fade_linear's middle key 1.0 -> 3.0, so the
# value at 0.5 s moves from 0.5 to 1.5.
file(READ "${project}/passes/main.json" main_json)
set(original_key "{\"time\": 1.0, \"value\": 1.0}")
string(FIND "${main_json}" "${original_key}" key_at)
if(key_at EQUAL -1)
    message(FATAL_ERROR "fixture no longer has the key ${original_key}")
endif()
string(REPLACE "${original_key}" "{\"time\": 1.0, \"value\": 3.0}" reloaded_json "${main_json}")
file(WRITE "${OUT_DIR}/reloaded_main.json" "${reloaded_json}")
file(WRITE "${OUT_DIR}/server_stdin.txt" "seek 2.5\npause\n")

execute_process(
    COMMAND "${CLI}" intro-link serve "${project}" --port 0 --exit-after-client --glslc "${GLSLC}"
    COMMAND "${CLIENT}" "${project}/passes/main.json" "${OUT_DIR}/reloaded_main.json"
    INPUT_FILE "${OUT_DIR}/server_stdin.txt"
    RESULTS_VARIABLE results
    OUTPUT_VARIABLE client_stdout
    ERROR_VARIABLE stderr
    TIMEOUT 90
)
list(LENGTH results result_count)
list(GET results 0 server_result)
if(result_count GREATER 1)
    list(GET results 1 client_result)
else()
    set(client_result "missing")
endif()
if(NOT server_result STREQUAL "0" OR NOT client_result STREQUAL "0")
    message(FATAL_ERROR "serve=${server_result} client=${client_result}\n"
        "client stdout:\n${client_stdout}\nstderr (server and client):\n${stderr}")
endif()

foreach(needle IN ITEMS
        "check shader main_frag magic=0x07230203"
        "check time paused=1 seek=2.50"
        "check reload fade_linear(0.5) 0.500 -> 1.500")
    string(FIND "${client_stdout}" "${needle}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR "client did not report '${needle}'\n${client_stdout}\n${stderr}")
    endif()
endforeach()

execute_process(
    COMMAND "${BAKED_PROBE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
)
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "release probe failed with ${probe_result}\n${probe_stdout}\n${probe_stderr}")
endif()
foreach(needle IN ITEMS "shader main_frag words=" "magic=0x07230203 changed=0 error=none" "time 0")
    string(FIND "${probe_stdout}" "${needle}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR "release probe did not report '${needle}'\n${probe_stdout}")
    endif()
endforeach()

# Only the "value ..." lines: the snapshot the client received against the
# tables the release form baked.
string(REGEX MATCHALL "value [^\r\n]*" client_values "${client_stdout}")
string(REGEX MATCHALL "value [^\r\n]*" probe_values "${probe_stdout}")
list(LENGTH client_values client_value_count)
if(client_value_count EQUAL 0)
    message(FATAL_ERROR "client printed no values\n${client_stdout}")
endif()
if(NOT client_values STREQUAL probe_values)
    string(REPLACE ";" "\n" client_lines "${client_values}")
    string(REPLACE ";" "\n" probe_lines "${probe_values}")
    message(FATAL_ERROR "development and release values differ\nclient:\n${client_lines}\n\nrelease:\n${probe_lines}")
endif()

# The release form compiles no socket code: its executable does not name the
# Windows socket library, while the development client does.
if(WIN32)
    file(STRINGS "${BAKED_PROBE}" probe_socket_imports REGEX "[Ww][Ss]2_32")
    file(STRINGS "${CLIENT}" client_socket_imports REGEX "[Ww][Ss]2_32")
    if(probe_socket_imports)
        message(FATAL_ERROR "the release probe imports the socket library: ${probe_socket_imports}")
    endif()
    if(NOT client_socket_imports)
        message(FATAL_ERROR "the development client does not name ws2_32; the import check sees nothing")
    endif()
endif()

message(STATUS "intro-link serve: ${client_value_count} values match the release form; shader, time and reload reached the client")
