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

execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${PROJECT_DIR}"
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
