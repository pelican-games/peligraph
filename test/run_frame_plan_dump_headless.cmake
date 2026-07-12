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
file(MAKE_DIRECTORY
    "${OUT_DIR}/project/assets"
    "${OUT_DIR}/project/scenes"
    "${OUT_DIR}/project/passes"
    "${OUT_DIR}/project/ui"
)

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "frame_plan_dump_headless",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Frame Plan Dump Headless",
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
configure_file("${PROJECT_DIR}/passes/main_rendering_config.json" "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${OUT_DIR}/project"
        --frames 1
        --size 160x90
        --fps 30
        --dump-frame-plan
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "frame plan dump player run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "frame plan dump run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(stdout MATCHES "pelican.frame_plan")
    message(FATAL_ERROR "--dump-frame-plan wrote the frame plan to stdout\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(NOT stderr MATCHES "pelican.frame_plan")
    message(FATAL_ERROR "--dump-frame-plan stderr did not include the frame plan schema\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(NOT stderr MATCHES "\"version\"[ \r\n\t]*:[ \r\n\t]*1")
    message(FATAL_ERROR "--dump-frame-plan stderr did not include frame plan version 1\nstderr:\n${stderr}")
endif()
if(NOT stderr MATCHES "\"graph\"[ \r\n\t]*:[ \r\n\t]*\"main_render\"")
    message(FATAL_ERROR "--dump-frame-plan stderr did not include the main_render graph\nstderr:\n${stderr}")
endif()
