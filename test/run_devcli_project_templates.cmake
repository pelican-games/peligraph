# F-2: the two templates `pelican_cli project init` writes, read as files.
#
# No GPU here: the frame each template draws is judged by
# run_devcli_project_init.cmake, which runs the player. What this script holds
# is the part a reader can check without one --
#   * the default is still the standard template, byte for byte what
#     `project init` wrote before F-2 (the digests below were taken from that
#     build; MG-2b re-took the two it changed, see the table), and
#     `--template standard` is the same thing spelled out;
#   * `--template blank` is a direct config -- no `pipeline`, no `features`,
#     no `render_targets` -- with one fullscreen pass `present` writing
#     `display` through the engine's `engine://passthrough` (VI-2), and ships
#     no shader of its own;
#   * an unknown template is refused by name, and nothing is written.

foreach(required IN ITEMS CLI OUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

function(project_init name)
    execute_process(
        COMMAND "${CLI}" project init "${OUT_DIR}/${name}" ${ARGN}
        RESULT_VARIABLE init_result
        OUTPUT_VARIABLE init_stdout
        ERROR_VARIABLE init_stderr
    )
    if(NOT init_result EQUAL 0)
        message(FATAL_ERROR
            "project init ${ARGN} failed with ${init_result}\n"
            "stdout:\n${init_stdout}\nstderr:\n${init_stderr}")
    endif()
endfunction()

# Every file the standard template writes, and its SHA-256 as `project init`
# wrote it before the templates moved into pelican_project.
#
# MG-2b (2026-09-24) re-took two rows: the engine reads pelican.scene and
# pelican.asset_data version 2 only, so the template writes `"version": 2` in
# scenes/main.scene.json and assets/asset_data.json -- the only byte that
# changed in either file. passes/main_rendering_config.json is now written as
# the pelican.graph root directly instead of wrapped at run time; its bytes
# (and so its row) did not change.
#
# MG-1 (2026-09-25) re-took the project.json row: `project init` records the
# engine that wrote the project as `"saved_by_engine": "0.1.0"`, one line after
# engine_min_version; nothing else in the file changed.
set(standard_digests
    ".gitattributes=53ca7dbd7bae3d4305c235b965e0af1b77aa1a6b0afa1ffcb8b8301e44c82955"
    ".gitignore=45658c9867458757f8857156830c81a4aabec1ac1b68dda97a9bed3c42c3f8a7"
    "README.md=88d9f3ec8d5d6c54b5e6f8ef9200d050c19764c5a4293759d45b7e8d86b9ae93"
    "assets/.gitkeep=01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b"
    "assets/asset_data.json=99284ff84b66d0665aee8d324fd2e5f64a708b025f06895c8766c5c57e999d38"
    "assets/audio/.gitkeep=01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b"
    "assets/models/.gitkeep=01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b"
    "assets/textures/.gitkeep=01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b"
    "code/CMakeLists.txt=db2a48b2cfd07cfc96f4ecd866d0656bc0af8634ea4a14e3331f72f19970c064"
    "code/game.cpp=950af38ba5d7348e40cce42181344387a0b1956f9388e1081983b8c81673a38e"
    "input/actions.json=0f02c5b9badd6adce192b8a65e197f09c4ed1f137f3852477365f4a8c1c41d74"
    "input/profiles/keyboard.json=86d35286615bce33adfb6bf9a9c1b54e02d30e9b85b35cd1513f14538657d97c"
    "passes/main_rendering_config.json=81163814279c3171d67a654ec510ed0698daa1636ca22327b0894013976e000b"
    "project.json=3dcd6a1c8a649988bdb70894d375b8478472f094da44fe20f90b598382fff2b7"
    "scenes/main.scene.json=c6ec9bab9c5fab7f326b9c556330dd7fb94db85d97780eddfe39c795c3808fcb"
    "ui/ui_overlay.json=f9cf52bb037434a9aa098e11a18fd12587bad51bf07928db10899b1da952a2ac"
)

# `dir` must hold exactly the files in `expected` (a list of path=digest),
# except those named in `skip`, which must exist but are judged elsewhere.
function(expect_files dir expected skip)
    file(GLOB_RECURSE actual LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    list(SORT actual)
    set(expected_paths "")
    foreach(entry IN LISTS expected)
        string(FIND "${entry}" "=" split_at)
        string(SUBSTRING "${entry}" 0 ${split_at} path)
        math(EXPR digest_at "${split_at} + 1")
        string(SUBSTRING "${entry}" ${digest_at} -1 digest)
        list(APPEND expected_paths "${path}")
        if(NOT EXISTS "${dir}/${path}")
            message(FATAL_ERROR "${dir} is missing ${path}")
        endif()
        list(FIND skip "${path}" skipped)
        if(skipped EQUAL -1)
            file(SHA256 "${dir}/${path}" actual_digest)
            if(NOT actual_digest STREQUAL digest)
                file(READ "${dir}/${path}" contents)
                message(FATAL_ERROR
                    "${dir}/${path} differs from the standard template "
                    "(sha256 ${actual_digest}, expected ${digest})\n${contents}")
            endif()
        endif()
    endforeach()
    list(SORT expected_paths)
    if(NOT actual STREQUAL expected_paths)
        message(FATAL_ERROR
            "${dir} holds a different set of files\n"
            "expected: ${expected_paths}\nactual:   ${actual}")
    endif()
endfunction()

# --- the default is the standard template, unchanged -----------------------
project_init(default_template)
expect_files("${OUT_DIR}/default_template" "${standard_digests}" "")
project_init(standard_template --template standard)
expect_files("${OUT_DIR}/standard_template" "${standard_digests}" "")

# --- blank ------------------------------------------------------------------
project_init(blank_template --template blank)
set(blank_dir "${OUT_DIR}/blank_template")
# Everything but the rendering config is the standard template's. VI-2: its
# pass draws with an engine shader, so it adds no file -- no shaders/ at all.
expect_files("${blank_dir}" "${standard_digests}"
    "passes/main_rendering_config.json")
if(EXISTS "${blank_dir}/shaders")
    message(FATAL_ERROR "blank template wrote a shaders folder its pass does not use")
endif()

file(READ "${blank_dir}/passes/main_rendering_config.json" blank_config)
string(JSON blank_schema ERROR_VARIABLE schema_error GET "${blank_config}" schema)
string(JSON blank_section_type ERROR_VARIABLE section_error
    TYPE "${blank_config}" rendering)
if(NOT schema_error STREQUAL "NOTFOUND" OR NOT blank_schema STREQUAL "pelican.graph" OR
   NOT section_error STREQUAL "NOTFOUND" OR NOT blank_section_type STREQUAL "OBJECT")
    message(FATAL_ERROR
        "blank rendering config is not a pelican.graph root with an object rendering section\n${blank_config}")
endif()
# A direct config: the absence of `pipeline` is what makes it one.
foreach(forbidden_member IN ITEMS pipeline features render_targets)
    string(JSON ignored ERROR_VARIABLE member_error
        TYPE "${blank_config}" rendering "${forbidden_member}")
    if(member_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "blank rendering config unexpectedly has ${forbidden_member}\n${blank_config}")
    endif()
endforeach()
string(JSON group_count LENGTH "${blank_config}" rendering rendering_passes)
string(JSON group_name GET "${blank_config}" rendering rendering_passes 0 name)
string(JSON pass_count LENGTH "${blank_config}" rendering rendering_passes 0 passes)
if(NOT group_count EQUAL 1 OR NOT group_name STREQUAL "main_render" OR NOT pass_count EQUAL 1)
    message(FATAL_ERROR
        "blank rendering config is not one group `main_render` with one pass\n${blank_config}")
endif()
string(JSON pass_name GET "${blank_config}" rendering rendering_passes 0 passes 0 name)
string(JSON pass_type GET "${blank_config}" rendering rendering_passes 0 passes 0 type)
string(JSON pass_input_count LENGTH "${blank_config}" rendering rendering_passes 0 passes 0 input)
string(JSON pass_output_count LENGTH "${blank_config}" rendering rendering_passes 0 passes 0 output color)
string(JSON pass_output GET "${blank_config}" rendering rendering_passes 0 passes 0 output color 0)
string(JSON pass_vertex GET "${blank_config}" rendering rendering_passes 0 passes 0 shader vertex)
string(JSON pass_fragment GET "${blank_config}" rendering rendering_passes 0 passes 0 shader fragment)
if(NOT pass_name STREQUAL "present" OR NOT pass_type STREQUAL "fullscreen" OR
   NOT pass_input_count EQUAL 0 OR NOT pass_output_count EQUAL 1 OR
   NOT pass_output STREQUAL "display" OR
   NOT pass_vertex STREQUAL "engine://fullscreen" OR
   NOT pass_fragment STREQUAL "engine://passthrough")
    message(FATAL_ERROR
        "blank rendering config's pass is not `present`, fullscreen, reading nothing and writing display "
        "from engine://passthrough\n${blank_config}")
endif()
# No defines: engine://passthrough shows input 0, the default, once present
# is given an input.
string(JSON pass_defines_type ERROR_VARIABLE defines_error
    TYPE "${blank_config}" rendering rendering_passes 0 passes 0 shader defines)
if(defines_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "blank rendering config's present sets shader defines\n${blank_config}")
endif()

# --- an unknown template ----------------------------------------------------
execute_process(
    COMMAND "${CLI}" project init "${OUT_DIR}/unknown_template" --template deferred
    RESULT_VARIABLE unknown_result
    OUTPUT_VARIABLE unknown_stdout
    ERROR_VARIABLE unknown_stderr
)
if(unknown_result EQUAL 0)
    message(FATAL_ERROR "project init accepted an unknown template")
endif()
string(CONCAT unknown_output "${unknown_stdout}" "${unknown_stderr}")
foreach(needle IN ITEMS deferred standard blank)
    string(FIND "${unknown_output}" "${needle}" needle_at)
    if(needle_at EQUAL -1)
        message(FATAL_ERROR
            "unknown-template error does not mention `${needle}`\n${unknown_output}")
    endif()
endforeach()
if(EXISTS "${OUT_DIR}/unknown_template")
    message(FATAL_ERROR "project init wrote a directory for an unknown template")
endif()
