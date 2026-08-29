include_guard(GLOBAL)

function(pelican_normalize_rpc_response_line response_line output_var)
    set(normalized "${response_line}")
    string(JSON heap_count ERROR_VARIABLE heap_error
        LENGTH "${normalized}" result memory heaps)
    if(heap_error STREQUAL "NOTFOUND" AND heap_count GREATER 0)
        math(EXPR heap_last "${heap_count} - 1")
        foreach(heap_index RANGE 0 ${heap_last})
            foreach(measured_field IN ITEMS usage budget)
                string(JSON measured_type ERROR_VARIABLE measured_error
                    TYPE "${normalized}" result memory heaps ${heap_index} ${measured_field})
                if(measured_error STREQUAL "NOTFOUND")
                    string(JSON normalized REMOVE "${normalized}"
                        result memory heaps ${heap_index} ${measured_field})
                endif()
            endforeach()
        endforeach()
    endif()
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()

function(pelican_normalize_rpc_stdout stdout output_var)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REGEX REPLACE "\n$" "" normalized "${normalized}")
    string(REGEX REPLACE [=["instance_id":"[0-9a-fA-F-]+"]=]
        [=["instance_id":"<uuid>"]=] normalized "${normalized}")
    string(REGEX REPLACE [=["startup":\{[^\}]*\}]=]
        [=["startup":"<measured>"]=] normalized "${normalized}")
    string(REGEX REPLACE [=["closure_generation":[0-9]+]=]
        [=["closure_generation":"<generation>"]=] normalized "${normalized}")
    string(REGEX REPLACE [=["provider_generation":[0-9]+]=]
        [=["provider_generation":"<generation>"]=] normalized "${normalized}")

    string(REPLACE "\n" ";" response_lines "${normalized}")
    set(normalized_lines)
    foreach(response_line IN LISTS response_lines)
        pelican_normalize_rpc_response_line("${response_line}" normalized_line)
        list(APPEND normalized_lines "${normalized_line}")
    endforeach()
    list(JOIN normalized_lines "\n" normalized)
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()
