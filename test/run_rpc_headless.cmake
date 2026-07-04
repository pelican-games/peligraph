if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED PROJECT_DIR)
    message(FATAL_ERROR "PROJECT_DIR is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

set(script_path "${OUT_DIR}/rpc_script.ndjson")
set(capture_path "${OUT_DIR}/rpc_capture.png")
file(WRITE "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"set_time\",\"params\":{\"t\":1.25}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"render_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"capture\",\"params\":{\"path\":\"${capture_path}\"}}\n"
"{bad json\n"
"{\"jsonrpc\":\"2.0\",\"method\":\"render_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"missing_method\",\"params\":{}}\n"
)

function(validate_rpc_stdout stdout label)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REGEX REPLACE "\n$" "" trimmed "${normalized}")
    if(trimmed STREQUAL "")
        message(FATAL_ERROR "${label}: rpc stdout was empty")
    endif()

    string(REPLACE "\n" ";" lines "${trimmed}")
    list(LENGTH lines line_count)
    if(NOT line_count EQUAL 7)
        message(FATAL_ERROR "${label}: expected 7 JSON-RPC response lines, got ${line_count}\nstdout:\n${stdout}")
    endif()

    foreach(line IN LISTS lines)
        if(NOT line MATCHES [=[^\{.*"jsonrpc":"2\.0".*\}$]=])
            message(FATAL_ERROR "${label}: stdout contains a non-protocol line:\n${line}\nfull stdout:\n${stdout}")
        endif()
    endforeach()

    list(GET lines 0 line0)
    list(GET lines 1 line1)
    list(GET lines 2 line2)
    list(GET lines 3 line3)
    list(GET lines 4 line4)
    list(GET lines 5 line5)
    list(GET lines 6 line6)

    if(NOT line0 MATCHES [=["id":1]=] OR NOT line0 MATCHES [=["result"]=])
        message(FATAL_ERROR "${label}: set_time response did not look successful:\n${line0}")
    endif()
    if(NOT line1 MATCHES [=["id":2]=] OR NOT line1 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: step_frame response did not report frame 1:\n${line1}")
    endif()
    if(NOT line2 MATCHES [=["id":3]=] OR NOT line2 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: render_frame response did not keep frame 1:\n${line2}")
    endif()
    if(NOT line3 MATCHES [=["id":4]=] OR NOT line3 MATCHES [=["path"]=])
        message(FATAL_ERROR "${label}: capture response did not include a path:\n${line3}")
    endif()
    if(NOT line4 MATCHES [=["code":-32700]=])
        message(FATAL_ERROR "${label}: malformed JSON did not return -32700:\n${line4}")
    endif()
    if(NOT line5 MATCHES [=["code":-32600]=])
        message(FATAL_ERROR "${label}: id-less request did not return -32600:\n${line5}")
    endif()
    if(NOT line6 MATCHES [=["code":-32601]=])
        message(FATAL_ERROR "${label}: unknown method did not return -32601:\n${line6}")
    endif()
endfunction()

function(run_rpc_once label output_var)
    file(REMOVE "${capture_path}")
    execute_process(
        COMMAND "${PLAYER}"
            --rpc
            --headless
            --project "${PROJECT_DIR}"
            --size 160x90
            --fps 30
        WORKING_DIRECTORY "${PLAYER_DIR}"
        INPUT_FILE "${script_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label}: rpc player run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR "${label}: rpc player run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    validate_rpc_stdout("${stdout}" "${label}")

    if(NOT EXISTS "${capture_path}")
        message(FATAL_ERROR "${label}: rpc capture output missing: ${capture_path}")
    endif()
    file(SIZE "${capture_path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "${label}: rpc capture output is empty: ${capture_path}")
    endif()

    set(${output_var} "${stdout}" PARENT_SCOPE)
endfunction()

run_rpc_once("first run" first_stdout)
run_rpc_once("second run" second_stdout)

if(NOT first_stdout STREQUAL second_stdout)
    message(FATAL_ERROR "rpc response stream is not deterministic\nfirst:\n${first_stdout}\nsecond:\n${second_stdout}")
endif()
