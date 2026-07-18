# Dear ImGui is an engine-development unit, not an SDK/system dependency. Keep
# the exact upstream revision reviewable and reproducible.
FetchContent_Declare(
  imgui_vendor
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        f5befd2d29e66809cd1110a152e375a7f1981f06 # v1.91.9b
)
FetchContent_GetProperties(imgui_vendor)
if(NOT imgui_vendor_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(imgui_vendor)
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()
endif()

add_library(pelican_imgui OBJECT
  assetbrowser.cpp
  imguiruntime.cpp
  imguisystem.cpp
  planviewer.cpp
  ${imgui_vendor_SOURCE_DIR}/imgui.cpp
  ${imgui_vendor_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_vendor_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_vendor_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_vendor_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_vendor_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_vendor_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
set_target_properties(pelican_imgui PROPERTIES
  CXX_STANDARD 20
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_include_directories(pelican_imgui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../userpublic
  ${imgui_vendor_SOURCE_DIR}
  ${imgui_vendor_SOURCE_DIR}/backends
  ${Vulkan_INCLUDE_DIRS}
)
target_compile_definitions(pelican_imgui PRIVATE
  PELICAN_WITH_IMGUI=1
  "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
)
target_link_libraries(pelican_imgui PRIVATE
  glfw
  quill
  glm
  nlohmann_json
  VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp
)

target_sources(pelican_core PRIVATE $<TARGET_OBJECTS:pelican_imgui>)
