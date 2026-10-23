target_sources(pelican_core PRIVATE
    actionmap.cpp
    inputsequence.cpp
    inputstate.cpp
    window.cpp
)

if(PLATFORM_DESKTOP)
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        7b6aead9fb88b3623e3b3725ebb42670cbe4c579 # 3.4
)
FetchContent_MakeAvailable(glfw)

target_link_libraries(pelican_core PRIVATE glfw)
endif()
