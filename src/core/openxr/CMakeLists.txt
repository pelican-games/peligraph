add_library(pelican_openxr STATIC
    openxraction.cpp
    openxrdiscovery.cpp
    openxrsession.cpp
)
set_target_properties(pelican_openxr PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_openxr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${Vulkan_INCLUDE_DIRS}
)
target_link_libraries(pelican_openxr PUBLIC OpenXR::headers)
target_link_libraries(pelican_openxr PRIVATE OpenXR::openxr_loader nlohmann_json quill)
target_link_libraries(pelican_core PRIVATE pelican_openxr)
