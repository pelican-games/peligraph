target_sources(pelican_core PRIVATE
    actionmap.cpp
    inputsequence.cpp
    inputstate.cpp
    liveinputs.cpp
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
FetchContent_MakeAvailable(glfw)

target_link_libraries(pelican_core PRIVATE glfw)

# The GLFW X11 backend is the only Linux window system this build exposes to
# the engine. Expose its native XID where the backend was actually compiled
# (Wayland-only builds keep the null native handle).
if(UNIX AND NOT APPLE AND GLFW_BUILD_X11)
  target_compile_definitions(pelican_core PRIVATE PELICAN_WINDOW_X11)
endif()
endif()
