foreach(required IN ITEMS PLAYER PLAYER_DIR PROJECT_DIR OUT_DIR CONFIG)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT CONFIG STREQUAL "Debug")
    message(STATUS
        "WSI fault injection is Debug-only; skipping ${CONFIG} integration run")
    return()
endif()

file(MAKE_DIRECTORY "${OUT_DIR}/user")

set(fault_script
    "acquire:surface_lost,"
    "surface_create:out_of_host_memory,"
    "surface_support_query:surface_lost,"
    "swapchain_create:surface_lost,"
    "dependent_resources:out_of_device_memory,"
    "present:surface_lost")
string(JOIN "" fault_script ${fault_script})

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "PELICAN_TEST_WSI_FAULT_SCRIPT=${fault_script}"
        "${PLAYER}"
        --project "${PROJECT_DIR}"
        --user-dir "${OUT_DIR}/user"
        --xr off
        --frames 180
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT 150
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "WSI fault player failed with ${result}\n"
        "stdout:\n${stdout}\n"
        "stderr:\n${stderr}")
endif()
if(NOT stderr STREQUAL "")
    message(FATAL_ERROR
        "WSI fault player wrote stderr:\n${stderr}")
endif()

foreach(call IN ITEMS
        acquire
        surface_create
        surface_support_query
        swapchain_create
        dependent_resources
        present)
    string(REGEX MATCHALL
        "WSI fault injected: call=${call} "
        matches "${stdout}")
    list(LENGTH matches match_count)
    if(NOT match_count EQUAL 1)
        message(FATAL_ERROR
            "expected exactly one ${call} injection, got ${match_count}\n"
            "stdout:\n${stdout}")
    endif()
endforeach()

string(REGEX MATCHALL
    "fresh Vulkan window surface created"
    surface_creations "${stdout}")
list(LENGTH surface_creations surface_creation_count)
if(NOT surface_creation_count EQUAL 5)
    message(FATAL_ERROR
        "expected five real surface creations, got ${surface_creation_count}\n"
        "stdout:\n${stdout}")
endif()

string(REGEX MATCHALL
    "swapchain epoch preparation failed:"
    preparation_failures "${stdout}")
list(LENGTH preparation_failures preparation_failure_count)
if(NOT preparation_failure_count EQUAL 4)
    message(FATAL_ERROR
        "expected four contained preparation failures, got "
        "${preparation_failure_count}\nstdout:\n${stdout}")
endif()

string(REGEX MATCHALL
    "swapchain epoch published:[^\n]*surface_epoch=[0-9]+"
    publications "${stdout}")
list(LENGTH publications publication_count)
if(NOT publication_count EQUAL 2)
    message(FATAL_ERROR
        "expected exactly two fresh-surface publications, got "
        "${publication_count}\n"
        "stdout:\n${stdout}")
endif()

if(NOT stdout MATCHES
   "WSI fault injection summary: injected=6 remaining=0")
    message(FATAL_ERROR
        "fault script was not completely consumed\nstdout:\n${stdout}")
endif()

foreach(forbidden IN ITEMS
        "LOG_ERROR"
        "Validation Error"
        "VUID-"
        "ErrorNativeWindowInUseKHR"
        "abort()")
    string(FIND "${stdout}" "${forbidden}" forbidden_position)
    if(NOT forbidden_position EQUAL -1)
        message(FATAL_ERROR
            "WSI fault player emitted '${forbidden}'\nstdout:\n${stdout}")
    endif()
endforeach()

message(STATUS
    "WSI fault window integration passed: injections=6 "
    "surface_creations=${surface_creation_count} "
    "publications=${publication_count}")
