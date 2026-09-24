# MG-1: `pelican_cli project migrate` as shipped. The production registry has
# no steps at v0, so on a fresh project and on every v0 corpus project the
# command reports nothing to do and writes nothing -- the dry run and the
# apply alike. The steps themselves are exercised in projectmigration_test.
foreach(required IN ITEMS CLI CORPUS_DIR OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

function(tree_digest dir output_var)
    file(GLOB_RECURSE files LIST_DIRECTORIES true RELATIVE "${dir}" "${dir}/*")
    list(SORT files)
    set(listing "")
    foreach(path IN LISTS files)
        if(IS_DIRECTORY "${dir}/${path}")
            string(APPEND listing "${path}/\n")
        else()
            file(SHA256 "${dir}/${path}" digest)
            string(APPEND listing "${path}=${digest}\n")
        endif()
    endforeach()
    set(${output_var} "${listing}" PARENT_SCOPE)
endfunction()

function(expect_nothing_to_migrate project_dir label)
    tree_digest("${project_dir}" before)
    foreach(mode IN ITEMS dry apply)
        if(mode STREQUAL "dry")
            set(extra --dry-run)
        else()
            set(extra "")
        endif()
        execute_process(
            COMMAND "${CLI}" project migrate --project "${project_dir}" ${extra}
            RESULT_VARIABLE result
            OUTPUT_VARIABLE stdout
            ERROR_VARIABLE stderr)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR
                "${label}: project migrate (${mode}) failed with ${result}\n"
                "stdout:\n${stdout}\nstderr:\n${stderr}")
        endif()
        string(JSON required_value ERROR_VARIABLE json_error GET "${stdout}" required)
        string(JSON document_count ERROR_VARIABLE count_error LENGTH "${stdout}" documents)
        if(NOT json_error STREQUAL "NOTFOUND" OR NOT count_error STREQUAL "NOTFOUND" OR
           NOT required_value STREQUAL "OFF" OR NOT document_count EQUAL 0)
            message(FATAL_ERROR
                "${label}: project migrate (${mode}) did not report an empty plan\n${stdout}")
        endif()
        if(mode STREQUAL "apply")
            string(JSON applied_value GET "${stdout}" applied)
            if(NOT applied_value STREQUAL "OFF")
                message(FATAL_ERROR "${label}: project migrate applied something\n${stdout}")
            endif()
        endif()
        tree_digest("${project_dir}" after)
        if(NOT before STREQUAL after)
            message(FATAL_ERROR
                "${label}: project migrate (${mode}) changed the project\n"
                "before:\n${before}\nafter:\n${after}")
        endif()
    endforeach()
endfunction()

# `project init` writes saved_by_engine; migrate then has nothing to do.
set(project_dir "${OUT_DIR}/new_project")
execute_process(
    COMMAND "${CLI}" project init "${project_dir}"
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "project init failed with ${init_result}\n${init_stdout}\n${init_stderr}")
endif()
file(READ "${project_dir}/project.json" project_json)
string(JSON saved_by_engine ERROR_VARIABLE saved_error GET "${project_json}" saved_by_engine)
if(NOT saved_error STREQUAL "NOTFOUND" OR NOT saved_by_engine MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "project init did not record saved_by_engine\n${project_json}")
endif()
expect_nothing_to_migrate("${project_dir}" "project init")

file(GLOB corpus_projects LIST_DIRECTORIES true "${CORPUS_DIR}/*")
set(corpus_count 0)
foreach(corpus_project IN LISTS corpus_projects)
    if(NOT EXISTS "${corpus_project}/project.json")
        continue()
    endif()
    get_filename_component(name "${corpus_project}" NAME)
    file(COPY "${corpus_project}" DESTINATION "${OUT_DIR}/corpus")
    expect_nothing_to_migrate("${OUT_DIR}/corpus/${name}" "corpus ${name}")
    math(EXPR corpus_count "${corpus_count} + 1")
endforeach()
if(NOT corpus_count EQUAL 7)
    message(FATAL_ERROR "expected 7 v0 corpus projects under ${CORPUS_DIR}, found ${corpus_count}")
endif()

# A named failure is named on stderr and exits non-zero.
execute_process(
    COMMAND "${CLI}" project migrate --project "${OUT_DIR}/missing"
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_stdout
    ERROR_VARIABLE missing_stderr)
if(missing_result EQUAL 0)
    message(FATAL_ERROR "project migrate accepted a missing project\n${missing_stdout}")
endif()
message(STATUS "project migrate: nothing to migrate in project init and ${corpus_count} corpus projects")
