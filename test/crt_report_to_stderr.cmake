# Links test/crt_report_to_stderr.cpp into every executable target defined in
# the test directory (and any directory below it): Catch2 executables and
# their groups, probes, fixture writers, fixture child processes, the studio
# test executables and the GPU lease launcher. Call once, after the last
# add_executable in test/CMakeLists.txt. Product executables (pelican_player,
# pelican_studio, pelican_cli) live outside test/ and are never touched.
#
# The source itself only does anything under the Windows Debug CRT.

set(PELICAN_CRT_REPORT_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/crt_report_to_stderr.cpp")

function(pelican_collect_executable_targets directory out_var)
    set(found)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "EXECUTABLE")
            list(APPEND found ${target})
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        pelican_collect_executable_targets("${child}" child_found)
        list(APPEND found ${child_found})
    endforeach()
    set(${out_var} ${found} PARENT_SCOPE)
endfunction()

function(pelican_route_test_crt_reports_to_stderr)
    if(NOT WIN32)
        return()
    endif()
    pelican_collect_executable_targets("${CMAKE_CURRENT_SOURCE_DIR}" executables)
    foreach(target IN LISTS executables)
        target_sources(${target} PRIVATE "${PELICAN_CRT_REPORT_SOURCE}")
    endforeach()
    list(LENGTH executables executable_count)
    message(STATUS
        "Pelican test CRT reports: stderr instead of a dialog in "
        "${executable_count} test executables")
endfunction()
