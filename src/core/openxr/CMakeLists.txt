add_library(pelican_openxr STATIC
    openxrcompositiontarget.cpp
    openxrdiscovery.cpp
    openxrsession.cpp
)
set_target_properties(pelican_openxr PROPERTIES CXX_STANDARD 20)
target_compile_definitions(pelican_openxr PRIVATE
    "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
)
target_include_directories(pelican_openxr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${Vulkan_INCLUDE_DIRS}
)
target_link_libraries(pelican_openxr PUBLIC OpenXR::headers)
target_link_libraries(pelican_openxr PRIVATE
    OpenXR::openxr_loader
    VulkanMemoryAllocator
    VulkanMemoryAllocator-Hpp
    glm
    nlohmann_json
    quill
)
target_link_libraries(pelican_core PRIVATE pelican_openxr)
