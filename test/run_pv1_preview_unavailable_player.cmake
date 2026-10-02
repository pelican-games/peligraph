# PV-1: pelican_player started headless on a project whose rendering root
# authors a history target (test/fixtures/pv1_history_accum). Before PV-1 the
# start failed with "preview graph '<config>' rejects authored history target
# 'accum'". Now it starts and draws (successive frames differ: accum adds to
# its history), get_status and get_frame_plan say the one-shot preview is
# unavailable and why, render_preview refuses as preview.unavailable@1, and
# the log has one warning line saying so.
foreach(required PLAYER PLAYER_DIR FIXTURE_DIR OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/project")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${OUT_DIR}/project")

set(script_path "${OUT_DIR}/rpc_script.ndjson")
set(log_path "${OUT_DIR}/player.log")
set(capture_a "${OUT_DIR}/frame_a.png")
set(capture_b "${OUT_DIR}/frame_b.png")
file(WRITE "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"capture\",\"params\":{\"path\":\"${capture_a}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"capture\",\"params\":{\"path\":\"${capture_b}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"get_frame_plan\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"render_preview\",\"params\":{\"capture\":{\"width\":16,\"height\":16,\"pixel_encoding\":\"png\",\"camera\":{\"position\":[0.0,0.0,-3.0],\"target\":[0.0,0.0,0.0],\"up\":[0.0,1.0,0.0],\"projection\":{\"kind\":\"perspective\",\"yfov\":0.8,\"znear\":0.1,\"zfar\":100.0}},\"graph_generation\":0,\"max_bytes\":1048576}}}\n"
)

execute_process(
    COMMAND "${PLAYER}"
        --rpc
        --headless
        --project "${OUT_DIR}/project"
        --size 64x64
        --fps 30
        --log-file "${log_path}"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    INPUT_FILE "${script_path}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the player did not start or run the project: ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

# One response per request, found by id.
string(REPLACE "\r\n" "\n" normalized "${stdout}")
string(REPLACE ";" "\\;" normalized "${normalized}")
string(REPLACE "\n" ";" lines "${normalized}")
foreach(line IN LISTS lines)
    if(line STREQUAL "")
        continue()
    endif()
    string(JSON id ERROR_VARIABLE json_error GET "${line}" id)
    if(NOT json_error)
        set(response_${id} "${line}")
    endif()
endforeach()
foreach(id RANGE 1 7)
    if(NOT DEFINED response_${id})
        message(FATAL_ERROR "no response to request ${id}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
endforeach()
foreach(id 2 3 4 5 6)
    string(JSON failed ERROR_VARIABLE json_error GET "${response_${id}}" error)
    if(NOT json_error)
        message(FATAL_ERROR "request ${id} failed: ${response_${id}}")
    endif()
endforeach()

function(require_history_unavailable label response)
    string(JSON available GET "${response}" result preview available)
    string(JSON code GET "${response}" result preview reason code)
    string(JSON feature GET "${response}" result preview reason feature)
    string(JSON target GET "${response}" result preview reason target)
    string(JSON message GET "${response}" result preview reason message)
    if(available OR NOT code STREQUAL "preview.unsupported_authoring@1" OR
       NOT feature STREQUAL "history" OR NOT target STREQUAL "accum" OR
       NOT message STREQUAL "preview graph '<config>' rejects authored history target 'accum'")
        string(JSON preview GET "${response}" result preview)
        message(FATAL_ERROR "${label} did not say preview is unavailable for the history target accum: ${preview}")
    endif()
endfunction()
require_history_unavailable("get_status" "${response_1}")
require_history_unavailable("get_frame_plan" "${response_6}")

string(JSON refused_code ERROR_VARIABLE json_error GET "${response_7}" error data code)
string(JSON refused_feature ERROR_VARIABLE json_error_feature GET "${response_7}" error data reason feature)
if(json_error OR json_error_feature OR NOT refused_code STREQUAL "preview.unavailable@1" OR
   NOT refused_feature STREQUAL "history")
    message(FATAL_ERROR "render_preview was not refused as preview.unavailable@1 (history): ${response_7}")
endif()

# Normal play draws, and history makes the second frame differ.
foreach(capture "${capture_a}" "${capture_b}")
    if(NOT EXISTS "${capture}")
        message(FATAL_ERROR "capture missing: ${capture}")
    endif()
endforeach()
file(READ "${capture_a}" frame_a HEX)
file(READ "${capture_b}" frame_b HEX)
if(frame_a STREQUAL frame_b)
    message(FATAL_ERROR "two frames of the history accumulation captured the same bytes")
endif()

# The start says it once, as a warning.
file(READ "${log_path}" log)
string(REGEX MATCHALL "render preview unavailable \\(preview\\.unsupported_authoring@1 feature=history target=accum\\)" warnings "${log}")
list(LENGTH warnings warning_count)
if(NOT warning_count EQUAL 1)
    message(FATAL_ERROR "expected one preview-unavailable warning line in the log, found ${warning_count}:\n${log}")
endif()
string(REGEX MATCH "[^\n]*render preview unavailable[^\n]*" warning_line "${log}")
if(NOT warning_line MATCHES "WARN")
    message(FATAL_ERROR "the preview-unavailable line is not a warning: ${warning_line}")
endif()
message(STATUS "PV-1 start-up log line: ${warning_line}")
