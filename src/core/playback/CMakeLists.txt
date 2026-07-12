target_sources(pelican_core PRIVATE
    camerabake.cpp
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
