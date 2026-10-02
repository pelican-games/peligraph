# IL-2: the release form of the intro link example stays small and imports
# only KERNEL32, USER32 and vulkan-1 (no sockets, no C runtime).
#   EXE        the release form (intro_link_example)
#   DEV_EXE    the development form, read as a control: it must show WS2_32,
#              so an empty or misread import list cannot pass
#   DUMPBIN    dumpbin.exe
#   MAX_BYTES  the fence on the file size (1.5x the measured size, in whole KB)

foreach(var EXE DEV_EXE DUMPBIN MAX_BYTES)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "missing -D${var}")
  endif()
endforeach()

function(intro_link_imports exe out_var)
  execute_process(COMMAND "${DUMPBIN}" /nologo /imports "${exe}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE text ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "dumpbin /imports ${exe} failed (${rc}): ${err}")
  endif()
  string(REGEX MATCHALL "\n[ \t]+[A-Za-z0-9_.-]+\\.[Dd][Ll][Ll]\r?\n" lines "${text}")
  set(dlls "")
  foreach(line IN LISTS lines)
    string(STRIP "${line}" dll)
    string(TOUPPER "${dll}" dll)
    list(APPEND dlls "${dll}")
  endforeach()
  list(SORT dlls)
  set(${out_var} "${dlls}" PARENT_SCOPE)
endfunction()

intro_link_imports("${DEV_EXE}" dev_dlls)
if(NOT "WS2_32.DLL" IN_LIST dev_dlls)
  message(FATAL_ERROR "control: the development form should import WS2_32.dll, read: ${dev_dlls}")
endif()

intro_link_imports("${EXE}" dlls)
set(expected "KERNEL32.DLL;USER32.DLL;VULKAN-1.DLL")
if(NOT dlls STREQUAL expected)
  message(FATAL_ERROR "the release form imports ${dlls}; expected only ${expected}")
endif()
foreach(dll IN LISTS dlls)
  if(dll MATCHES "^(WS2_32|VCRUNTIME|UCRT|API-MS-WIN-CRT|MSVCR|MSVCP)")
    message(FATAL_ERROR "the release form imports ${dll}")
  endif()
endforeach()

file(SIZE "${EXE}" bytes)
if(bytes GREATER MAX_BYTES)
  message(FATAL_ERROR "the release form is ${bytes} bytes, above the fence of ${MAX_BYTES}")
endif()
message(STATUS "release form: ${bytes} bytes (fence ${MAX_BYTES}), imports ${dlls}")
