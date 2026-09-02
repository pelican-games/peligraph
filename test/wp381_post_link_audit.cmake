foreach(required IN ITEMS DUMPBIN STALE_DLL HOST_EXE SOURCE_DEPENDENCIES
                          CURRENT_USERPUBLIC EXPECTED_HOST)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

if(NOT EXISTS "${SOURCE_DEPENDENCIES}")
  message(FATAL_ERROR
    "stale fixture /sourceDependencies output is missing: ${SOURCE_DEPENDENCIES}")
endif()
file(READ "${SOURCE_DEPENDENCIES}" source_dependencies)
string(REPLACE "\\" "/" source_dependencies "${source_dependencies}")
string(REPLACE "\\" "/" current_userpublic "${CURRENT_USERPUBLIC}")
string(TOLOWER "${source_dependencies}" source_dependencies)
string(TOLOWER "${current_userpublic}" current_userpublic)
string(FIND "${source_dependencies}" "${current_userpublic}" live_sdk_index)
if(NOT live_sdk_index EQUAL -1)
  message(FATAL_ERROR
    "frozen v1 fixture consumed a live src/core/userpublic dependency")
endif()

execute_process(
  COMMAND "${DUMPBIN}" /nologo /imports "${STALE_DLL}"
  RESULT_VARIABLE imports_result
  OUTPUT_VARIABLE imports_output
  ERROR_VARIABLE imports_error)
if(NOT imports_result EQUAL 0)
  message(FATAL_ERROR
    "dumpbin /imports failed (${imports_result}): ${imports_error}")
endif()
string(FIND "${imports_output}" "${EXPECTED_HOST}" host_index)
if(host_index EQUAL -1)
  message(FATAL_ERROR
    "stale import stub module is not ${EXPECTED_HOST}:\n${imports_output}")
endif()
set(old_behavior
  "?__registerBehavior@UserBehaviorRegistererTemplatePublic@internal@Pelican@@AEAA?AVRegistrationToken@23@UBehaviorRegistration@23@@Z")
set(old_getter
  "?getBehaviorRegisterer@internal@Pelican@@YAAEAVUserBehaviorRegistererTemplatePublic@12@XZ")
set(old_event
  "?__registerEvent@UserEventRegistererTemplatePublic@internal@Pelican@@AEAA?AVRegistrationToken@23@UEventTypeRegistration@23@@Z")
set(expected_host_imports
  "??0Behavior@Pelican@@QEAA@XZ"
  "??1Behavior@Pelican@@UEAA@XZ"
  "${old_behavior}"
  "${old_event}"
  "${old_getter}"
  "?getEventRegisterer@internal@Pelican@@YAAEAVUserEventRegistererTemplatePublic@12@XZ"
  "?onDestroy@Behavior@Pelican@@UEAAXAEAVBehaviorContext@2@@Z"
  "?onInit@Behavior@Pelican@@UEAAXAEAVBehaviorContext@2@@Z"
  "?onUpdate@Behavior@Pelican@@UEAAXAEAVBehaviorContext@2@@Z")
foreach(expected IN LISTS expected_host_imports)
  string(FIND "${imports_output}" "${expected}" expected_index)
  if(expected_index EQUAL -1)
    message(FATAL_ERROR
      "stale behavior import set is missing ${expected}:\n${imports_output}")
  endif()
endforeach()
string(REGEX MATCHALL "@Pelican@@" pelican_import_markers
  "${imports_output}")
list(LENGTH pelican_import_markers pelican_import_count)
if(NOT pelican_import_count EQUAL 9)
  message(FATAL_ERROR
    "stale behavior host import set has ${pelican_import_count} symbols; expected exactly 9:\n${imports_output}")
endif()
foreach(unexpected IN ITEMS "__emit@" "__registerSystem"
                            "pelican_game_logic_epoch_v2")
  string(FIND "${imports_output}" "${unexpected}" unexpected_index)
  if(NOT unexpected_index EQUAL -1)
    message(FATAL_ERROR
      "stale behavior import set unexpectedly contains ${unexpected}:\n${imports_output}")
  endif()
endforeach()

execute_process(
  COMMAND "${DUMPBIN}" /nologo /exports "${HOST_EXE}"
  RESULT_VARIABLE exports_result
  OUTPUT_VARIABLE exports_output
  ERROR_VARIABLE exports_error)
if(NOT exports_result EQUAL 0)
  message(FATAL_ERROR
    "dumpbin /exports failed (${exports_result}): ${exports_error}")
endif()
foreach(legacy IN ITEMS "__registerBehavior" "__registerEvent" "__emit@"
                        "__registerSystem")
  string(FIND "${exports_output}" "${legacy}" legacy_index)
  if(NOT legacy_index EQUAL -1)
    message(FATAL_ERROR
      "v2 host still exports legacy entrance ${legacy}:\n${exports_output}")
  endif()
endforeach()
string(FIND "${exports_output}" "pelican_game_logic_epoch_v2" anchor_index)
if(anchor_index EQUAL -1)
  message(FATAL_ERROR
    "v2 host does not export pelican_game_logic_epoch_v2:\n${exports_output}")
endif()

message(STATUS
  "WP381 post-link audit passed: frozen dependencies, v1 import module/set, and v2 host exports")
