set(PELICAN_LIB_DIR ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>/lib/bin)
set(PELICAN_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/dist$<$<CONFIG:Debug>:_debug>/lib/include)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PELICAN_LIB_DIR})
add_library(pelican_core log.cpp)
unset(CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
set_target_properties(pelican_core PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(pelican_core PROPERTIES CXX_STANDARD 20)

target_link_libraries(pelican_core PRIVATE quill)
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

# dependencies
get_target_property(PELICAN_DEPENDS_RAW pelican_core LINK_LIBRARIES)
foreach(DEP IN LISTS PELICAN_DEPENDS_RAW)
  if(TARGET ${DEP})
    get_target_property(DEPTYPE ${DEP} TYPE)
    if(${DEPTYPE} MATCHES "STATIC_LIBRARY")
      message(STATUS "pelican depdends: ${DEP}")
      list(APPEND PELICAN_DEPENDS $<TARGET_FILE:${DEP}>)
    endif()
  elseif(EXISTS ${DEP})
    list(APPEND PELICAN_DEPENDS ${DEP})
    message(STATUS "pelican depdends: ${DEP}")
  endif()
endforeach()

file(GLOB PELICAN_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/userpublic/*.hpp)

add_custom_command(
  TARGET pelican_core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PELICAN_INCLUDE_DIR} ${PELICAN_LIB_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PELICAN_PUBLIC_HEADERS} ${PELICAN_INCLUDE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PELICAN_DEPENDS} ${PELICAN_LIB_DIR}
  COMMAND_EXPAND_LISTS
)
