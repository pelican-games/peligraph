include("${CMAKE_CURRENT_LIST_DIR}/run_rpc_inject_input_headless.cmake")

set(recording_path "${OUT_DIR}/wasd.input_seq.jsonl")
set(record_capture "${OUT_DIR}/record_capture.png")
set(record_script "${OUT_DIR}/record.ndjson")
file(WRITE "${record_script}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"start_input_record\",\"params\":{\"path\":\"${recording_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_down\",\"key\":\"W\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_up\",\"key\":\"W\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"stop_input_record\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"capture\",\"params\":{\"path\":\"${record_capture}\"}}\n"
)

execute_process(
    COMMAND "${PLAYER}" --rpc --headless --project "${OUT_DIR}/project" --size 160x90 --fps 30
    WORKING_DIRECTORY "${PLAYER_DIR}"
    INPUT_FILE "${record_script}"
    RESULT_VARIABLE record_result
    OUTPUT_VARIABLE record_stdout
    ERROR_VARIABLE record_stderr
)
if(NOT record_result EQUAL 0)
    message(FATAL_ERROR "input recording failed: ${record_result}\nstdout:\n${record_stdout}\nstderr:\n${record_stderr}")
endif()
if(NOT EXISTS "${recording_path}" OR NOT EXISTS "${record_capture}")
    message(FATAL_ERROR "input recording did not produce input_seq and capture")
endif()
file(READ "${recording_path}" recording)
if(NOT recording MATCHES [=["schema":"pelican.input_seq"]=] OR
   NOT recording MATCHES [=["version":1]=] OR
   NOT recording MATCHES [=["fps":30.0]=])
    message(FATAL_ERROR "input_seq header is missing schema/version/fps:\n${recording}")
endif()
string(REGEX MATCHALL [=[\{"frame":[0-9]+\}]=] markers "${recording}")
list(LENGTH markers marker_count)
if(NOT marker_count EQUAL 3)
    message(FATAL_ERROR "input_seq expected 3 frame boundary markers, got ${marker_count}:\n${recording}")
endif()
if(NOT record_stdout MATCHES [=["id":7.*"events":2.*"frames":3]=])
    message(FATAL_ERROR "stop_input_record response did not report 3 frames/2 events:\n${record_stdout}")
endif()

function(run_replay label capture_path capture_hex_var)
    set(script "${OUT_DIR}/${label}.ndjson")
    file(WRITE "${script}"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"start_input_replay\",\"params\":{\"path\":\"${recording_path}\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_down\",\"key\":\"A\"}]}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"get_status\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"step_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"step_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"step_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"capture\",\"params\":{\"path\":\"${capture_path}\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"stop_input_replay\",\"params\":{}}\n"
    )
    execute_process(
        COMMAND "${PLAYER}" --rpc --headless --project "${OUT_DIR}/project" --size 160x90 --fps 30
        WORKING_DIRECTORY "${PLAYER_DIR}"
        INPUT_FILE "${script}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label} failed: ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(NOT stdout MATCHES [=["id":1.*"frames":3.*"hot_reload":false]=])
        message(FATAL_ERROR "${label}: start_input_replay did not disable hot reload:\n${stdout}")
    endif()
    if(NOT stdout MATCHES [=["id":2.*"code":-32000.*inject_input cannot be combined with start_input_replay]=])
        message(FATAL_ERROR "${label}: replay/inject_input conflict lacked both method names:\n${stdout}")
    endif()
    if(NOT stdout MATCHES [=["hot_reload":false.*"replaying":true]=])
        message(FATAL_ERROR "${label}: get_status did not expose deterministic replay mode:\n${stdout}")
    endif()
    if(NOT EXISTS "${capture_path}")
        message(FATAL_ERROR "${label}: capture missing")
    endif()
    file(READ "${capture_path}" capture_hex HEX)
    set(${capture_hex_var} "${capture_hex}" PARENT_SCOPE)
endfunction()

run_replay("replay_first" "${OUT_DIR}/replay_first.png" replay_first_hex)
run_replay("replay_second" "${OUT_DIR}/replay_second.png" replay_second_hex)
file(READ "${record_capture}" record_capture_hex HEX)

if(NOT replay_first_hex STREQUAL replay_second_hex)
    message(FATAL_ERROR "same input recording produced different rpc capture bytes across two replays")
endif()
if(NOT record_capture_hex STREQUAL replay_first_hex)
    message(FATAL_ERROR "recording run and replay rpc capture bytes differ")
endif()
