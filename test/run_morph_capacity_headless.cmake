# MV-1: a morph model loaded after a large static model starts and deforms.
# For each weight (on, off) morph_capacity_check writes a project whose
# 300,000-vertex static model comes before a morph quad, pelican_player
# renders it headless, and the check reads the frame: the morph moves the
# quad's top edge with the weight on and leaves it with the weight off.
foreach(required PLAYER PLAYER_DIR MORPH_CHECK OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

foreach(variant IN ITEMS on off)
    set(project_dir "${OUT_DIR}/${variant}")
    set(output_png "${OUT_DIR}/${variant}.png")
    execute_process(
        COMMAND "${MORPH_CHECK}" write "${project_dir}" ${variant}
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
        # The log goes to stdout headless; the fatal line is the reason.
        string(REGEX MATCHALL "[^\n]*Pelican fatal error[^\n]*" fatal_lines
            "${player_stdout}\n${player_stderr}")
        message(FATAL_ERROR
            "${variant}: pelican_player failed: ${player_result}\n"
            "${fatal_lines}\n"
            "stderr:\n${player_stderr}")
    endif()
    execute_process(
        COMMAND "${MORPH_CHECK}" check "${output_png}" ${variant}
        RESULT_VARIABLE check_result
        OUTPUT_VARIABLE check_stdout
        ERROR_VARIABLE check_stderr
    )
    message(STATUS "${variant}:\n${check_stdout}${check_stderr}")
    if(NOT check_result EQUAL 0)
        message(FATAL_ERROR "${variant}: the frame does not show the morph as weighted (${output_png})")
    endif()
endforeach()
