# Run against a player built with PELICAN_PROJECT and its adjacent game DLL.
# The existing project-code smoke provides PROJECT_DIR and GAME_DLL.
foreach(required PLAYER PROJECT_DIR GAME_DLL OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${OUT_DIR}/bundle" "${OUT_DIR}/unrelated-cwd")
file(COPY "${PROJECT_DIR}/" DESTINATION "${OUT_DIR}/bundle")
get_filename_component(player_name "${PLAYER}" NAME)
get_filename_component(player_dir "${PLAYER}" DIRECTORY)
get_filename_component(dll_name "${GAME_DLL}" NAME)
configure_file("${PLAYER}" "${OUT_DIR}/bundle/${player_name}" COPYONLY)
configure_file("${GAME_DLL}" "${OUT_DIR}/bundle/${dll_name}" COPYONLY)
# Runtime DLLs may be needed by Debug or staged builds.
file(GLOB runtime_dlls "${player_dir}/*.dll")
foreach(dll IN LISTS runtime_dlls)
    file(COPY "${dll}" DESTINATION "${OUT_DIR}/bundle")
endforeach()
if(WIN32)
    set(ENV{PATH} "${OUT_DIR}/bundle;$ENV{PATH}")
else()
    set(ENV{PATH} "${OUT_DIR}/bundle:$ENV{PATH}")
endif()
set(ENV{PELICAN_NO_MESSAGEBOX} 1)
execute_process(COMMAND "${player_name}" --headless --frames 1 --size 160x90
    --log-file "${OUT_DIR}/player.log" --render-out "${OUT_DIR}/frame.png"
    WORKING_DIRECTORY "${OUT_DIR}/unrelated-cwd"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
file(WRITE "${OUT_DIR}/process.log" "${out}\n${err}")
if(NOT result EQUAL 0 OR NOT EXISTS "${OUT_DIR}/frame.png")
    message(FATAL_ERROR "PATH launch failed (${result}): ${out}\n${err}")
endif()
file(READ "${OUT_DIR}/player.log" log)
string(REPLACE "\\" "/" normalized_log "${log}")
string(TOLOWER "${normalized_log}" normalized_log)
string(TOLOWER "${OUT_DIR}/bundle" expected_root)
string(TOLOWER "${dll_name}" expected_dll)
string(FIND "${normalized_log}" "implicit project root = ${expected_root}" root_at)
string(FIND "${normalized_log}" "${expected_root}/${expected_dll}" dll_at)
if(root_at EQUAL -1 OR dll_at EQUAL -1 OR NOT log MATCHES "game logic DLL loaded:")
    message(FATAL_ERROR "PATH launch did not resolve implicit project and game DLL: ${log}")
endif()
# An impossible explicit GPU must be refused by the production argument path.
execute_process(COMMAND "${player_name}" --headless --frames 1 --size 160x90 --gpu 999999
    --log-file "${OUT_DIR}/invalid-gpu.log"
    WORKING_DIRECTORY "${OUT_DIR}/unrelated-cwd"
    RESULT_VARIABLE gpu_result OUTPUT_VARIABLE gpu_out ERROR_VARIABLE gpu_err TIMEOUT 60)
file(WRITE "${OUT_DIR}/invalid-gpu-process.log" "${gpu_out}\n${gpu_err}")
if(gpu_result EQUAL 0 OR NOT gpu_err MATCHES "999999" OR NOT gpu_err MATCHES "Devices:")
    message(FATAL_ERROR "Explicit --gpu was not refused with named devices: ${gpu_result}\n${gpu_err}")
endif()
