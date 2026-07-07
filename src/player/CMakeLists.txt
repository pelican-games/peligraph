add_executable(pelican_player main.cpp)
set_target_properties(pelican_player PROPERTIES CXX_STANDARD 20)

target_link_libraries(pelican_player pelican_core argparse)
target_include_directories(pelican_player PRIVATE ${Vulkan_INCLUDE_DIRS})

function(pelican_game_sources)
    if(NOT ARGN)
        message(FATAL_ERROR "pelican_game_sources requires at least one source file")
    endif()

    set(resolved_sources)
    foreach(source IN LISTS ARGN)
        if(IS_ABSOLUTE "${source}")
            list(APPEND resolved_sources "${source}")
        else()
            list(APPEND resolved_sources "${PELICAN_PROJECT_CODE_DIR}/${source}")
        endif()
    endforeach()

    target_sources(pelican_player PRIVATE ${resolved_sources})
    target_include_directories(pelican_player PRIVATE
        "${PELICAN_PROJECT_ROOT}"
        "${PELICAN_PROJECT_CODE_DIR}"
    )
endfunction()

if(PELICAN_PROJECT)
    if(IS_ABSOLUTE "${PELICAN_PROJECT}")
        set(PELICAN_PROJECT_ROOT "${PELICAN_PROJECT}")
    else()
        set(PELICAN_PROJECT_ROOT "${CMAKE_SOURCE_DIR}/${PELICAN_PROJECT}")
    endif()
    get_filename_component(PELICAN_PROJECT_ROOT "${PELICAN_PROJECT_ROOT}" ABSOLUTE)
    set(PELICAN_PROJECT_CODE_DIR "${PELICAN_PROJECT_ROOT}/code")
    set(PELICAN_PROJECT_CODE_CMAKE "${PELICAN_PROJECT_CODE_DIR}/CMakeLists.txt")

    if(NOT EXISTS "${PELICAN_PROJECT_CODE_CMAKE}")
        message(FATAL_ERROR "PELICAN_PROJECT was specified, but code/CMakeLists.txt was not found: ${PELICAN_PROJECT_CODE_CMAKE}")
    endif()

    message(STATUS "Including Pelican project code from ${PELICAN_PROJECT_CODE_DIR}")
    include("${PELICAN_PROJECT_CODE_CMAKE}")
endif()
