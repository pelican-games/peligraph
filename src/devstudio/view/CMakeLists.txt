target_sources(pelican_studio PRIVATE
    uimain.cpp
)

target_link_libraries(pelican_studio PRIVATE Qt6::Core Qt6::Widgets Qt6::Quick)
