target_sources(pelican_core PRIVATE
    projectsrc.cpp
    basicconfig.cpp
)

FetchContent_Declare(
  nlohmann
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
)
FetchContent_MakeAvailable(nlohmann)
target_link_libraries(pelican_core PRIVATE nlohmann_json)
