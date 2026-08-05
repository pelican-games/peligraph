target_sources(pelican_studio PRIVATE
    uimain.cpp
    mainwindow.cpp
    inspectorwidget.cpp
    inspectorwidget.hpp
)

target_link_libraries(pelican_studio PRIVATE Qt6::Core Qt6::Widgets)

# custom target for debug
if(WIN32)
add_custom_target(deploy_studio
  COMMAND ${WINDEPLOYQT_EXECUTABLE} $<TARGET_FILE:pelican_studio>
  DEPENDS pelican_studio
)
add_custom_target(run_studio
    COMMAND $<TARGET_FILE:pelican_studio>
    DEPENDS deploy_studio
)
else()
add_custom_target(run_studio
    COMMAND $<TARGET_FILE:pelican_studio>
    DEPENDS pelican_studio
)
endif()
