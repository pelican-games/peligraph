add_library(pelican_core log.cpp profiler.cpp job_system.cpp startup.cpp) # [BENCHMARK_ONLY] Added profiler.cpp

# settings
set_target_properties(pelican_core PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set_target_properties(pelican_core PROPERTIES CXX_STANDARD 20)
target_compile_definitions(pelican_core PUBLIC
  PELICAN_WITH_AUDIO=$<BOOL:${PELICAN_WITH_AUDIO}>
  PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
  PELICAN_WITH_EXR=$<BOOL:${PELICAN_WITH_EXR}>
  PELICAN_WITH_RPC=$<BOOL:${PELICAN_WITH_RPC}>
  PELICAN_WITH_SEQPLAYER=$<BOOL:${PELICAN_WITH_SEQPLAYER}>
  PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
  PELICAN_WITH_PHYSICS=$<BOOL:${PELICAN_WITH_PHYSICS}>
  PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
  PELICAN_WITH_RENDERDOC=$<BOOL:${PELICAN_WITH_RENDERDOC}>
  PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=$<BOOL:${PELICAN_WITH_STANDARD_RENDER_ALGORITHMS}>
)
target_compile_definitions(pelican_core PRIVATE
  PELICAN_WITH_BUILTIN_PHYSICS=$<BOOL:${PELICAN_WITH_BUILTIN_PHYSICS}>
  PELICAN_WITH_JOLT_PHYSICS=$<BOOL:${PELICAN_WITH_JOLT_PHYSICS}>
  PELICAN_ENGINE_BUILD=1
)

# modules
add_subdirectory(resources)
add_subdirectory(appflow)
add_subdirectory(audio)
add_subdirectory(communication)
add_subdirectory(renderdoc)
add_subdirectory(os)
add_subdirectory(watch)
if(PELICAN_WITH_IMGUI)
add_subdirectory(imgui)
endif()
add_subdirectory(ecs)
add_subdirectory(asset)
add_subdirectory(loader)
add_subdirectory(persistence)
add_subdirectory(vkcore)
add_subdirectory(shader)
add_subdirectory(material)
add_subdirectory(model)
add_subdirectory(animation)
add_subdirectory(playback)
add_subdirectory(renderingpass)
add_subdirectory(renderer)
add_subdirectory(fullscreenpass)
add_subdirectory(light)
add_subdirectory(phys)
add_subdirectory(ui)
add_subdirectory(gamelogic)
if(PELICAN_WITH_OPENXR)
add_subdirectory(openxr)
endif()

add_subdirectory(userpublic)
target_include_directories(pelican_core PUBLIC ./userpublic ${CMAKE_CURRENT_SOURCE_DIR})

# external libraries
target_link_libraries(pelican_core PUBLIC quill)
target_link_libraries(pelican_core PRIVATE pelican_project)
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
target_link_libraries(pelican_core PUBLIC glm)
if(PELICAN_WITH_EXR)
target_link_libraries(pelican_core PRIVATE tinyexr)
endif()
if(PELICAN_WITH_AUDIO)
target_link_libraries(pelican_core PRIVATE miniaudio)
endif()
if(PELICAN_WITH_JOLT_PHYSICS)
target_link_libraries(pelican_core PRIVATE Jolt)
endif()

target_link_libraries(pelican_core PUBLIC nlohmann_json)
target_link_libraries(pelican_core PRIVATE picosha2)

if(PELICAN_AUTO_STAGE_SDK)
  # SDK staging intentionally remains opt-in. A Debug pelican_core archive is
  # about 1 GiB, so copying it after every routine engine/test relink causes
  # substantial write amplification.

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
  add_custom_command(
    TARGET pelican_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/handle.hpp
            ${PELICAN_INCLUDE_DIR}/handle.hpp
  )
  add_custom_command(
    TARGET pelican_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PELICAN_INCLUDE_DIR}/phys
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/phys/physquery.hpp
            ${PELICAN_INCLUDE_DIR}/phys/physquery.hpp
  )
endif()
