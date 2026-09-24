# The test-automation surface. It is compiled into pelican_studio always and
# reachable only through `--automation <name>`: without the flag uimain() never
# constructs AutomationServer, so no socket is created and no command below can
# run. Qt6::Test is linked for its event synthesis alone -- nothing here is a
# Catch2 or QTest test binary.
find_package(Qt6 REQUIRED COMPONENTS Core Network Test Widgets)

add_library(pelican_studio_automation STATIC
    automationserver.cpp
    automationserver.hpp
    automationcommands.cpp
    automationcommands.hpp
)

set_target_properties(pelican_studio_automation PROPERTIES
    AUTOMOC ON
    CXX_STANDARD 20
)

target_include_directories(pelican_studio_automation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(pelican_studio_automation
    PUBLIC
        Qt6::Core
        Qt6::Widgets
    PRIVATE
        Qt6::Network
        Qt6::Test
        nlohmann_json
        pelican_studio_viewport
)

if(WIN32)
    # The window-content screenshot (PrintWindow, GetDIBits).
    target_link_libraries(pelican_studio_automation PRIVATE user32 gdi32)
endif()
