target_sources(pelican_core PRIVATE
    pelican_core.cpp
    cameracontrollersystem.cpp
    deterministicrng.cpp
    gamecontext.cpp
    gameobjects.cpp
    userinput.cpp
    sprite/flipbook.cpp
    sprite/pixelpolicy.cpp
    sprite/spriteworld.cpp
    platformer/charactercontroller2d.cpp
    animation/animgraph.cpp
    details/reload/registrationowner.cpp
)

add_subdirectory(serialize)
add_subdirectory(components)
add_subdirectory(details/component)
add_subdirectory(details/behavior)
add_subdirectory(details/ecs)
add_subdirectory(details/event)
add_subdirectory(details/system)
