add_library(pelican_studio_viewport STATIC
    embeddedviewport.cpp
    embeddedviewport.hpp
    engineprocess.cpp
    engineprocess.hpp
    nativewindowhost.cpp
    nativewindowhost.hpp
    viewportgeometry.cpp
    viewportgeometry.hpp
)

set_target_properties(pelican_studio_viewport PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
)

target_include_directories(pelican_studio_viewport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_studio_viewport PUBLIC Qt6::Core Qt6::Widgets)

if(WIN32)
    target_link_libraries(pelican_studio_viewport PRIVATE user32)
endif()
