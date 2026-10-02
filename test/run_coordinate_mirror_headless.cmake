# CC-1a: the 3D screen is the world as seen, not its mirror
# (docs/design_coordinate_convention.md section 5). For each camera variant
# coordinate_mirror_check writes a project, pelican_player renders it
# headless, and the check reads the frame: +X on the right, +Y at the top,
# an F that reads the right way, and a roll that turns the picture.
foreach(required PLAYER PLAYER_DIR MIRROR_CHECK OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

foreach(variant IN ITEMS front side roll)
    set(project_dir "${OUT_DIR}/${variant}")
    set(output_png "${OUT_DIR}/${variant}.png")
    execute_process(
        COMMAND "${MIRROR_CHECK}" write "${project_dir}" ${variant}
        RESULT_VARIABLE write_result
        OUTPUT_VARIABLE write_stdout
        ERROR_VARIABLE write_stderr
    )
    if(NOT write_result EQUAL 0)
        message(FATAL_ERROR
            "${variant}: writing the project failed: ${write_result}\n${write_stdout}\n${write_stderr}")
    endif()
    execute_process(
        COMMAND "${PLAYER}"
            --headless
            --project "${project_dir}"
            --frames 3
            --size 128x128
            --fps 30
            --render-out "${output_png}"
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE player_result
        OUTPUT_VARIABLE player_stdout
        ERROR_VARIABLE player_stderr
    )
    if(NOT player_result EQUAL 0 OR NOT EXISTS "${output_png}")
        message(FATAL_ERROR
            "${variant}: pelican_player failed: ${player_result}\n"
            "stdout:\n${player_stdout}\nstderr:\n${player_stderr}")
    endif()
    execute_process(
        COMMAND "${MIRROR_CHECK}" check "${output_png}" ${variant}
        RESULT_VARIABLE check_result
        OUTPUT_VARIABLE check_stdout
        ERROR_VARIABLE check_stderr
    )
    message(STATUS "${variant}:\n${check_stdout}${check_stderr}")
    if(NOT check_result EQUAL 0)
        message(FATAL_ERROR "${variant}: the frame is not the world as seen (${output_png})")
    endif()
endforeach()
