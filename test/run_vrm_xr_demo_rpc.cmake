if(NOT DEFINED PLAYER OR NOT DEFINED PROJECT_DIR OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "PLAYER, PROJECT_DIR, and OUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

function(write_rpc_script path prefix response_count_var)
    file(WRITE "${path}" "")
    set(id 1)
    macro(append_request method params)
        file(APPEND "${path}"
            "{\"jsonrpc\":\"2.0\",\"id\":${id},\"method\":\"${method}\",\"params\":${params}}\n")
        math(EXPR id "${id} + 1")
    endmacro()
    macro(append_steps count)
        foreach(unused RANGE 1 ${count})
            append_request("step_frame" "{}")
        endforeach()
    endmacro()

    append_steps(2)
    append_request("capture" "{\"path\":\"${prefix}_idle.png\"}")
    append_request("inject_input" "{\"events\":[{\"type\":\"key_down\",\"key\":\"W\"}]}")
    append_steps(8)
    append_request("capture" "{\"path\":\"${prefix}_locomotion.png\"}")
    append_request("inject_input" "{\"events\":[{\"type\":\"key_down\",\"key\":\"Space\"}]}")
    append_steps(8)
    append_request("capture" "{\"path\":\"${prefix}_jump.png\"}")
    append_request("inject_input" "{\"events\":[{\"type\":\"key_up\",\"key\":\"Space\"},{\"type\":\"key_down\",\"key\":\"E\"}]}")
    append_steps(2)
    append_request("capture" "{\"path\":\"${prefix}_expression.png\"}")
    append_request("inject_input" "{\"events\":[{\"type\":\"key_up\",\"key\":\"E\"},{\"type\":\"key_down\",\"key\":\"right\"}]}")
    append_steps(2)
    append_request("capture" "{\"path\":\"${prefix}_look.png\"}")
    append_request("get_status" "{}")
    append_request("inject_input" "{\"events\":[{\"type\":\"key_up\",\"key\":\"W\"},{\"type\":\"key_up\",\"key\":\"right\"}]}")
    append_steps(1)
    math(EXPR response_count "${id} - 1")
    set(${response_count_var} ${response_count} PARENT_SCOPE)
endfunction()

function(run_demo label result_prefix)
    set(run_dir "${OUT_DIR}/${label}")
    file(MAKE_DIRECTORY "${run_dir}")
    set(script "${run_dir}/rpc.ndjson")
    set(prefix "${run_dir}/capture")
    write_rpc_script("${script}" "${prefix}" expected_lines)

    execute_process(
        COMMAND "${PLAYER}" --rpc --headless --project "${PROJECT_DIR}"
                --size 320x180 --fps 60
        WORKING_DIRECTORY "${run_dir}"
        INPUT_FILE "${script}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label}: WP135 RPC player failed ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR "${label}: Vulkan validation error\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()

    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REGEX REPLACE "\n$" "" normalized "${normalized}")
    string(REPLACE "\n" ";" lines "${normalized}")
    list(LENGTH lines line_count)
    if(NOT line_count EQUAL expected_lines)
        message(FATAL_ERROR "${label}: expected ${expected_lines} RPC responses, got ${line_count}\n${stdout}")
    endif()
    foreach(line IN LISTS lines)
        if(NOT line MATCHES [=["jsonrpc":"2\.0"]=] OR
           line MATCHES [=[,"error":\{"code":]=])
            message(FATAL_ERROR "${label}: unexpected RPC response: ${line}")
        endif()
    endforeach()

    foreach(stage IN ITEMS idle locomotion jump expression look)
        set(image "${prefix}_${stage}.png")
        if(NOT EXISTS "${image}")
            message(FATAL_ERROR "${label}: missing ${stage} capture")
        endif()
        file(SIZE "${image}" image_size)
        if(image_size EQUAL 0)
            message(FATAL_ERROR "${label}: empty ${stage} capture")
        endif()
        file(READ "${image}" image_hex HEX)
        set(${result_prefix}_${stage} "${image_hex}" PARENT_SCOPE)
    endforeach()

    set(log_path "${run_dir}/pelican.log")
    if(NOT EXISTS "${log_path}")
        message(FATAL_ERROR "${label}: pelican.log was not written")
    endif()
    file(READ "${log_path}" log)
    foreach(marker IN ITEMS
            "WP135 VRM XR demo ready"
            "state=Jump"
            "expression=angry"
            "speed=1.000"
            "yaw=30.000"
            "gaze=flat_camera")
        if(NOT log MATCHES "${marker}")
            message(FATAL_ERROR "${label}: log is missing '${marker}'\n${log}")
        endif()
    endforeach()
endfunction()

run_demo("first" first)
run_demo("second" second)

foreach(stage IN ITEMS idle locomotion jump expression look)
    if(NOT first_${stage} STREQUAL second_${stage})
        message(FATAL_ERROR "WP135 ${stage} capture is not deterministic")
    endif()
endforeach()

if(first_idle STREQUAL first_locomotion)
    message(FATAL_ERROR "W input did not change the locomotion frame")
endif()
if(first_locomotion STREQUAL first_jump)
    message(FATAL_ERROR "Jump input did not change the animation frame")
endif()
if(first_jump STREQUAL first_expression)
    message(FATAL_ERROR "expression cycle did not change the rendered frame")
endif()
if(first_expression STREQUAL first_look)
    message(FATAL_ERROR "lookAt angle did not change the rendered frame")
endif()

message(STATUS "WP135 deterministic RPC demo fixture passed")
