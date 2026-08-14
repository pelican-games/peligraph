foreach(required IN ITEMS PLAYER PROJECT_DIR STUDIO_ARGUMENTS_PROBE
                          EXPECT_DEFAULT_ENABLED OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

function(response_for_id stdout id output_var)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REPLACE ";" "\\;" normalized "${normalized}")
    string(REPLACE "\n" ";" lines "${normalized}")
    set(found "")
    foreach(line IN LISTS lines)
        if(line MATCHES "\\\"id\\\":${id}([,}])")
            set(found "${line}")
        endif()
    endforeach()
    if(found STREQUAL "")
        message(FATAL_ERROR "missing JSON-RPC id ${id}:\n${stdout}")
    endif()
    string(JSON result_type ERROR_VARIABLE json_error TYPE "${found}" result)
    if(json_error OR NOT result_type)
        message(FATAL_ERROR
            "JSON-RPC id ${id} did not return a result:\n${found}")
    endif()
    set(${output_var} "${found}" PARENT_SCOPE)
endfunction()

function(studio_arguments output_var)
    execute_process(
        COMMAND "${STUDIO_ARGUMENTS_PROBE}" "${PROJECT_DIR}" ${ARGN}
        RESULT_VARIABLE arguments_result
        OUTPUT_VARIABLE arguments_stdout
        ERROR_VARIABLE arguments_stderr
    )
    if(NOT arguments_result EQUAL 0)
        message(FATAL_ERROR
            "studioPlayerArguments probe failed with ${arguments_result}\n"
            "stdout:\n${arguments_stdout}\nstderr:\n${arguments_stderr}")
    endif()
    string(REPLACE "\r\n" "\n" arguments_stdout "${arguments_stdout}")
    string(REPLACE "\r" "\n" arguments_stdout "${arguments_stdout}")
    string(REGEX REPLACE "\n$" "" arguments_stdout "${arguments_stdout}")
    string(REPLACE ";" "\\;" arguments_stdout "${arguments_stdout}")
    string(REPLACE "\n" ";" arguments "${arguments_stdout}")
    set(${output_var} "${arguments}" PARENT_SCOPE)
endfunction()

function(run_status prefix)
    studio_arguments(arguments
        --headless --frames 0 --size 64x64 --fps 30 ${ARGN})
    execute_process(
        COMMAND "${PLAYER}" ${arguments}
        INPUT_FILE "${rpc_script}"
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE run_stdout
        ERROR_VARIABLE run_stderr
        TIMEOUT 180
    )
    set(${prefix}_result "${run_result}" PARENT_SCOPE)
    set(${prefix}_stdout "${run_stdout}" PARENT_SCOPE)
    set(${prefix}_stderr "${run_stderr}" PARENT_SCOPE)
    if(run_result EQUAL 0)
        response_for_id("${run_stdout}" 1 response)
        set(${prefix}_response "${response}" PARENT_SCOPE)
    endif()
endfunction()

function(require_no_validation_errors label stdout stderr)
    if(stdout MATCHES "Validation Error|VUID-" OR
       stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR
            "${label} emitted Vulkan validation errors\n"
            "stdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
endfunction()

function(read_validation_status response prefix)
    foreach(field IN ITEMS layer available enabled synchronization reason)
        string(JSON value ERROR_VARIABLE field_error
            GET "${response}" result vulkan_validation ${field})
        if(field_error)
            message(FATAL_ERROR
                "running player did not report vulkan_validation.${field}:\n"
                "${response}")
        endif()
        set(${prefix}_${field} "${value}" PARENT_SCOPE)
    endforeach()
endfunction()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
set(rpc_script "${OUT_DIR}/get_status.ndjson")
file(WRITE "${rpc_script}"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n")

# Both launches pass through the exact argument assembly used by Studio, then
# assert the state resolved by the running Vulkan core. Merely finding the CLI
# string in Studio's argv would not prove that the layer reached VkInstance.
run_status(default)
if(NOT default_result EQUAL 0)
    set(default_output "${default_stdout}\n${default_stderr}")
    if(default_output MATCHES "No suitable Vulkan physical device found")
        message(STATUS
            "PELICAN_VULKAN_VALIDATION_RUNTIME_SKIP_NO_DEVICE: "
            "${default_output}")
        return()
    endif()
    message(FATAL_ERROR
        "default validation launch failed with ${default_result}\n"
        "stdout:\n${default_stdout}\nstderr:\n${default_stderr}")
endif()
require_no_validation_errors(
    "default validation launch" "${default_stdout}" "${default_stderr}")
read_validation_status("${default_response}" default)

if(NOT default_layer STREQUAL "VK_LAYER_KHRONOS_validation" OR
   NOT default_available)
    message(FATAL_ERROR
        "default launch did not report the installed Khronos layer:\n"
        "${default_response}")
endif()
if(EXPECT_DEFAULT_ENABLED)
    if(NOT default_enabled OR NOT default_synchronization OR
       NOT default_reason STREQUAL "enabled_by_debug_build_default")
        message(FATAL_ERROR
            "_DEBUG default did not enable validation and synchronization:\n"
            "${default_response}")
    endif()
    set(contrast_mode off)
    set(expected_contrast_reason disabled_by_launch_option)
else()
    if(default_enabled OR default_synchronization OR
       NOT default_reason STREQUAL "disabled_by_non_debug_build_default")
        message(FATAL_ERROR
            "non-_DEBUG default unexpectedly enabled validation:\n"
            "${default_response}")
    endif()
    set(contrast_mode on)
    set(expected_contrast_reason enabled_by_launch_option)
endif()

run_status(contrast "--vulkan-validation=${contrast_mode}")
if(NOT contrast_result EQUAL 0)
    message(FATAL_ERROR
        "explicit validation ${contrast_mode} launch failed with "
        "${contrast_result}\nstdout:\n${contrast_stdout}\n"
        "stderr:\n${contrast_stderr}")
endif()
require_no_validation_errors(
    "explicit validation ${contrast_mode} launch"
    "${contrast_stdout}" "${contrast_stderr}")
read_validation_status("${contrast_response}" contrast)

if(NOT contrast_available OR
   NOT contrast_reason STREQUAL expected_contrast_reason)
    message(FATAL_ERROR
        "explicit validation launch reported the wrong resolution:\n"
        "${contrast_response}")
endif()
if(EXPECT_DEFAULT_ENABLED)
    if(contrast_enabled OR contrast_synchronization)
        message(FATAL_ERROR
            "explicit off did not disable the running validation layer:\n"
            "${contrast_response}")
    endif()
else()
    if(NOT contrast_enabled OR NOT contrast_synchronization)
        message(FATAL_ERROR
            "explicit on did not enable the running validation layer:\n"
            "${contrast_response}")
    endif()
endif()
if((default_enabled AND contrast_enabled) OR
   (NOT default_enabled AND NOT contrast_enabled))
    message(FATAL_ERROR
        "default and explicit contrast reported the same running state:\n"
        "default=${default_response}\ncontrast=${contrast_response}")
endif()

# The loader filter makes the installed validation layer disappear from
# enumeration without changing the machine. In that same environment, an
# explicit off launch must run and report unavailable/disabled, while an
# explicit on launch must fail before a Vulkan instance is accepted.
studio_arguments(missing_off_arguments
    --headless --frames 0 --size 64x64 --fps 30
    --vulkan-validation=off)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation"
        "${PLAYER}" ${missing_off_arguments}
    INPUT_FILE "${rpc_script}"
    RESULT_VARIABLE missing_off_result
    OUTPUT_VARIABLE missing_off_stdout
    ERROR_VARIABLE missing_off_stderr
    TIMEOUT 180
)
if(NOT missing_off_result EQUAL 0)
    message(FATAL_ERROR
        "no-request launch failed while validation layer was hidden with "
        "${missing_off_result}\nstdout:\n${missing_off_stdout}\n"
        "stderr:\n${missing_off_stderr}")
endif()
response_for_id("${missing_off_stdout}" 1 missing_off_response)
read_validation_status("${missing_off_response}" missing_off)
if(missing_off_available OR missing_off_enabled OR
   missing_off_synchronization OR
   NOT missing_off_reason STREQUAL "disabled_by_launch_option")
    message(FATAL_ERROR
        "hidden-layer no-request control reported the wrong running state:\n"
        "${missing_off_response}")
endif()

studio_arguments(missing_on_arguments
    --headless --frames 0 --size 64x64 --fps 30
    --vulkan-validation=on)
set(missing_on_workdir "${OUT_DIR}/missing_on")
file(MAKE_DIRECTORY "${missing_on_workdir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation"
        "${PLAYER}" ${missing_on_arguments}
    INPUT_FILE "${rpc_script}"
    RESULT_VARIABLE missing_on_result
    OUTPUT_VARIABLE missing_on_stdout
    ERROR_VARIABLE missing_on_stderr
    WORKING_DIRECTORY "${missing_on_workdir}"
    TIMEOUT 180
)
if(missing_on_result EQUAL 0)
    message(FATAL_ERROR
        "explicit validation unexpectedly succeeded while the layer was "
        "hidden\nstdout:\n${missing_on_stdout}\n"
        "stderr:\n${missing_on_stderr}")
endif()
set(missing_on_log "")
if(EXISTS "${missing_on_workdir}/pelican.log")
    file(READ "${missing_on_workdir}/pelican.log" missing_on_log)
endif()
set(missing_on_output
    "${missing_on_stdout}\n${missing_on_stderr}\n${missing_on_log}")
foreach(expected IN ITEMS
        "pelican.vulkan.validation_layer_unavailable@1"
        "--vulkan-validation=on"
        "VK_LAYER_KHRONOS_validation")
    string(FIND "${missing_on_output}" "${expected}" expected_at)
    if(expected_at EQUAL -1)
        message(FATAL_ERROR
            "hidden-layer hard error did not name '${expected}':\n"
            "${missing_on_output}")
    endif()
endforeach()

message(STATUS
    "Vulkan validation runtime contrast passed: default enabled="
    "${default_enabled}, explicit ${contrast_mode} enabled="
    "${contrast_enabled}; hidden layer rejects explicit on")
