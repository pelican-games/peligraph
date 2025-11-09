target_sources(pelican_core PRIVATE
    window.cpp
)

if(PLATFORM_DESKTOP)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
)
FetchContent_MakeAvailable(glfw)

target_link_libraries(pelican_core PRIVATE glfw)
endif()
