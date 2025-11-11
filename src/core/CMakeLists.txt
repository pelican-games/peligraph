add_library(pelican_core pelican_core.cpp log.cpp)

target_link_libraries(pelican_core PRIVATE quill)
set_target_properties(pelican_core PROPERTIES CXX_STANDARD 20)

add_subdirectory(resources)

add_subdirectory(appflow)
add_subdirectory(os)
add_subdirectory(vkcore)
add_subdirectory(material)
add_subdirectory(model)
add_subdirectory(loader)

target_sources(pelican_core PRIVATE)

target_include_directories(pelican_core INTERFACE .)

target_include_directories(pelican_core PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(pelican_core PRIVATE ${Vulkan_LIBRARIES})
target_link_libraries(pelican_core PRIVATE glm)
