target_sources(pelican_core PRIVATE
    camerabake.cpp
    launchcameraoverride.cpp
    # The prepared-scene protocol for both players, shared by the real and the
    # stub build variants below so the stubs cannot drift from it.
    playbackscenestate.cpp
)

if(PELICAN_WITH_SEQPLAYER)
target_sources(pelican_core PRIVATE
    seqplayer.cpp
)
else()
target_sources(pelican_core PRIVATE
    seqplayer_stub.cpp
)
endif()

if(PELICAN_WITH_VAT)
target_sources(pelican_core PRIVATE
    vatplayer.cpp
)
else()
target_sources(pelican_core PRIVATE
    vatplayer_stub.cpp
)
endif()
