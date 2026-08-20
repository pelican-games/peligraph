include(view/ui_depended_setup.cmake)

add_subdirectory(viewport)

add_library(pelican_studio_layout STATIC
    layoutpreset.cpp
    toollayoutpreset.cpp
)
set_target_properties(pelican_studio_layout PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
)
target_include_directories(pelican_studio_layout PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_layout PUBLIC Qt6::Core)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PELICAN_INSTALL_DIR})
add_executable(pelican_studio main.cpp)
unset(CMAKE_RUNTIME_OUTPUT_DIRECTORY)

set_target_properties(pelican_studio PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    CXX_STANDARD 20
)

if(MSVC)
    include(${CMAKE_SOURCE_DIR}/cmake/pelican_crash_symbols.cmake)
    pelican_read_git_identity(
        SOURCE_DIR ${CMAKE_SOURCE_DIR}
        COMMIT_OUT pelican_studio_source_commit
        GIT_OUT pelican_studio_git_executable
    )
    pelican_preserve_crash_symbols(
        TARGET pelican_studio
        ARCHIVE_ROOT ${PELICAN_CRASH_SYMBOL_ARCHIVE_DIR}
        SOURCE_COMMIT ${pelican_studio_source_commit}
        SOURCE_DIR ${CMAKE_SOURCE_DIR}
        GIT_EXECUTABLE ${pelican_studio_git_executable}
        MAX_GENERATIONS ${PELICAN_CRASH_SYMBOL_GENERATIONS}
    )
    message(STATUS
        "Pelican Studio crash symbols: ${PELICAN_CRASH_SYMBOL_ARCHIVE_DIR} "
        "(keep ${PELICAN_CRASH_SYMBOL_GENERATIONS})")
endif()

# D0: Studio is a public-client application, never an engine-internal client.
target_link_libraries(pelican_studio PRIVATE
    argparse
    pelican_project
    pelican_studio_layout
    pelican_studio_viewport
)

# subdirs
add_subdirectory(model)
add_subdirectory(view)

include(${CMAKE_SOURCE_DIR}/cmake/devstudio_link_boundary.cmake)
pelican_collect_directory_targets(${CMAKE_SOURCE_DIR}/src/core pelican_engine_targets)
pelican_assert_link_boundary(
    TARGET pelican_studio
    REQUIRED_DIRECT_TARGETS pelican_project
    FORBIDDEN_TARGETS ${pelican_engine_targets}
)
