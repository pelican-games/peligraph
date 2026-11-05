# IL-1: pelican_cli intro-link bake writes one C header, byte-identical across
# runs, and refuses names that cannot be C identifiers or that repeat.
foreach(required IN ITEMS CLI GLSLC FIXTURE OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

function(copy_fixture out_var name)
    set(root "${OUT_DIR}/${name}")
    file(COPY "${FIXTURE}/" DESTINATION "${root}")
    set(${out_var} "${root}" PARENT_SCOPE)
endfunction()

function(bake label project out expect_success)
    execute_process(
        COMMAND "${CLI}" intro-link bake "${project}" --out "${out}" --glslc "${GLSLC}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(expect_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "${label}: bake failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(NOT expect_success AND result EQUAL 0)
        message(FATAL_ERROR "${label}: bake unexpectedly succeeded\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    set(BAKE_STDERR "${stderr}" PARENT_SCOPE)
    set(BAKE_STDOUT "${stdout}" PARENT_SCOPE)
endfunction()

function(expect_contains text needle label)
    string(FIND "${text}" "${needle}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR "${label}: expected to find\n${needle}\n\nin:\n${text}")
    endif()
endfunction()

# 1. Two runs, one byte sequence (the second run from a different directory
#    name, so no absolute path may reach the header).
copy_fixture(FIRST_ROOT first)
copy_fixture(SECOND_ROOT second_copy)
bake(first "${FIRST_ROOT}" "${OUT_DIR}/first.h" TRUE)
bake(second "${SECOND_ROOT}" "${OUT_DIR}/second.h" TRUE)
# IL-3: bake says what goes into the intro, one line per track and shader.
# The fixture's one-call helper makes -Os the smaller build.
foreach(needle IN ITEMS "track fade_linear: 3 keys, 60 bytes" "shader main_frag: " " bytes (Os)" "total: 180 bytes of keys + ")
    expect_contains("${BAKE_STDOUT}" "${needle}" "bake stdout")
endforeach()
file(SHA256 "${OUT_DIR}/first.h" first_hash)
file(SHA256 "${OUT_DIR}/second.h" second_hash)
if(NOT first_hash STREQUAL second_hash)
    message(FATAL_ERROR "bake is not deterministic: ${first_hash} vs ${second_hash}")
endif()
file(READ "${OUT_DIR}/first.h" header)
foreach(needle IN ITEMS
        "#define PIL_BAKED_DATA_INCLUDED 1"
        "#define PIL_BAKED_TRACK_COUNT 3"
        "PIL_TRACK_fade_linear = 0,"
        "PIL_TRACK_flash_step = 1,"
        "PIL_TRACK_wobble_cubic = 2,"
        "/* fade_linear: passes/main.json */"
        "{1.0f, 1.0f, 0.0f, 0.0f, 0u},"
        "{0.5f, 1.0f, 0.0f, 0.0f, 1u},"
        "{1.5f, 1.0f, 0.0f, -1.0f, 2u},"
        "#define PIL_BAKED_SHADER_COUNT 1"
        "PIL_SHADER_main_frag = 0,"
        "static const uint32_t pil_baked_shader_main_frag[] = {\n    0x07230203u,")
    expect_contains("${header}" "${needle}" "baked header")
endforeach()
foreach(absolute IN ITEMS "${OUT_DIR}" "${FIXTURE}")
    string(FIND "${header}" "${absolute}" absolute_at)
    if(NOT absolute_at EQUAL -1)
        message(FATAL_ERROR "baked header contains the absolute path ${absolute}")
    endif()
endforeach()

# 2. The same track id in two graph documents.
copy_fixture(DUP_ROOT duplicate)
file(WRITE "${DUP_ROOT}/passes/extra.json" [=[
{
  "schema": "pelican.graph",
  "version": 1,
  "nodes": [
    {"id": "fade_linear", "op": "value.curve",
     "parameters": {"keys": [{"time": 0.0, "value": 1.0}]}, "outputs": ["value"]}
  ]
}
]=])
bake(duplicate "${DUP_ROOT}" "${OUT_DIR}/duplicate.h" FALSE)
expect_contains("${BAKE_STDERR}" "intro_link.name_duplicate@1: track \"fade_linear\"" "duplicate")
expect_contains("${BAKE_STDERR}" "passes/extra.json" "duplicate")
expect_contains("${BAKE_STDERR}" "passes/main.json" "duplicate")
if(EXISTS "${OUT_DIR}/duplicate.h")
    message(FATAL_ERROR "a refused bake still wrote its header")
endif()

# 3. A node id that is not a C identifier.
copy_fixture(NAME_ROOT not_identifier)
file(WRITE "${NAME_ROOT}/passes/extra.json" [=[
{"schema": "pelican.graph", "version": 1, "nodes": [
  {"id": "fade-out", "op": "value.curve", "parameters": {"keys": [{"time": 0.0, "value": 1.0}]}}]}
]=])
bake(not_identifier "${NAME_ROOT}" "${OUT_DIR}/not_identifier.h" FALSE)
expect_contains("${BAKE_STDERR}" "intro_link.name_not_identifier@1: track \"fade-out\" in passes/extra.json"
                "not_identifier")

# 4. Key times that go backwards.
copy_fixture(ORDER_ROOT unordered)
file(WRITE "${ORDER_ROOT}/passes/extra.json" [=[
{"schema": "pelican.graph", "version": 1, "nodes": [
  {"id": "late", "op": "value.curve",
   "parameters": {"keys": [{"time": 1.0, "value": 1.0}, {"time": 0.5, "value": 0.0}]}}]}
]=])
bake(unordered "${ORDER_ROOT}" "${OUT_DIR}/unordered.h" FALSE)
expect_contains("${BAKE_STDERR}" "intro_link.curve_invalid@1: curve \"late\" in passes/extra.json" "unordered")

# 5. CB-1: a handle is validated on every key even though the intro link
#    has no Bezier representation.
copy_fixture(HANDLE_ROOT handle)
file(WRITE "${HANDLE_ROOT}/passes/extra.json" [=[
{"schema": "pelican.graph", "version": 1, "nodes": [
  {"id": "handled", "op": "value.curve", "parameters": {"keys": [
    {"time": 0.0, "value": 1.0, "out_handle": [-0.5, 0.0]},
    {"time": 1.0, "value": 2.0}]}}]}
]=])
bake(invalid_handle "${HANDLE_ROOT}" "${OUT_DIR}/invalid_handle.h" FALSE)
expect_contains("${BAKE_STDERR}"
                "intro_link.curve_invalid@1: curve \"handled\" in passes/extra.json: out_handle time delta has the wrong sign"
                "invalid_handle")
file(WRITE "${HANDLE_ROOT}/passes/extra.json" [=[
{"schema": "pelican.graph", "version": 1, "nodes": [
  {"id": "handled", "op": "value.curve", "parameters": {"keys": [
    {"time": 0.0, "value": 1.0, "in_handle": [0.5, 0.0]},
    {"time": 1.0, "value": 2.0}]}}]}
]=])
bake(invalid_in_handle "${HANDLE_ROOT}" "${OUT_DIR}/invalid_in_handle.h" FALSE)
expect_contains("${BAKE_STDERR}"
                "intro_link.curve_invalid@1: curve \"handled\" in passes/extra.json: in_handle time delta has the wrong sign"
                "invalid_in_handle")

# 6. CB-1: a Bezier interpolation is refused, not silently baked as linear.
copy_fixture(BEZIER_ROOT bezier)
file(WRITE "${BEZIER_ROOT}/passes/extra.json" [=[
{"schema": "pelican.graph", "version": 1, "nodes": [
  {"id": "curvy", "op": "value.curve", "parameters": {"keys": [
    {"time": 0.0, "value": 1.0, "interpolation": "bezier"},
    {"time": 1.0, "value": 2.0}]}}]}
]=])
bake(bezier "${BEZIER_ROOT}" "${OUT_DIR}/bezier.h" FALSE)
expect_contains("${BAKE_STDERR}"
                "intro_link.curve_invalid@1: curve \"curvy\" in passes/extra.json: interpolation \"bezier\" is not supported by the intro link"
                "bezier")

message(STATUS "intro-link bake: deterministic header ${first_hash}; named errors as expected")
