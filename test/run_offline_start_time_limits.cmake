# FX-6: refusals and acceptance at the one-hour offline timeline boundary.
# The invalid camera is parsed AFTER start-time. Its named error proves that
# the time was accepted while ensuring that no engine / GPU is initialized.
file(MAKE_DIRECTORY "${OUT_DIR}/limits_project")
file(WRITE "${OUT_DIR}/limits_project/project.json" "{}\n")

function(check_start_time_limit time fps accepted)
    execute_process(
        COMMAND "${PLAYER}" --headless --project "${OUT_DIR}/limits_project"
            --fps "${fps}" --start-time "${time}" --frames 1
            --camera 0,0,0,0,0,0,-1
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 10)
    if(result EQUAL 0 OR NOT result MATCHES "^-?[0-9]+$")
        message(FATAL_ERROR "start-time ${time} at ${fps} fps: exit ${result}\n${stdout}\n${stderr}")
    endif()
    if(accepted)
        if(stderr MATCHES "offline\\.time_invalid@1" OR
           NOT stderr MATCHES "--camera fov_deg must be positive")
            message(FATAL_ERROR "start-time ${time} at ${fps} fps was not accepted before camera validation:\n${stderr}")
        endif()
    elseif(NOT stderr MATCHES "offline\\.time_invalid@1")
        message(FATAL_ERROR "start-time ${time} at ${fps} fps was not refused by name:\n${stderr}")
    endif()
endfunction()

# Enormous values must be refused before llround / any warm-up.
check_start_time_limit(1e17 60 FALSE)
check_start_time_limit(1e300 60 FALSE)
check_start_time_limit(1e300 1e300 FALSE) # multiplication overflows double
# Previously finite / overflowing rounded engine times are both beyond one hour.
check_start_time_limit(1.5e308 1e-307 FALSE)
check_start_time_limit(1.79e308 1e-307 FALSE) # rounds to frame 18, time overflows
# Within the seconds limit, independently reject overflow and llround's endpoint.
check_start_time_limit(90 1e308 FALSE) # multiplication overflows double
check_start_time_limit(1 9.2233720368547748e18 TRUE) # last double below 2^63
check_start_time_limit(1 9.2233720368547758e18 FALSE) # 2^63 cannot reach llround
check_start_time_limit(3600 1e-307 TRUE) # finite step; nearest frame is zero
check_start_time_limit(-1 60 FALSE)
check_start_time_limit(later 60 FALSE)
check_start_time_limit(nan 60 FALSE)
check_start_time_limit(inf 60 FALSE)
check_start_time_limit(0 60 TRUE)
check_start_time_limit(90 60 TRUE)
check_start_time_limit(3599.999 60 TRUE)
check_start_time_limit(3600 60 TRUE)
check_start_time_limit(3600.001 60 FALSE) # rounds to the same frame as 3600
# The seconds limit stays the same at custom frame rates.
check_start_time_limit(90 30 TRUE)
check_start_time_limit(3600 30 TRUE)
check_start_time_limit(3600.001 30 FALSE)
check_start_time_limit(90 120 TRUE)
check_start_time_limit(3600 120 TRUE)
check_start_time_limit(3600.001 120 FALSE)
message(STATUS "FX-6 offline start-time limits: PASS (no GPU)")
