target_sources(pelican_core PRIVATE
    pelican_core.cpp
    gamelogic.cpp
    cameracontrollersystem.cpp
    modaltransformsystem.cpp
    deterministicrng.cpp
    gamecontext.cpp
    gameobjects.cpp
    userinput.cpp
    sprite/flipbook.cpp
    sprite/pixelpolicy.cpp
    sprite/spriteworld.cpp
    platformer/charactercontroller2d.cpp
    animation/curve.cpp
    valuegraph.cpp
    details/reload/registrationowner.cpp
)

if(PELICAN_WITH_ANIMATION)
target_sources(pelican_core PRIVATE animation/animgraph.cpp)
endif()

add_subdirectory(serialize)
add_subdirectory(components)
add_subdirectory(details/component)
add_subdirectory(details/behavior)
add_subdirectory(details/ecs)
add_subdirectory(details/event)
add_subdirectory(details/system)
