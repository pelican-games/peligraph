include("${CMAKE_CURRENT_LIST_DIR}/../cmake/pelican_project_requirements.cmake")

function(pelican_add_catalog_game_logic project_dir target_stem output_var)
    set(${output_var} "" PARENT_SCOPE)
    pelican_unavailable_project_requirement("${project_dir}/project.json"
        unavailable_requirement unavailable_option)
    if(unavailable_requirement)
        message(STATUS "skip catalog game DLL ${target_stem}: requires ${unavailable_requirement} (${unavailable_option}=OFF)")
        return()
    endif()
    set(PELICAN_PROJECT_ROOT "${project_dir}")
    set(PELICAN_PROJECT_CODE_DIR "${project_dir}/code")
    set(PELICAN_GAME_LOGIC_TARGET_OVERRIDE
        "pelican_catalog_${target_stem}_game_logic")
    include("${PELICAN_PROJECT_CODE_DIR}/CMakeLists.txt")
    set(${output_var} "${PELICAN_GAME_LOGIC_TARGET_OVERRIDE}" PARENT_SCOPE)
endfunction()
