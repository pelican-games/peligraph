# GP-3: game code and the common graph write the UI, seen in headless
# captures of the shipped player. Each project below is a separate player run.
#
#   facade  ctx.ui() changes a label's text, then hides it, then fills a gauge;
#           each capture differs from the one before, and two frames with no
#           write in between are byte-identical (the control).
#   sink    value.curve(time) -> ui.value fills a gauge; two times, two images.
#           A facade write to the sink's (path, property) is ui.sink_conflict@1,
#           the same node's other property is accepted.
#   nofeat  the render config does not reference the ui feature: the facade
#           answers ui.feature_unreferenced@1.
#   prep_*  render configs refused while they are prepared, by name.
foreach(required PLAYER PLAYER_DIR PROJECT_DIR OUT_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(READ "${PROJECT_DIR}/passes/main_rendering_config.json" base_config)
set(ui_feature "\"engine://features/ui.json\"")
string(FIND "${base_config}" "${ui_feature}" ui_feature_at)
if(ui_feature_at LESS 0)
  message(FATAL_ERROR "the example render config no longer references ${ui_feature}")
endif()
string(REPLACE ", ${ui_feature}" "" no_ui_config "${base_config}")
string(FIND "${no_ui_config}" "${ui_feature}" still_there)
if(NOT still_there LESS 0)
  message(FATAL_ERROR "could not drop the ui feature from the render config")
endif()

set(curve_nodes [=[[
  {"id":"clock","op":"value.input","parameters":{"name":"time"},"inputs":{},"outputs":["value"]},
  {"id":"fill","op":"value.curve","parameters":{"keys":[{"time":0,"value":0},{"time":1,"value":1}]},
   "inputs":{"time":{"node":"clock","port":"value"}},"outputs":["value"]},
  {"id":"meter_sink","op":"ui.value","parameters":{"node":"root/hud/meter"},
   "inputs":{"value":{"node":"fill","port":"value"}},"outputs":[]}
]]=])

# project(<name> <render config text>)
function(write_project name config)
  set(dir "${OUT_DIR}/${name}")
  file(MAKE_DIRECTORY "${dir}/assets" "${dir}/scenes" "${dir}/passes" "${dir}/ui")
  file(WRITE "${dir}/project.json" [=[
{"schema":"pelican.project","version":1,"name":"gp3_ui","engine_min_version":"0.1.0",
 "basic_config":{"window_title":"GP-3 UI","window_size":{"width":160,"height":90},
 "fullscreen":false,"framerate":30,"camera":{"yfov":0.7853981633974483,"znear":0.1,"zfar":1000.0},
 "default_scene_id":"default_scene","scene_data_json":"scenes/main.scene.json",
 "asset_data_json":"assets/asset_data.json","rendering_config_json":"passes/main_rendering_config.json",
 "default_rendering_pass":"main_render","ui_config_json":"ui/ui_overlay.json"}}
]=])
  file(WRITE "${dir}/assets/asset_data.json" "{\"schema\":\"pelican.asset_data\",\"version\":2,\"models\":[]}\n")
  file(WRITE "${dir}/scenes/main.scene.json" [=[
{"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
 {"name":"Camera","components":[{"name":"transform","pos":[0.0,0.0,-4.0],"rotation":[0.0,1.0,0.0,0.0],"scale":[1.0,1.0,1.0]},{"name":"camera","type":"perspective","yfov":0.78539816339,"znear":0.1,"zfar":1000.0}]}
]}}}
]=])
  file(WRITE "${dir}/ui/ui_overlay.json" [=[
{"schema":"pelican.ui","version":1,"key":"gp3_hud","revision":"gp3",
 "root":{"id":"root","type":"panel","children":[
  {"id":"hud","type":"panel","color":[20,24,30,255],
   "layout":{"x":{"mode":"fixed","value":150},"y":{"mode":"fixed","value":80},"offsets":[5,5,0,0]},
   "children":[
    {"id":"label","type":"label","text":"HI",
     "layout":{"x":{"mode":"content"},"y":{"mode":"content"},"offsets":[4,4,0,0]}},
    {"id":"gauge","type":"gauge","value":0.1,
     "layout":{"x":{"mode":"fixed","value":100},"y":{"mode":"fixed","value":8},"offsets":[4,24,0,0]}},
    {"id":"meter","type":"gauge","color":[200,90,60,255],
     "layout":{"x":{"mode":"fixed","value":100},"y":{"mode":"fixed","value":8},"offsets":[4,40,0,0]}}
   ]}
 ]}}
]=])
  file(WRITE "${dir}/passes/main_rendering_config.json" "${config}")
endfunction()

function(run_player name script out_var result_var)
  execute_process(COMMAND "${PLAYER}" --rpc --headless --project "${OUT_DIR}/${name}" --size 160x90 --fps 30
    WORKING_DIRECTORY "${PLAYER_DIR}" INPUT_FILE "${script}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  set(${out_var} "${stdout}\n${stderr}" PARENT_SCOPE)
  set(${result_var} "${result}" PARENT_SCOPE)
endfunction()

set(rpc_id 0)
# Each helper appends one request to ${script} and advances ${rpc_id} in the
# caller, so a status request's id can be matched in the output.
function(rpc text)
  math(EXPR next "${rpc_id} + 1")
  file(APPEND "${script}" "{\"jsonrpc\":\"2.0\",\"id\":${next},${text}}\n")
  set(rpc_id ${next} PARENT_SCOPE)
endfunction()
function(step_frame)
  rpc("\"method\":\"step_frame\",\"params\":{}")
  set(rpc_id ${rpc_id} PARENT_SCOPE)
endfunction()
function(capture path)
  rpc("\"method\":\"capture\",\"params\":{\"path\":\"${path}\"}")
  set(rpc_id ${rpc_id} PARENT_SCOPE)
endfunction()
# inject(<step> <var>) queues the event, steps the frame that delivers it (the
# write lands at that frame's boundary), reads the seed the probe set, and
# stores that status request's id in <var>.
function(inject step status_id_var)
  rpc("\"method\":\"inject_event\",\"params\":{\"type\":\"Gp3UiStep\",\"payload\":{\"step\":${step}}}")
  step_frame()
  rpc("\"method\":\"get_status\",\"params\":{}")
  set(${status_id_var} ${rpc_id} PARENT_SCOPE)
  set(rpc_id ${rpc_id} PARENT_SCOPE)
endfunction()

function(require_seed stdout id seed what)
  if(NOT stdout MATCHES "\"id\":${id},[^\n]*\"seed\":${seed}[,}]")
    message(FATAL_ERROR "${what}: expected seed ${seed} in get_status ${id}\n${stdout}")
  endif()
endfunction()
function(require_images_differ left right what)
  file(READ "${left}" left_hex HEX)
  file(READ "${right}" right_hex HEX)
  if(left_hex STREQUAL "" OR right_hex STREQUAL "")
    message(FATAL_ERROR "${what}: a capture is missing")
  endif()
  if(left_hex STREQUAL right_hex)
    message(FATAL_ERROR "${what}: the captures are identical")
  endif()
endfunction()
function(require_images_equal left right what)
  file(READ "${left}" left_hex HEX)
  file(READ "${right}" right_hex HEX)
  if(left_hex STREQUAL "" OR NOT left_hex STREQUAL right_hex)
    message(FATAL_ERROR "${what}: the captures differ")
  endif()
endfunction()
function(require_clean_run name result stdout)
  if(NOT result EQUAL 0 OR stdout MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "${name} run failed (${result})\n${stdout}")
  endif()
endfunction()

# --- facade ---------------------------------------------------------------
write_project(facade "${base_config}")
set(dir "${OUT_DIR}/facade")
set(script "${dir}/script.ndjson")
file(WRITE "${script}" "")
set(rpc_id 0)
step_frame()
capture("${dir}/still_a.png")
step_frame()
capture("${dir}/still_b.png")
inject(1 text_status)
capture("${dir}/text.png")
inject(2 hide_status)
capture("${dir}/hidden.png")
inject(3 gauge_status)
capture("${dir}/gauge.png")
inject(6 unknown_status)
inject(7 mismatch_status)
inject(8 keyed_status)
inject(9 other_key_status)
run_player(facade "${script}" facade_out facade_result)
require_clean_run(facade "${facade_result}" "${facade_out}")
require_seed("${facade_out}" ${text_status} 7010 "setText accepted")
require_seed("${facade_out}" ${hide_status} 7020 "setVisible accepted")
require_seed("${facade_out}" ${gauge_status} 7030 "setValue accepted")
require_seed("${facade_out}" ${unknown_status} 7062 "unknown path is ui.node_unknown@1")
require_seed("${facade_out}" ${mismatch_status} 7073 "setText on a gauge is ui.property_mismatch@1")
require_seed("${facade_out}" ${keyed_status} 7080 "the document's own key is accepted")
require_seed("${facade_out}" ${other_key_status} 7092 "a key the project lacks is ui.node_unknown@1")
require_images_equal("${dir}/still_a.png" "${dir}/still_b.png" "control: frames without a write")
require_images_differ("${dir}/still_b.png" "${dir}/text.png" "setText changed the label's pixels")
require_images_differ("${dir}/text.png" "${dir}/hidden.png" "setVisible(false) changed the pixels")
require_images_differ("${dir}/hidden.png" "${dir}/gauge.png" "setValue changed the gauge's pixels")

# --- sink -----------------------------------------------------------------
string(JSON sink_config SET "${base_config}" nodes "${curve_nodes}")
write_project(sink "${sink_config}")
set(dir "${OUT_DIR}/sink")
set(script "${dir}/script.ndjson")
file(WRITE "${script}" "")
set(rpc_id 0)
step_frame()
step_frame()
capture("${dir}/early.png")
foreach(frame RANGE 1 12)
  step_frame()
endforeach()
capture("${dir}/late.png")
inject(4 conflict_status)
inject(5 other_property_status)
run_player(sink "${script}" sink_out sink_result)
require_clean_run(sink "${sink_result}" "${sink_out}")
require_images_differ("${dir}/early.png" "${dir}/late.png" "value.curve -> ui.value moved the gauge")
require_seed("${sink_out}" ${conflict_status} 7044 "a facade write to the sink's unit is ui.sink_conflict@1")
require_seed("${sink_out}" ${other_property_status} 7050 "the sink's node, another property, is accepted")

# --- nofeat ---------------------------------------------------------------
write_project(nofeat "${no_ui_config}")
set(dir "${OUT_DIR}/nofeat")
set(script "${dir}/script.ndjson")
file(WRITE "${script}" "")
set(rpc_id 0)
step_frame()
inject(1 nofeat_status)
run_player(nofeat "${script}" nofeat_out nofeat_result)
require_clean_run(nofeat "${nofeat_result}" "${nofeat_out}")
require_seed("${nofeat_out}" ${nofeat_status} 7011 "no ui feature is ui.feature_unreferenced@1")

# --- refused while the render config is prepared ---------------------------
function(require_prepare_refused name config code)
  write_project(${name} "${config}")
  set(script "${OUT_DIR}/${name}/script.ndjson")
  file(WRITE "${script}" "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"step_frame\",\"params\":{}}\n")
  run_player(${name} "${script}" out result)
  if(result EQUAL 0 OR NOT out MATCHES "${code}")
    message(FATAL_ERROR "${name}: expected the player to refuse the render config with ${code} (${result})\n${out}")
  endif()
endfunction()
string(JSON prep_nofeat_config SET "${no_ui_config}" nodes "${curve_nodes}")
require_prepare_refused(prep_nofeat "${prep_nofeat_config}" "ui\\.feature_unreferenced@1")
string(JSON second_sink GET "${curve_nodes}" 2)
string(JSON second_sink SET "${second_sink}" id "\"meter_sink_2\"")
string(JSON two_sinks SET "${curve_nodes}" 3 "${second_sink}")
string(JSON prep_conflict_config SET "${base_config}" nodes "${two_sinks}")
require_prepare_refused(prep_conflict "${prep_conflict_config}" "ui\\.sink_conflict@1")
string(JSON missing_nodes SET "${curve_nodes}" 2 parameters node "\"root/hud/missing\"")
string(JSON prep_unknown_config SET "${base_config}" nodes "${missing_nodes}")
require_prepare_refused(prep_unknown "${prep_unknown_config}" "ui\\.node_unknown@1")
string(JSON label_nodes SET "${curve_nodes}" 2 parameters node "\"root/hud/label\"")
string(JSON prep_mismatch_config SET "${base_config}" nodes "${label_nodes}")
require_prepare_refused(prep_mismatch "${prep_mismatch_config}" "ui\\.property_mismatch@1")
