add_library(pelican_core log.cpp profiler.cpp job_system.cpp) # [BENCHMARK_ONLY] Added profiler.cpp

# settings
set_target_properties(pelican_core PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(pelican_core PROPERTIES CXX_STANDARD 20)

# modules
add_subdirectory(resources)
add_subdirectory(appflow)
add_subdirectory(communication)
add_subdirectory(os)
add_subdirectory(ecs)
add_subdirectory(asset)
add_subdirectory(loader)
add_subdirectory(vkcore)
add_subdirectory(shader)
add_subdirectory(material)
add_subdirectory(model)
add_subdirectory(playback)
add_subdirectory(renderingpass)
add_subdirectory(renderer)
add_subdirectory(fullscreenpass)
add_subdirectory(light)

add_subdirectory(userpublic)
target_include_directories(pelican_core PUBLIC ./userpublic)

# external libraries
target_link_libraries(pelican_core PUBLIC quill)
# vma-hpp v3.3.0+3 (VMA 3.3.0 対応) 以降は例外送出を vulkan.hpp の
# vk::detail::resultCheck に委譲するため、Vulkan SDK 1.4 系の例外クラス改名
# (ValidationFailedEXTError → ValidationFailedError) の影響を受けない。
# また VMA 本体をサブモジュールとして同梱し VulkanMemoryAllocator ターゲットを
# 自前で定義するため、VMA を別途 FetchContent するとターゲット名が衝突する。
FetchContent_Declare(
  vma-hpp
  GIT_REPOSITORY https://github.com/YaaZ/VulkanMemoryAllocator-Hpp.git
  GIT_TAG        6e5ce659439afffa52094a7a5d54017a194935f1 # tag v3.3.0+3(タグは張り替え可能なのでハッシュで固定)
)
FetchContent_MakeAvailable(vma-hpp)

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
)
FetchContent_MakeAvailable(stb)

target_include_directories(pelican_core PRIVATE ${Vulkan_INCLUDE_DIRS})
target_include_directories(pelican_core PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(pelican_core PRIVATE
  "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
)
target_link_libraries(pelican_core PRIVATE ${Vulkan_LIBRARIES})
target_link_libraries(pelican_core PRIVATE VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_link_libraries(pelican_core PRIVATE glm)
target_link_libraries(pelican_core PRIVATE tinyexr)

target_link_libraries(pelican_core PRIVATE nlohmann_json)

# build distributable dir
## list up static lib dependencies
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

## copy library files
add_custom_command(
  TARGET pelican_core POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PELICAN_INCLUDE_DIR} ${PELICAN_LIB_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:pelican_core> ${PELICAN_LIB_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PELICAN_DEPENDS} ${PELICAN_LIB_DIR}
  COMMAND_EXPAND_LISTS
)

## list up public headers
function(pelican_copy_headers SRC_PATH DST_PATH)
  message(STATUS "pelican_copy_headers ${SRC_PATH} -> ${DST_PATH}")
  file(GLOB_RECURSE HEADERS ${SRC_PATH}/*.hpp)
  message(STATUS "headers: ${HEADERS}")
  foreach(HEADERFILE ${HEADERS})
    file(RELATIVE_PATH RPATH ${SRC_PATH} ${HEADERFILE})
    message(STATUS "header ${HEADERFILE} -> ${PELICAN_INCLUDE_DIR}/${DST_PATH}/${RPATH}")
    add_custom_command(
      TARGET pelican_core POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${HEADERFILE} ${PELICAN_INCLUDE_DIR}/${DST_PATH}/${RPATH}
    )
  endforeach()
endfunction()
pelican_copy_headers(${CMAKE_CURRENT_SOURCE_DIR}/userpublic .)
pelican_copy_headers(${CMAKE_CURRENT_SOURCE_DIR}/ecs ./ecs)
