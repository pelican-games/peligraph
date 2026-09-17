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

function(pelican_classify_game_logic_target target_name game_logic_class game_logic_role)
    set_property(TARGET ${target_name} PROPERTY PELICAN_GAME_DLL_CLASS
        "${game_logic_class}")
    set_property(TARGET ${target_name} PROPERTY PELICAN_GAME_DLL_ROLE
        "${game_logic_role}")
endfunction()

function(pelican_mark_game_logic_target target_name game_logic_class game_logic_role)
    set_property(TARGET ${target_name} PROPERTY PELICAN_GAME_DLL TRUE)
    pelican_classify_game_logic_target(
        ${target_name} "${game_logic_class}" "${game_logic_role}")
    set_property(GLOBAL APPEND PROPERTY PELICAN_GAME_DLL_TARGETS
        ${target_name})
endfunction()

function(pelican_configure_game_logic_target target_name)
    set(options LINK_ONLY NO_INVENTORY)
    set(one_value_args HOST_TARGET ROLE)
    cmake_parse_arguments(PGL "${options}" "${one_value_args}" "" ${ARGN})
    if(PGL_HOST_TARGET)
        set(game_logic_host_target "${PGL_HOST_TARGET}")
    else()
        set(game_logic_host_target pelican_player)
    endif()
    if(PGL_LINK_ONLY)
        set(game_logic_class "link-only")
    else()
        set(game_logic_class "loader")
    endif()
    if(PGL_ROLE)
        set(game_logic_role "${PGL_ROLE}")
    else()
        set(game_logic_role "current-sdk")
    endif()
    if(PGL_NO_INVENTORY)
        pelican_classify_game_logic_target(
            ${target_name} "${game_logic_class}" "${game_logic_role}")
    else()
        pelican_mark_game_logic_target(
            ${target_name} "${game_logic_class}" "${game_logic_role}")
    endif()
    set_property(TARGET ${target_name} PROPERTY PELICAN_GAME_DLL_HOST_TARGET
        "${game_logic_host_target}")

    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    target_sources(${target_name} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/gamelogic_entry.cpp")
    add_dependencies(${target_name} ${game_logic_host_target})
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/src/schema")
    target_link_libraries(${target_name} PRIVATE
        "$<TARGET_LINKER_FILE:${game_logic_host_target}>"
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
        PELICAN_WITH_ANIMATION=$<BOOL:${PELICAN_WITH_ANIMATION}>
        PELICAN_WITH_SEQPLAYER=$<BOOL:${PELICAN_WITH_SEQPLAYER}>
        PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
        PELICAN_WITH_PHYSICS=$<BOOL:${PELICAN_WITH_PHYSICS}>
    )
endfunction()

function(pelican_game_sources)
    if(NOT ARGN)
        message(FATAL_ERROR "pelican_game_sources requires at least one source file")
    endif()

    set(game_logic_target pelican_game_logic)
    if(DEFINED PELICAN_GAME_LOGIC_TARGET_OVERRIDE AND
       NOT "${PELICAN_GAME_LOGIC_TARGET_OVERRIDE}" STREQUAL "")
        set(game_logic_target "${PELICAN_GAME_LOGIC_TARGET_OVERRIDE}")
    endif()

    set(resolved_sources)
    foreach(source IN LISTS ARGN)
        if(IS_ABSOLUTE "${source}")
            list(APPEND resolved_sources "${source}")
        else()
            list(APPEND resolved_sources "${PELICAN_PROJECT_CODE_DIR}/${source}")
        endif()
    endforeach()

    if(NOT TARGET ${game_logic_target})
        add_library(${game_logic_target} SHARED)
        set_target_properties(${game_logic_target} PROPERTIES
            OUTPUT_NAME "${game_logic_target}"
        )
        # A game DLL imports the SDK surface exported by the running player.
        # This keeps the engine registries and module container single-owned.
        pelican_configure_game_logic_target(${game_logic_target})
        if(game_logic_target STREQUAL "pelican_game_logic")
            target_compile_definitions(pelican_player PRIVATE
                PELICAN_GAME_LOGIC_DLL_NAME="pelican_game_logic${CMAKE_SHARED_LIBRARY_SUFFIX}"
            )
        endif()
    endif()

    target_sources(${game_logic_target} PRIVATE ${resolved_sources})
    target_include_directories(${game_logic_target} PRIVATE
        "${PELICAN_PROJECT_ROOT}"
        "${PELICAN_PROJECT_CODE_DIR}"
    )
endfunction()

# X-1: a package compiles against the public SDK and nothing else.
#
# The game logic DLL's own include path carries "${CMAKE_SOURCE_DIR}/src/core",
# so project code can reach every engine-internal header.  A package is code
# that came from outside the tree, and "only the public SDK" has to be a judged
# property rather than a promise: this builds a directory of forwarding headers
# holding exactly what PELICAN_AUTO_STAGE_SDK publishes
# (src/core/userpublic/**, src/core/ecs/**, handle.hpp, phys/physquery.hpp) and
# gives the package target that directory instead of src/core.  Forwarding and
# not copying, so each real header still resolves its own relative includes and
# nothing can go stale.
function(pelican_package_sdk_include_dir out_var)
    set(sdk_dir "${CMAKE_BINARY_DIR}/pelican_package_sdk/include")
    get_property(prepared GLOBAL PROPERTY PELICAN_PACKAGE_SDK_PREPARED)
    if(prepared)
        set(${out_var} "${sdk_dir}" PARENT_SCOPE)
        return()
    endif()

    set(sdk_entries)
    file(GLOB_RECURSE userpublic_headers CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/core/userpublic/*.hpp")
    foreach(header IN LISTS userpublic_headers)
        file(RELATIVE_PATH relative
            "${CMAKE_SOURCE_DIR}/src/core/userpublic" "${header}")
        list(APPEND sdk_entries "${relative}|${header}")
    endforeach()
    file(GLOB_RECURSE ecs_headers CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/core/ecs/*.hpp")
    foreach(header IN LISTS ecs_headers)
        file(RELATIVE_PATH relative
            "${CMAKE_SOURCE_DIR}/src/core/ecs" "${header}")
        list(APPEND sdk_entries "ecs/${relative}|${header}")
    endforeach()
    list(APPEND sdk_entries "handle.hpp|${CMAKE_SOURCE_DIR}/src/core/handle.hpp")
    list(APPEND sdk_entries
        "phys/physquery.hpp|${CMAKE_SOURCE_DIR}/src/core/phys/physquery.hpp")

    foreach(entry IN LISTS sdk_entries)
        string(FIND "${entry}" "|" separator)
        string(SUBSTRING "${entry}" 0 ${separator} relative)
        math(EXPR header_begin "${separator} + 1")
        string(SUBSTRING "${entry}" ${header_begin} -1 PELICAN_PACKAGE_SDK_HEADER)
        configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package_sdk_header.hpp.in"
            "${sdk_dir}/${relative}" @ONLY)
    endforeach()
    set_property(GLOBAL PROPERTY PELICAN_PACKAGE_SDK_PREPARED TRUE)
    set(${out_var} "${sdk_dir}" PARENT_SCOPE)
endfunction()

# Declares the sources of one vendored package.  Valid only inside a package's
# own code/CMakeLists.txt, which pelican_packages() adds as a subdirectory.
# The objects join the project's game logic DLL; a package never gets a DLL of
# its own.
function(pelican_package_sources)
    if(NOT ARGN)
        message(FATAL_ERROR "pelican_package_sources requires at least one source file")
    endif()
    if(NOT DEFINED PELICAN_PACKAGE_TARGET OR "${PELICAN_PACKAGE_TARGET}" STREQUAL "")
        message(FATAL_ERROR
            "pelican_package_sources() belongs in a package's code/CMakeLists.txt; "
            "pelican_packages() is what adds that directory")
    endif()

    set(resolved_sources)
    foreach(source IN LISTS ARGN)
        if(IS_ABSOLUTE "${source}")
            list(APPEND resolved_sources "${source}")
        else()
            list(APPEND resolved_sources "${PELICAN_PACKAGE_CODE_DIR}/${source}")
        endif()
    endforeach()

    if(NOT TARGET ${PELICAN_PACKAGE_TARGET})
        pelican_package_sdk_include_dir(package_sdk_include)
        add_library(${PELICAN_PACKAGE_TARGET} OBJECT)
        set_target_properties(${PELICAN_PACKAGE_TARGET} PROPERTIES
            CXX_STANDARD 20
            POSITION_INDEPENDENT_CODE ON
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        )
        # No "${CMAKE_SOURCE_DIR}/src/core" here.  That omission is the whole
        # point: an engine-internal include from package code fails to compile.
        target_include_directories(${PELICAN_PACKAGE_TARGET} PRIVATE
            "${package_sdk_include}"
            "${CMAKE_SOURCE_DIR}/src/schema"
            "${PELICAN_PACKAGE_DIR}"
            "${PELICAN_PACKAGE_CODE_DIR}"
        )
        target_link_libraries(${PELICAN_PACKAGE_TARGET} PRIVATE
            glm
            nlohmann_json
        )
        target_compile_definitions(${PELICAN_PACKAGE_TARGET} PRIVATE
            PELICAN_GAME_DLL=1
            PELICAN_WITH_AUDIO=$<BOOL:${PELICAN_WITH_AUDIO}>
            PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
            PELICAN_WITH_EXR=$<BOOL:${PELICAN_WITH_EXR}>
            PELICAN_WITH_RPC=$<BOOL:${PELICAN_WITH_RPC}>
            PELICAN_WITH_ANIMATION=$<BOOL:${PELICAN_WITH_ANIMATION}>
            PELICAN_WITH_SEQPLAYER=$<BOOL:${PELICAN_WITH_SEQPLAYER}>
            PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
            PELICAN_WITH_PHYSICS=$<BOOL:${PELICAN_WITH_PHYSICS}>
        )
    endif()
    target_sources(${PELICAN_PACKAGE_TARGET} PRIVATE ${resolved_sources})
endfunction()

# Builds every package vendored under <project>/packages/ that ships code.
# Call it after pelican_game_sources(...): the objects join that DLL, and the
# player finds the DLL by a fixed name next to the executable.
function(pelican_packages)
    if(ARGN)
        message(FATAL_ERROR "pelican_packages() takes no arguments")
    endif()
    if(NOT DEFINED PELICAN_PROJECT_ROOT OR "${PELICAN_PROJECT_ROOT}" STREQUAL "")
        message(FATAL_ERROR
            "pelican_packages() belongs in a project's code/CMakeLists.txt, "
            "where PELICAN_PROJECT_ROOT is set")
    endif()

    file(GLOB package_manifests CONFIGURE_DEPENDS
        "${PELICAN_PROJECT_ROOT}/packages/*/package.json")
    list(SORT package_manifests)
    set(package_object_targets)
    foreach(package_manifest IN LISTS package_manifests)
        get_filename_component(package_dir "${package_manifest}" DIRECTORY)
        get_filename_component(package_name "${package_dir}" NAME)
        if(package_name MATCHES "\\.tmp-")
            # An interrupted install left staging behind; it is not installed.
            continue()
        endif()
        if(NOT EXISTS "${package_dir}/code/CMakeLists.txt")
            continue()
        endif()
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" package_stem "${package_name}")
        set(PELICAN_PACKAGE_NAMESPACE "${package_name}")
        set(PELICAN_PACKAGE_DIR "${package_dir}")
        set(PELICAN_PACKAGE_CODE_DIR "${package_dir}/code")
        set(PELICAN_PACKAGE_TARGET "pelican_package_${package_stem}")
        # Absolute source dir with an explicit binary dir: the project's
        # code/CMakeLists.txt is include()d from src/player, so a relative path
        # would be looked up under the engine tree.
        add_subdirectory("${package_dir}/code"
            "${CMAKE_CURRENT_BINARY_DIR}/pelican_packages/${package_stem}")
        if(TARGET "pelican_package_${package_stem}")
            list(APPEND package_object_targets "pelican_package_${package_stem}")
        endif()
    endforeach()

    if(NOT package_object_targets)
        return()
    endif()

    set(game_logic_target pelican_game_logic)
    if(DEFINED PELICAN_GAME_LOGIC_TARGET_OVERRIDE AND
       NOT "${PELICAN_GAME_LOGIC_TARGET_OVERRIDE}" STREQUAL "")
        set(game_logic_target "${PELICAN_GAME_LOGIC_TARGET_OVERRIDE}")
    endif()
    if(NOT TARGET ${game_logic_target})
        message(FATAL_ERROR
            "pelican_packages() has to come after pelican_game_sources(...): "
            "package objects join the project's game logic DLL, and it does "
            "not exist yet")
    endif()
    foreach(package_target IN LISTS package_object_targets)
        target_sources(${game_logic_target} PRIVATE
            "$<TARGET_OBJECTS:${package_target}>")
    endforeach()
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
