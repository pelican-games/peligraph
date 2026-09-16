target_sources(pelican_core PRIVATE 
    modelview.cpp
    localtransformsystem.cpp
    modelviewtransformsystem.cpp
    camerasystem.cpp
    modelviewupdatesystem.cpp
    spriteviewsystem.cpp
)

if(PELICAN_WITH_ANIMATION)
target_sources(pelican_core PRIVATE animationsystem.cpp)
endif()
