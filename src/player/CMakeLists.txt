add_executable(pelican_player main.cpp)
set_target_properties(pelican_player PROPERTIES
    CXX_STANDARD 20
    ENABLE_EXPORTS TRUE
)

# The hot-reload game DLL imports the public SDK from the player executable.
# Keep every pelican_core object in that export host even when the player does
# not call a particular public helper itself (for example FlipbookClip).
if(MSVC)
    target_link_options(pelican_player PRIVATE
        "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:pelican_core>"
    )
    target_link_libraries(pelican_player pelican_core argparse)
elseif(APPLE)
    target_link_libraries(pelican_player
        "-Wl,-force_load,$<TARGET_FILE:pelican_core>"
        argparse
    )
else()
    target_link_libraries(pelican_player
        "-Wl,--whole-archive"
        pelican_core
        "-Wl,--no-whole-archive"
        argparse
    )
endif()
target_include_directories(pelican_player PRIVATE ${Vulkan_INCLUDE_DIRS})

function(pelican_configure_game_logic_target target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    add_dependencies(${target_name} pelican_player)
    target_link_libraries(${target_name} PRIVATE
        "$<TARGET_LINKER_FILE:pelican_player>"
        glm
        nlohmann_json
    )
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/core/userpublic"
        "${CMAKE_SOURCE_DIR}/src/core"
        ${Vulkan_INCLUDE_DIRS}
    )
    target_compile_definitions(${target_name} PRIVATE
        PELICAN_GAME_DLL=1
        PELICAN_WITH_AUDIO=$<BOOL:${PELICAN_WITH_AUDIO}>
        PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
        PELICAN_WITH_EXR=$<BOOL:${PELICAN_WITH_EXR}>
        PELICAN_WITH_RPC=$<BOOL:${PELICAN_WITH_RPC}>
        PELICAN_WITH_SEQPLAYER=$<BOOL:${PELICAN_WITH_SEQPLAYER}>
        PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
    )
endfunction()

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

    if(NOT TARGET pelican_game_logic)
        add_library(pelican_game_logic SHARED gamelogic_entry.cpp)
        set_target_properties(pelican_game_logic PROPERTIES
            OUTPUT_NAME "pelican_game_logic"
        )
        # A game DLL imports the SDK surface exported by the running player.
        # This keeps the engine registries and module container single-owned.
        pelican_configure_game_logic_target(pelican_game_logic)
        target_compile_definitions(pelican_player PRIVATE
            PELICAN_GAME_LOGIC_DLL_NAME="pelican_game_logic${CMAKE_SHARED_LIBRARY_SUFFIX}"
        )
    endif()

    target_sources(pelican_game_logic PRIVATE ${resolved_sources})
    target_include_directories(pelican_game_logic PRIVATE
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
