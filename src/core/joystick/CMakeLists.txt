if(NOT PLATFORM_DESKTOP)
  message(FATAL_ERROR "joystick.module_unavailable@1: PELICAN_WITH_JOYSTICK_INPUT needs the GLFW desktop build")
endif()
target_sources(pelican_core PRIVATE joystickmonitor.cpp joystickruntime.cpp joystick_glfw.cpp)
