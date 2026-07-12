if(NOT DEFINED PLAYER OR NOT DEFINED PLAYER_DIR OR NOT DEFINED PROJECT_DIR OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "PLAYER, PLAYER_DIR, PROJECT_DIR, and OUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/project/assets" "${OUT_DIR}/project/scenes"
                    "${OUT_DIR}/project/passes" "${OUT_DIR}/project/ui")
file(WRITE "${OUT_DIR}/project/project.json" [=[
{"schema":"pelican.project","version":1,"name":"ui_u2_rpc","engine_min_version":"0.1.0",
 "basic_config":{"window_title":"UI U2 RPC","window_size":{"width":160,"height":90},
 "fullscreen":false,"framerate":30,"camera":{"yfov":0.7853981633974483,"znear":0.1,"zfar":1000.0,"up":[0.0,1.0,0.0]},
 "default_scene_id":"default_scene","scene_data_json":"scenes/main.scene.json",
 "asset_data_json":"assets/asset_data.json","rendering_config_json":"passes/main_rendering_config.json",
 "default_rendering_pass":"main_render","ui_config_json":"ui/ui_overlay.json"}}
]=])
file(WRITE "${OUT_DIR}/project/assets/asset_data.json" "{\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/scenes/main.scene.json" [=[
{"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
 {"name":"Camera","components":[{"name":"transform","pos":[0.0,0.0,-4.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},{"name":"camera","type":"perspective","yfov":0.78539816339,"znear":0.1,"zfar":1000.0}]}
]}}}
]=])
file(WRITE "${OUT_DIR}/project/ui/ui_overlay.json" [=[
{"schema":"pelican.ui","version":1,"key":"rpc_demo","revision":"wp93",
 "root":{"id":"root","type":"panel","children":[
  {"id":"click","type":"button","text":"CLICK","color":[44,62,82,255],"hover_color":[55,82,108,255],"pressed_color":[20,36,52,255],
   "layout":{"x":{"mode":"fixed","value":96},"y":{"mode":"fixed","value":32},"offsets":[8,8,0,0]},
   "emit":{"on_click":{"event":"UiDemoClick","fields":[{"name":"source","from":"stable_id"},{"name":"amount","from":"static","value":7}]}}},
  {"id":"drag","type":"button","text":"DRAG","color":[70,52,82,255],
   "layout":{"x":{"mode":"fixed","value":96},"y":{"mode":"fixed","value":28},"offsets":[8,50,0,0]},
   "emit":{"on_drag":{"event":"UiDemoDrag","fields":[{"name":"delta","from":"drag_delta_ui"}]}}}
 ]}}
]=])
configure_file("${PROJECT_DIR}/passes/main_rendering_config.json"
               "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

set(recording "${OUT_DIR}/ui_click_drag.input.jsonl")
set(pressed "${OUT_DIR}/rpc_click_pressed.png")
set(record_final "${OUT_DIR}/record_final.png")
set(record_script "${OUT_DIR}/record.ndjson")
file(WRITE "${record_script}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"start_input_record\",\"params\":{\"path\":\"${recording}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"mouse_move\",\"x\":20,\"y\":20},{\"type\":\"mouse_down\",\"button\":\"left\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"capture\",\"params\":{\"path\":\"${pressed}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"mouse_up\",\"button\":\"left\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"mouse_move\",\"x\":20,\"y\":60},{\"type\":\"mouse_down\",\"button\":\"left\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"mouse_move\",\"x\":30,\"y\":60},{\"type\":\"mouse_up\",\"button\":\"left\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"stop_input_record\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"capture\",\"params\":{\"path\":\"${record_final}\"}}\n")

execute_process(COMMAND "${PLAYER}" --rpc --headless --project "${OUT_DIR}/project" --size 160x90 --fps 30
    WORKING_DIRECTORY "${PLAYER_DIR}" INPUT_FILE "${record_script}"
    RESULT_VARIABLE record_result OUTPUT_VARIABLE record_stdout ERROR_VARIABLE record_stderr)
if(NOT record_result EQUAL 0 OR record_stdout MATCHES "Validation Error|VUID-" OR record_stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "UI U2 record run failed (${record_result})\n${record_stdout}\n${record_stderr}")
endif()
if(NOT record_stdout MATCHES [=["id":8.*"seed":9307]=] OR
   NOT record_stdout MATCHES [=["id":14.*"seed":9310]=])
    message(FATAL_ERROR "UI click/drag semantic delivery was not next-frame deterministic\n${record_stdout}")
endif()
if(NOT EXISTS "${recording}" OR NOT EXISTS "${pressed}" OR NOT EXISTS "${record_final}")
    message(FATAL_ERROR "UI U2 record artifacts are missing")
endif()

file(READ "${record_final}" record_hex HEX)
foreach(run RANGE 1 2)
    set(replay_capture "${OUT_DIR}/replay_${run}.png")
    set(replay_script "${OUT_DIR}/replay_${run}.ndjson")
    file(WRITE "${replay_script}"
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"start_input_replay\",\"params\":{\"path\":\"${recording}\"}}\n")
    foreach(id RANGE 2 7)
        file(APPEND "${replay_script}" "{\"jsonrpc\":\"2.0\",\"id\":${id},\"method\":\"step_frame\",\"params\":{}}\n")
    endforeach()
    file(APPEND "${replay_script}"
      "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"get_status\",\"params\":{}}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"capture\",\"params\":{\"path\":\"${replay_capture}\"}}\n")
    execute_process(COMMAND "${PLAYER}" --rpc --headless --project "${OUT_DIR}/project" --size 160x90 --fps 30
      WORKING_DIRECTORY "${PLAYER_DIR}" INPUT_FILE "${replay_script}"
      RESULT_VARIABLE replay_result OUTPUT_VARIABLE replay_stdout ERROR_VARIABLE replay_stderr)
    if(NOT replay_result EQUAL 0 OR NOT replay_stdout MATCHES [=["id":8.*"seed":9310]=])
        message(FATAL_ERROR "UI U2 replay ${run} failed (${replay_result})\n${replay_stdout}\n${replay_stderr}")
    endif()
    file(READ "${replay_capture}" replay_hex HEX)
    if(NOT replay_hex STREQUAL record_hex)
        message(FATAL_ERROR "UI U2 record/replay capture ${run} is not byte-identical")
    endif()
endforeach()
