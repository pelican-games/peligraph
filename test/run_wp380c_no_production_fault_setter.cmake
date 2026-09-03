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
