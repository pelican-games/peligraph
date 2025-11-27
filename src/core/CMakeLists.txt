set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>/lib)
add_library(pelican_core log.cpp)
unset(CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
set_target_properties(pelican_core PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_link_libraries(pelican_core PRIVATE quill)
set_target_properties(pelican_core PROPERTIES CXX_STANDARD 20)

add_subdirectory(resources)

add_subdirectory(appflow)
add_subdirectory(os)
add_subdirectory(ecs)
add_subdirectory(asset)
add_subdirectory(loader)
add_subdirectory(vkcore)
add_subdirectory(shader)
add_subdirectory(material)
add_subdirectory(model)
add_subdirectory(renderingpass)
add_subdirectory(renderer)
add_subdirectory(fullscreenpass)

target_sources(pelican_core PRIVATE)

add_subdirectory(userpublic)
target_include_directories(pelican_core INTERFACE ./userpublic)

set(PELICAN_LIB_DIR ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>/lib)
set(PELICAN_INCLUDE_DIR ${PELICAN_LIB_DIR}/include)
set(PELICAN_SRC_PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/userpublic/*.hpp)
if(WIN32)
string(REPLACE / \\ PELICAN_INCLUDE_DIR ${PELICAN_INCLUDE_DIR})
string(REPLACE / \\ PELICAN_LIB_DIR ${PELICAN_LIB_DIR})
string(REPLACE / \\ PELICAN_SRC_PUBLIC ${PELICAN_SRC_PUBLIC})
string(REPLACE / \\ Vulkan_LIBRARY_WINFIXED ${Vulkan_LIBRARY})
add_custom_command(
  TARGET pelican_core POST_BUILD
  COMMAND if not exist ${PELICAN_INCLUDE_DIR} mkdir ${PELICAN_INCLUDE_DIR}
  COMMAND copy ${PELICAN_SRC_PUBLIC} ${PELICAN_INCLUDE_DIR}
  COMMAND copy ${Vulkan_LIBRARY_WINFIXED} ${PELICAN_LIB_DIR})
else()
add_custom_command(
  TARGET pelican_core POST_BUILD
  COMMAND mkdir -p ${PELICAN_INCLUDE_DIR}
  COMMAND cp ${PELICAN_SRC_PUBLIC} ${PELICAN_INCLUDE_DIR}
  COMMAND cp ${Vulkan_LIBRARY} ${PELICAN_LIB_DIR})
endif()

FetchContent_Declare(
  vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.2.1
)
FetchContent_MakeAvailable(vma)
FetchContent_Declare(
  vma-hpp
  GIT_REPOSITORY https://github.com/YaaZ/VulkanMemoryAllocator-Hpp.git
  GIT_TAG        v3.2.1
)
FetchContent_MakeAvailable(vma-hpp)

target_include_directories(pelican_core PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(pelican_core PRIVATE ${Vulkan_LIBRARIES})
target_link_libraries(pelican_core PRIVATE VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_link_libraries(pelican_core PRIVATE glm)
