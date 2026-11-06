target_sources(pelican_core PRIVATE
    actionmap.cpp
    inputsequence.cpp
    inputstate.cpp
    liveinputs.cpp
    liveinputrecording.cpp
    liveinputrecordingbake.cpp
)

# The Window module has one implementation per platform: GLFW on the desktop,
# the activity's ANativeWindow on Android (vkcore picks the matching surface).
if(ANDROID)
target_sources(pelican_core PRIVATE window_android.cpp ../../platform/android/inputqueue.cpp)
target_include_directories(pelican_core PRIVATE "${ANDROID_NDK}/sources/android/native_app_glue")
else()
target_sources(pelican_core PRIVATE window_glfw.cpp outputwindow.cpp)
endif()

if(PLATFORM_DESKTOP)
if(UNIX AND NOT APPLE)
  # LX-1: GLFW builds its Wayland and X11 backends by default and stops when their
  # tools / headers are missing (a Linux compute node has no wayland-scanner).
  # A headless player never initialises GLFW, so a missing window system drops
  # that backend with a named line instead of failing the configure.
  find_program(PELICAN_WAYLAND_SCANNER wayland-scanner)
  if(NOT PELICAN_WAYLAND_SCANNER AND NOT DEFINED GLFW_BUILD_WAYLAND)
    message(STATUS "wayland-scanner not found: GLFW without Wayland (windowed runs need X11)")
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Build support for Wayland")
  endif()
  find_package(X11 QUIET)
  if(NOT DEFINED GLFW_BUILD_X11 AND NOT (X11_FOUND AND X11_Xrandr_INCLUDE_PATH AND
     X11_Xinerama_INCLUDE_PATH AND X11_Xkb_INCLUDE_PATH AND X11_Xcursor_INCLUDE_PATH AND
     X11_Xi_INCLUDE_PATH AND X11_Xshape_INCLUDE_PATH))
    message(STATUS "X11 development headers not found: GLFW without X11 (windowed runs need Wayland)")
    set(GLFW_BUILD_X11 OFF CACHE BOOL "Build support for X11")
  endif()
endif()
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        7b6aead9fb88b3623e3b3725ebb42670cbe4c579 # 3.4
)
# DMAP-1a: prepare and patch a build-private copy. In particular, a shared
# FETCHCONTENT_SOURCE_DIR_GLFW checkout is an input, never a mutation target.
FetchContent_GetProperties(glfw)
if(NOT glfw_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(glfw)
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()
endif()
find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${glfw_SOURCE_DIR}" rev-parse HEAD
  RESULT_VARIABLE pelican_glfw_revision_result
  OUTPUT_VARIABLE pelican_glfw_revision
  ERROR_VARIABLE pelican_glfw_revision_error
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT pelican_glfw_revision_result EQUAL 0 OR
   NOT pelican_glfw_revision STREQUAL "7b6aead9fb88b3623e3b3725ebb42670cbe4c579")
  message(FATAL_ERROR
    "GLFW input is not the pinned revision: ${pelican_glfw_revision} "
    "${pelican_glfw_revision_error}")
endif()
set(pelican_glfw_patched_dir "${FETCHCONTENT_BASE_DIR}/glfw-pelican-src")
file(REMOVE_RECURSE "${pelican_glfw_patched_dir}")
file(COPY "${glfw_SOURCE_DIR}/" DESTINATION "${pelican_glfw_patched_dir}"
  PATTERN ".git" EXCLUDE)
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DPELICAN_GLFW_SOURCE_DIR=${pelican_glfw_patched_dir}"
          -P "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/patch_glfw_dinput_bounds.cmake"
  RESULT_VARIABLE pelican_glfw_patch_result)
if(NOT pelican_glfw_patch_result EQUAL 0)
  message(FATAL_ERROR "patching GLFW failed: ${pelican_glfw_patch_result}")
endif()
set(PELICAN_GLFW_INPUT_SOURCE_DIR "${glfw_SOURCE_DIR}" CACHE INTERNAL
    "Pristine GLFW input source")
set(PELICAN_GLFW_SOURCE_DIR "${pelican_glfw_patched_dir}" CACHE INTERNAL
    "Build-private, patched GLFW source")
add_subdirectory("${pelican_glfw_patched_dir}" "${glfw_BINARY_DIR}")

target_link_libraries(pelican_core PRIVATE glfw)

# The GLFW X11 backend is the only Linux window system this build exposes to
# the engine. Expose its native XID where the backend was actually compiled
# (Wayland-only builds keep the null native handle).
if(UNIX AND NOT APPLE AND GLFW_BUILD_X11)
  target_compile_definitions(pelican_core PRIVATE PELICAN_WINDOW_X11)
endif()
endif()
