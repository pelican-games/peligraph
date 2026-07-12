if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED WRITER)
    message(FATAL_ERROR "WRITER is required")
endif()
if(NOT DEFINED EXAMPLE_PASS)
    message(FATAL_ERROR "EXAMPLE_PASS is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY
    "${OUT_DIR}/project/assets"
    "${OUT_DIR}/project/scenes"
    "${OUT_DIR}/project/passes"
    "${OUT_DIR}/project/ui"
    "${OUT_DIR}/frames"
)

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "vatplayer_headless",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "VAT Player Headless",
    "window_size": {"width": 160, "height": 90},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000.0, "up": [0.0, 1.0, 0.0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui_overlay.json"
  }
}
]=])

file(WRITE "${OUT_DIR}/project/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
}
]=])
file(WRITE "${OUT_DIR}/project/assets/asset_data.json" "{\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/ui_overlay.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
configure_file("${EXAMPLE_PASS}" "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

execute_process(
    COMMAND "${WRITER}" "${OUT_DIR}/project/assets/tiny_vat.glb"
    RESULT_VARIABLE writer_result
    OUTPUT_VARIABLE writer_stdout
    ERROR_VARIABLE writer_stderr
)
if(NOT writer_result EQUAL 0)
    message(FATAL_ERROR "VAT fixture writer failed with ${writer_result}\nstdout:\n${writer_stdout}\nstderr:\n${writer_stderr}")
endif()

execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${OUT_DIR}/project"
        --frames 3
        --size 160x90
        --fps 1
        --play-vat "assets/tiny_vat.glb"
        --camera "0,0,2,0,0,0,45"
        --render-out "${OUT_DIR}/frames/%04d.png"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "vatplayer headless run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

foreach(frame 0001 0002 0003)
    set(path "${OUT_DIR}/frames/${frame}.png")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "vatplayer headless output missing: ${path}")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "vatplayer headless output is empty: ${path}")
    endif()
endforeach()

if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "vatplayer headless run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
