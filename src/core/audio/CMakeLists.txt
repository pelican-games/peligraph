if(PELICAN_WITH_AUDIO)
target_sources(pelican_core PRIVATE
    audio.cpp
    # AT-1: the device clock's ring and line.
    audioclock.cpp
    sounddecode.cpp
    # AB-1: the analysis bake (pelican_cli compiles the same two units).
    audioanalysis.cpp
)
endif()
