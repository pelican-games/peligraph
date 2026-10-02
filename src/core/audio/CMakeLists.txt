if(PELICAN_WITH_AUDIO)
target_sources(pelican_core PRIVATE
    audio.cpp
    sounddecode.cpp
    # AB-1: the analysis bake (pelican_cli compiles the same two units).
    audioanalysis.cpp
)
endif()
