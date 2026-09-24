if(NOT DEFINED PRODUCTION_ARCHIVE OR NOT EXISTS "${PRODUCTION_ARCHIVE}")
  message(FATAL_ERROR "pelican_core production archive is missing: ${PRODUCTION_ARCHIVE}")
endif()

if(NOT DEFINED PROJECT_ARCHIVE OR NOT EXISTS "${PROJECT_ARCHIVE}")
  message(FATAL_ERROR "pelican_project production archive is missing: ${PROJECT_ARCHIVE}")
endif()

if(NOT DEFINED WP380C_DUMPBIN OR NOT EXISTS "${WP380C_DUMPBIN}")
  message(FATAL_ERROR "dumpbin is missing: ${WP380C_DUMPBIN}")
endif()
execute_process(
  COMMAND "${WP380C_DUMPBIN}" /symbols "${PRODUCTION_ARCHIVE}"
  RESULT_VARIABLE dump_result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE dump_error)
if(NOT dump_result EQUAL 0)
  message(FATAL_ERROR "dumpbin failed (${dump_result}): ${dump_error}")
endif()
execute_process(
  COMMAND "${WP380C_DUMPBIN}" /symbols "${PROJECT_ARCHIVE}"
  RESULT_VARIABLE project_dump_result
  OUTPUT_VARIABLE project_symbols
  ERROR_VARIABLE project_dump_error)
if(NOT project_dump_result EQUAL 0)
  message(FATAL_ERROR "dumpbin failed (${project_dump_result}): ${project_dump_error}")
endif()

string(FIND "${symbols}" "SceneLoadFaultInjectorForTesting" setter_index)
if(NOT setter_index EQUAL -1)
  message(FATAL_ERROR
    "test-only scene-load fault setter leaked into pelican_core")
endif()

foreach(test_source IN ITEMS "${SCENE_FAULT_TEST_SOURCE}" "${MODEL_RELEASE_FAULT_TEST_SOURCE}"
                            "${MODEL_RELOAD_FAULT_TEST_SOURCE}")
  string(FIND "${PRODUCTION_SOURCES}" "${test_source}" source_index)
  if(NOT source_index EQUAL -1)
    message(FATAL_ERROR "test-only fault source leaked into pelican_core: ${test_source}")
  endif()
endforeach()

# AG-0: the test-only graph module proves a module can be purged. It must never
# be part of a shipped archive, by symbol or by source.
foreach(archive_symbols IN ITEMS "${symbols}" "${project_symbols}")
  string(FIND "${archive_symbols}" "registerTestOpaqueGraphModule" module_index)
  if(NOT module_index EQUAL -1)
    message(FATAL_ERROR "test-only graph module leaked into a production archive")
  endif()
  string(FIND "${archive_symbols}" "pelican.test.opaque" namespace_index)
  if(NOT namespace_index EQUAL -1)
    message(FATAL_ERROR "test-only graph module namespace leaked into a production archive")
  endif()
endforeach()
foreach(production_sources IN ITEMS "${PRODUCTION_SOURCES}" "${PROJECT_SOURCES}")
  string(FIND "${production_sources}" "${GRAPH_TEST_MODULE_SOURCE}" source_index)
  if(NOT source_index EQUAL -1)
    message(FATAL_ERROR
      "test-only graph module source leaked into production: ${GRAPH_TEST_MODULE_SOURCE}")
  endif()
endforeach()

# MG-1: the test-only migration step proves the migration path end to end. No
# shipped archive or tool may carry it: pelican_core, pelican_project and
# pelican_cli by source, the two archives by symbol and by step id.
foreach(archive_symbols IN ITEMS "${symbols}" "${project_symbols}")
  foreach(needle IN ITEMS "registerTestSceneKeyRenameMigrationStep"
                          "testSceneKeyRenameStepId"
                          "pelican.test.scene_key_rename")
    string(FIND "${archive_symbols}" "${needle}" step_index)
    if(NOT step_index EQUAL -1)
      message(FATAL_ERROR "test-only migration step leaked into a production archive: ${needle}")
    endif()
  endforeach()
endforeach()
foreach(production_sources IN ITEMS "${PRODUCTION_SOURCES}" "${PROJECT_SOURCES}" "${CLI_SOURCES}")
  string(FIND "${production_sources}" "projectmigrationteststep" source_index)
  if(NOT source_index EQUAL -1)
    message(FATAL_ERROR
      "test-only migration step source leaked into production: ${MIGRATION_TEST_STEP_SOURCE}")
  endif()
endforeach()
if(NOT DEFINED MIGRATION_TEST_STEP_SOURCE OR NOT EXISTS "${MIGRATION_TEST_STEP_SOURCE}")
  message(FATAL_ERROR "the test-only migration step source is missing: ${MIGRATION_TEST_STEP_SOURCE}")
endif()

foreach(header IN ITEMS "${SCENE_HEADER}" "${RELEASE_HEADER}" "${INSTANCE_HEADER}")
  if(NOT EXISTS "${header}")
    message(FATAL_ERROR "production fault header is missing: ${header}")
  endif()
  file(READ "${header}" header_contents)
  string(REGEX MATCH
    "set[A-Za-z0-9_]*ForTesting[A-Za-z0-9_]*[ \t\r\n]*\\([^;{}]*\\)[ \t\r\n]*(noexcept[ \t\r\n]*)?\\{"
    header_setter_definition "${header_contents}")
  if(header_setter_definition)
    message(FATAL_ERROR "test-only fault setter has a production-header definition: ${header}")
  endif()
endforeach()

foreach(setter IN ITEMS "SceneLoadFaultInjectorForTesting"
                            "MorphMetadataReleaseFailureForTesting"
                            "MaterialReleaseFailureForTesting"
                            "TextureReleaseFailureForTesting"
                            "setRayQueryGeometryGenerationForTesting")
  string(FIND "${symbols}" "${setter}" setter_index)
  if(NOT setter_index EQUAL -1)
    message(FATAL_ERROR "test-only fault setter leaked into pelican_core: ${setter}")
  endif()
endforeach()
