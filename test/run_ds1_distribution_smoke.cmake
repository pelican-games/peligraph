# DS-1 default configure smoke. Dependency source overrides can make this offline.
# Run from a toolchain environment with Python and uv removed from PATH.
# After success, build pelican_player in OUT_DIR via the platform build wrapper.
foreach(required SOURCE_DIR OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} required")
    endif()
endforeach()
find_program(ds1_python NAMES python python3 uv PATHS ENV PATH NO_DEFAULT_PATH)
if(ds1_python)
    message(FATAL_ERROR "T0 requires PATH without Python/uv: ${ds1_python}")
endif()
set(forwarded)
get_cmake_property(variables VARIABLES)
foreach(var IN LISTS variables)
    if(var MATCHES "^FETCHCONTENT_SOURCE_DIR_" OR var STREQUAL "CMAKE_PREFIX_PATH" OR
       var STREQUAL "CMAKE_MAKE_PROGRAM" OR var STREQUAL "CMAKE_CXX_COMPILER" OR
       var STREQUAL "CMAKE_C_COMPILER")
        list(APPEND forwarded "-D${var}=${${var}}")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${OUT_DIR}"
    -G Ninja -DCMAKE_BUILD_TYPE=Release -DPELICAN_PYTHON_TESTS=OFF
    -DPELICAN_WITH_SPIRV_LINK=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON
    ${forwarded} RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 600)
file(WRITE "${OUT_DIR}/ds1-configure.log" "${out}\n${err}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Python-free configure failed (${result}): ${out}\n${err}")
endif()
