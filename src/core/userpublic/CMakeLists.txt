target_sources(pelican_core PRIVATE
    pelican_core.cpp
    gameobjects.cpp
    userinput.cpp
)

add_subdirectory(serialize)
add_subdirectory(details/component)
add_subdirectory(details/ecs)
