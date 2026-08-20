# WP333 production-wiring control.
#
# crash_symbol_archive_contract drives cmake/pelican_crash_symbols.cmake through
# its own fixture project, so it proves the module works. It does not prove that
# pelican_studio uses it: commenting the pelican_preserve_crash_symbols() call
# out of src/devstudio/CMakeLists.txt leaves that test green while the product
# stops archiving anything. This test looks at what the real studio build
# actually produced.

if(NOT DEFINED ARCHIVE_ROOT)
    message(FATAL_ERROR "ARCHIVE_ROOT is required")
endif()
if(NOT DEFINED EXPECTED_TARGET)
    message(FATAL_ERROR "EXPECTED_TARGET is required")
endif()

set(target_root "${ARCHIVE_ROOT}/${EXPECTED_TARGET}")
if(NOT IS_DIRECTORY "${target_root}")
    message(FATAL_ERROR
        "No crash-symbol archive for '${EXPECTED_TARGET}' under "
        "'${ARCHIVE_ROOT}'. The production build is not wired to "
        "pelican_preserve_crash_symbols(); a dump taken against this build "
        "will be unreadable once the binary is replaced.")
endif()

file(GLOB generations "${target_root}/generation-*")
if(generations STREQUAL "")
    message(FATAL_ERROR
        "'${target_root}' exists but holds no generation. Nothing was "
        "archived for the production ${EXPECTED_TARGET} build.")
endif()

# Newest generation wins; the directory name is zero padded so this sorts.
list(SORT generations)
list(GET generations -1 newest)

set(manifest "${newest}/manifest.txt")
if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR "Archived generation '${newest}' has no manifest.txt")
endif()

file(READ "${manifest}" manifest_text)

foreach(field IN ITEMS "target" "source_commit" "pdb_file" "pdb_sha256")
    if(NOT manifest_text MATCHES "(^|\n)${field}=([^\n]+)")
        message(FATAL_ERROR
            "manifest.txt in '${newest}' has no non-empty '${field}'. A dump "
            "cannot be tied back to a commit without it.")
    endif()
    set("manifest_${field}" "${CMAKE_MATCH_2}")
endforeach()

if(NOT manifest_target STREQUAL EXPECTED_TARGET)
    message(FATAL_ERROR
        "manifest target is '${manifest_target}', expected "
        "'${EXPECTED_TARGET}'")
endif()

# The commit has to be a real hash, not a placeholder: recovering which build a
# dump came from is the whole point of the archive.
# CMake's regex flavour has no {n} repetition, so the length is checked apart
# from the alphabet.
string(LENGTH "${manifest_source_commit}" commit_length)
if(NOT commit_length EQUAL 40 OR
   NOT manifest_source_commit MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "manifest source_commit is '${manifest_source_commit}', which is not a "
        "full commit hash")
endif()

# The archived pdb must carry the commit in its name, because that is the name
# the linker stamps into the binary via /PDBALTPATH and therefore the only
# thing a crash dump can lead us back to.
if(NOT manifest_pdb_file MATCHES "${manifest_source_commit}")
    message(FATAL_ERROR
        "archived pdb '${manifest_pdb_file}' does not carry commit "
        "'${manifest_source_commit}' in its name")
endif()

if(NOT EXISTS "${newest}/${manifest_pdb_file}")
    message(FATAL_ERROR
        "manifest names '${manifest_pdb_file}' but the file is absent from "
        "'${newest}'")
endif()

file(SHA256 "${newest}/${manifest_pdb_file}" actual_pdb_sha)
if(NOT actual_pdb_sha STREQUAL manifest_pdb_sha256)
    message(FATAL_ERROR
        "archived pdb hash ${actual_pdb_sha} does not match the manifest's "
        "${manifest_pdb_sha256}")
endif()

message(STATUS
    "production ${EXPECTED_TARGET} archived at ${manifest_source_commit} "
    "(${manifest_pdb_file})")
