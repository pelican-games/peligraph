if(NOT DEFINED PRODUCTION_ARCHIVE OR NOT EXISTS "${PRODUCTION_ARCHIVE}")
  message(FATAL_ERROR "pelican_core production archive is missing: ${PRODUCTION_ARCHIVE}")
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
