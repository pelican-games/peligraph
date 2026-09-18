if(PELICAN_WITH_ANIMATION)
target_sources(pelican_core PRIVATE animationjobs.cpp animationprobe.cpp animationservice.cpp
    animationphasescheduler.cpp
    animationposearena.cpp
    animationserviceabi.cpp
    animationsinkregistry.cpp
    posegraph.cpp
    posegraphruntime.cpp
    vrmapplication.cpp vrmaretarget.cpp)
else()
# The frozen negotiation surface stays; everything that evaluates a pose is gone.
target_sources(pelican_core PRIVATE animation_disabled.cpp)
endif()
