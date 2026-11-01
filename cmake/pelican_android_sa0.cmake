# SA0: the Android (NDK clang, arm64-v8a) spike.  Only the logic that does not
# touch Vulkan, a window, Qt or the Win32 API is built here: the schema leaf,
# pelican_project, and Catch2 test executables over them (plus a few
# header-only / CPU-only parts of core).  The root CMakeLists includes this
# file and returns before any of the desktop configuration runs, so a Windows
# configure never reads it.
#
# Dependencies are not downloaded: pass FETCHCONTENT_SOURCE_DIR_<NAME> for
# nlohmann, picosha2, stb, glm, argparse, quill and catch2 (see the SA0 report).

if(NOT ANDROID)
  message(FATAL_ERROR "PELICAN_ANDROID_SA0 is for the Android NDK toolchain only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# NDK r27c's libc++ (LLVM 18) keeps std::jthread / std::stop_token behind the
# experimental-library switch; MSVC's STL ships them as stable C++20.
add_compile_options(-fexperimental-library)

# Where the test executables find the source tree on the device.
set(PELICAN_SA0_DEVICE_ROOT "/data/local/tmp/sa0/src" CACHE STRING
    "Source-tree mirror on the device that PELICAN_TEST_SOURCE_DIR points at")

FetchContent_Declare(nlohmann
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        55f93686c01528224f448c19128836e7df245f72) # v3.12.0
FetchContent_MakeAvailable(nlohmann)

FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        a532f5b1cf27d6a3c099437e6959cf7e398a0a67) # 1.0.2
FetchContent_MakeAvailable(glm)

FetchContent_Declare(argparse
  GIT_REPOSITORY https://github.com/p-ranav/argparse.git
  GIT_TAG        3eda91b2e1ce7d569f84ba295507c4cd8fd96910) # v3.2
FetchContent_MakeAvailable(argparse)

FetchContent_Declare(quill
  GIT_REPOSITORY https://github.com/odygrd/quill.git
  GIT_TAG        f1d3250d0f3d2e9b90ae1ccfdd639d8a57b45f40) # v10.2.0
FetchContent_MakeAvailable(quill)

foreach(pelican_sa0_header_dep picosha2 stb)
  if(pelican_sa0_header_dep STREQUAL "picosha2")
    FetchContent_Declare(picosha2
      GIT_REPOSITORY https://github.com/okdshin/PicoSHA2.git
      GIT_TAG        161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29) # v1.0.1
  else()
    FetchContent_Declare(stb
      GIT_REPOSITORY https://github.com/nothings/stb.git
      GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4)
  endif()
  FetchContent_GetProperties(${pelican_sa0_header_dep})
  if(NOT ${pelican_sa0_header_dep}_POPULATED)
    if(POLICY CMP0169)
      cmake_policy(PUSH)
      cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(${pelican_sa0_header_dep})
    if(POLICY CMP0169)
      cmake_policy(POP)
    endif()
  endif()
  if(NOT TARGET ${pelican_sa0_header_dep})
    add_library(${pelican_sa0_header_dep} INTERFACE)
    target_include_directories(${pelican_sa0_header_dep} INTERFACE
      ${${pelican_sa0_header_dep}_SOURCE_DIR})
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Libraries
add_subdirectory(src/schema)
include_directories(${CMAKE_SOURCE_DIR}/src/schema)
add_subdirectory(src/project)

# The schema vocabulary adapter lives under core/communication, whose
# CMakeLists also adds sources to pelican_core; build only the adapter here.
add_library(pelican_schema_core_adapter STATIC
  src/core/communication/schemavocabularyadapter.cpp)
target_include_directories(pelican_schema_core_adapter PUBLIC
  ${CMAKE_SOURCE_DIR}/src/core/communication)
target_link_libraries(pelican_schema_core_adapter PUBLIC pelican_schema_leaf)
target_compile_options(pelican_schema_core_adapter PRIVATE
  -Wswitch-enum -Werror=switch-enum)

# CPU-only slices of core.  Each is optional: a slice whose sources do not
# compile is reported, not fixed in shared code.
set(PELICAN_SA0_CORE_SLICES "" CACHE STRING
    "Semicolon list of core slices to build: physquery;animation")

set(pelican_sa0_core_defs
  PELICAN_WITH_AUDIO=$<BOOL:${PELICAN_WITH_AUDIO}>
  PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
  PELICAN_WITH_EXR=$<BOOL:${PELICAN_WITH_EXR}>
  PELICAN_WITH_RPC=$<BOOL:${PELICAN_WITH_RPC}>
  PELICAN_WITH_ANIMATION=$<BOOL:${PELICAN_WITH_ANIMATION}>
  PELICAN_WITH_SEQPLAYER=$<BOOL:${PELICAN_WITH_SEQPLAYER}>
  PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
  PELICAN_WITH_PHYSICS=$<BOOL:${PELICAN_WITH_PHYSICS}>
  PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
  PELICAN_WITH_RENDERDOC=$<BOOL:${PELICAN_WITH_RENDERDOC}>
  PELICAN_WITH_HOT_RELOAD=$<BOOL:${PELICAN_WITH_HOT_RELOAD}>
  PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=$<BOOL:${PELICAN_WITH_STANDARD_RENDER_ALGORITHMS}>)

function(pelican_sa0_core_slice name)
  add_library(pelican_sa0_core_${name} STATIC ${ARGN})
  target_include_directories(pelican_sa0_core_${name} PUBLIC
    ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/core/userpublic)
  target_compile_definitions(pelican_sa0_core_${name} PUBLIC ${pelican_sa0_core_defs}
    PRIVATE PELICAN_ENGINE_BUILD=1
            PELICAN_WITH_BUILTIN_PHYSICS=$<BOOL:${PELICAN_WITH_BUILTIN_PHYSICS}>
            PELICAN_WITH_JOLT_PHYSICS=0)
  target_link_libraries(pelican_sa0_core_${name} PUBLIC glm nlohmann_json quill
    PRIVATE pelican_project)
endfunction()

if("physquery" IN_LIST PELICAN_SA0_CORE_SLICES)
  pelican_sa0_core_slice(physquery
    src/core/phys/physquery.cpp
    src/core/phys/physquerysweep.cpp
    src/core/phys/physqueryaggregate.cpp
    src/core/phys/physquerycontract.cpp
    src/core/phys/trianglemesh.cpp)
endif()
if("animation" IN_LIST PELICAN_SA0_CORE_SLICES)
  pelican_sa0_core_slice(animation
    src/core/animation/animationjobs.cpp
    src/core/animation/animationprobe.cpp
    src/core/model/skeletalanimation.cpp
    src/core/log.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests: one executable per source (the desktop groups are a link-time
# optimisation only), run on the device by tools/android/sa0_run_tests.sh.
if(NOT BUILD_TESTING)
  return()
endif()

FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        b3fb4b9feafcd8d91c5cb510a4775143fdbef02f) # v3.11.0
FetchContent_MakeAvailable(Catch2)

set(PELICAN_SA0_TESTS "")
function(pelican_sa0_test name)
  cmake_parse_arguments(SA0 "" "" "SOURCES;LIBS" ${ARGN})
  set(target pelican_test_${name})
  add_executable(${target} test/${name}.cpp test/retain_observed_test_support.cpp ${SA0_SOURCES})
  target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain nlohmann_json ${SA0_LIBS})
  target_compile_definitions(${target} PRIVATE
    PELICAN_TEST_SOURCE_DIR="${PELICAN_SA0_DEVICE_ROOT}")
  set(PELICAN_SA0_TESTS ${PELICAN_SA0_TESTS} ${target} PARENT_SCOPE)
endfunction()

# Plain executables (no Catch) that the desktop registers with add_test.
function(pelican_sa0_plain_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE pelican_project)
  target_compile_definitions(${target} PRIVATE
    PELICAN_TEST_SOURCE_DIR="${PELICAN_SA0_DEVICE_ROOT}")
  set(PELICAN_SA0_TESTS ${PELICAN_SA0_TESTS} ${target} PARENT_SCOPE)
endfunction()

pelican_sa0_plain_test(pelican_test_project_execution_leaf
  test/project_execution_shaderreference_test.cpp
  test/project_execution_passinfo_test.cpp
  test/project_execution_passloopindex_test.cpp
  test/project_execution_computetaskdefinition_test.cpp)
pelican_sa0_plain_test(pelican_test_project_library_boundary test/project_library_boundary.cpp)
pelican_sa0_plain_test(pelican_test_render_graph_ir_boundary test/render_graph_ir_boundary.cpp)

pelican_sa0_test(schema_vocabulary_test LIBS pelican_project pelican_schema_core_adapter)
pelican_sa0_test(jsonrpc_test LIBS pelican_project)
pelican_sa0_test(combination_fixture_test LIBS pelican_project glm)
foreach(t
    importmanifest_test importrules_test assetdataformat_test projectformat_test
    packagemanifest_test materialformat_test materiallowering_test rasterpass_test
    rendervocabulary_test
    targetplanning_test targetrenderplanning_test samplecountplanning_test
    graphvariantpolicy_test generationresources_test passshapepolicy_test
    gpudrawtiming_test
    assetsmanifest_test surfaceformat_test openpbrmapping_test)
  pelican_sa0_test(${t} LIBS pelican_project)
endforeach()
pelican_sa0_test(projectmigration_test LIBS pelican_project argparse
  SOURCES test/projectmigrationteststep.cpp src/devcli/projectmigratecommand.cpp)
# Diagnostic twin (not in pelican_sa0_tests): link(2) replaced by an exclusive
# copy, to separate the hard-link refusal from any other failure.
add_executable(pelican_sa0_diag_projectmigration_linkshim
  test/projectmigration_test.cpp test/retain_observed_test_support.cpp
  test/projectmigrationteststep.cpp src/devcli/projectmigratecommand.cpp
  tools/android/sa0_link_shim.cpp)
target_link_libraries(pelican_sa0_diag_projectmigration_linkshim PRIVATE
  Catch2::Catch2WithMain nlohmann_json pelican_project argparse)
target_compile_definitions(pelican_sa0_diag_projectmigration_linkshim PRIVATE
  PELICAN_TEST_SOURCE_DIR="${PELICAN_SA0_DEVICE_ROOT}")
target_link_options(pelican_sa0_diag_projectmigration_linkshim PRIVATE -Wl,--wrap=link)
set_target_properties(pelican_sa0_diag_projectmigration_linkshim PROPERTIES EXCLUDE_FROM_ALL TRUE)

if(PELICAN_WITH_CANVAS)
  foreach(t canvas_lowering_test canvas_layers_test canvas_text_test
            canvas_blend_test canvas_ship_test canvas_filter_test)
    pelican_sa0_test(${t} LIBS pelican_project)
  endforeach()
endif()

# Header-only / CPU-only core pieces.
pelican_sa0_test(parallel_prepare_test)
pelican_sa0_test(render_coverage_test)
# xractivation_test is not built: core/launchconfig.hpp includes
# <vulkan/vulkan.hpp> for vk::Extent2D (the NDK ships only the C headers).
option(PELICAN_SA0_TRY_VULKAN_LEAKS "Also build tests whose headers include vulkan.hpp" OFF)
if(PELICAN_SA0_TRY_VULKAN_LEAKS)
  pelican_sa0_test(xractivation_test LIBS quill)
endif()
if(TARGET pelican_sa0_core_physquery)
  pelican_sa0_test(physquery_test LIBS pelican_sa0_core_physquery)
  pelican_sa0_test(trianglemesh_test LIBS pelican_sa0_core_physquery)
endif()
if(TARGET pelican_sa0_core_animation)
  pelican_sa0_test(animation_jobs_test LIBS pelican_sa0_core_animation)
endif()

add_custom_target(pelican_sa0_tests DEPENDS ${PELICAN_SA0_TESTS})
string(REPLACE ";" "\n" pelican_sa0_test_list "${PELICAN_SA0_TESTS}")
file(WRITE "${CMAKE_BINARY_DIR}/sa0_tests.txt" "${pelican_sa0_test_list}\n")
