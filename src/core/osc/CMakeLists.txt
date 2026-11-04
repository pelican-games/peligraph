target_sources(pelican_core PRIVATE
    oscparser.cpp
    oscbindings.cpp
    oscinput.cpp
    osclistenerconfig.cpp
    osclistener.cpp
)

if(WIN32)
    target_link_libraries(pelican_core PRIVATE ws2_32)
endif()
