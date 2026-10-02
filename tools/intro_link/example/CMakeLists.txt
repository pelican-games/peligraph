# IL-2: the intro link example -- a small Vulkan intro in two forms.
#   intro_link_example_dev  development form: connects to `pelican_cli intro-link serve`
#   intro_link_example      release form: the baked header, no C runtime, fixed flags
# The root CMakeLists adds this directory only with PELICAN_WITH_INTRO_LINK=ON.

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(STATUS "IL-2: intro link example skipped (needs 64-bit Windows and MSVC)")
  return()
endif()
if(NOT Vulkan_INCLUDE_DIR OR NOT Vulkan_LIBRARY OR NOT EXISTS "${Vulkan_INCLUDE_DIR}/vulkan/vulkan.h")
  message(STATUS "IL-2: intro link example skipped (Vulkan header or vulkan-1.lib not found)")
  return()
endif()
find_program(PELICAN_INTRO_LINK_GLSLC glslc
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT PELICAN_INTRO_LINK_GLSLC)
  message(STATUS "IL-2: intro link example skipped (glslc not found)")
  return()
endif()

# Nothing from the engine's directory scope or the tree's build type reaches
# these two targets: each one states its own flags below.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")
set_property(DIRECTORY PROPERTY LINK_OPTIONS "")
set(CMAKE_C_FLAGS "")
set(CMAKE_EXE_LINKER_FLAGS "")
set(CMAKE_C_STANDARD_LIBRARIES "")
foreach(config DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
  set(CMAKE_C_FLAGS_${config} "")
  set(CMAKE_EXE_LINKER_FLAGS_${config} "")
endforeach()

set(intro_link_header_dir "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(intro_link_project "${CMAKE_CURRENT_SOURCE_DIR}/project")
file(GLOB_RECURSE intro_link_project_files CONFIGURE_DEPENDS "${intro_link_project}/*")

# ---- development form: C runtime, sockets, rebuilds the pipeline on a new shader
add_executable(intro_link_example_dev intro.c)
target_include_directories(intro_link_example_dev PRIVATE ${intro_link_header_dir} ${Vulkan_INCLUDE_DIR})
target_compile_options(intro_link_example_dev PRIVATE /W3
  "$<$<CONFIG:Debug>:/MDd;/Od;/Zi;/RTC1>"
  "$<$<NOT:$<CONFIG:Debug>>:/MD;/O2>")
target_link_options(intro_link_example_dev PRIVATE /MACHINE:X64 /SUBSYSTEM:CONSOLE /INCREMENTAL:NO
  "$<$<CONFIG:Debug>:/DEBUG>")
target_link_libraries(intro_link_example_dev PRIVATE kernel32 user32 ws2_32 ${Vulkan_LIBRARY})

# ---- release form: the header bake writes, no C runtime
set(intro_link_baked_header "${CMAKE_CURRENT_BINARY_DIR}/intro_baked.h")
add_custom_command(
  OUTPUT "${intro_link_baked_header}"
  COMMAND $<TARGET_FILE:pelican_cli> intro-link bake "${intro_link_project}"
          --out "${intro_link_baked_header}" --glslc "${PELICAN_INTRO_LINK_GLSLC}"
  DEPENDS pelican_cli ${intro_link_project_files}
  COMMENT "IL-2: pelican_cli intro-link bake (example project)"
  VERBATIM
)
add_executable(intro_link_example intro.c "${intro_link_baked_header}")
target_compile_definitions(intro_link_example PRIVATE PELICAN_INTRO_LINK_BAKED)
target_include_directories(intro_link_example PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR} ${intro_link_header_dir} ${Vulkan_INCLUDE_DIR})
# The same flags in every build type (a Debug tree too).
target_compile_options(intro_link_example PRIVATE
  /O1 /Os /GS- /Gs9999999 /GR- /Gy /Gw /Zl /W3)
# IL-3 layout: .text (code, constants and .pdata), .data and .idata (which
# cannot be merged); .data stays apart so no section is both writable and
# executable. 16-byte section and file alignment, a 64-byte DOS stub
# (dos_stub.bin: "MZ", 4 header paragraphs, relocations at 0x40, the rest
# zero; link writes e_lfanew) and no Rich header. LNK4108 (/ALIGN without
# /DRIVER) is expected here.
set(intro_link_dos_stub "${CMAKE_CURRENT_SOURCE_DIR}/dos_stub.bin")
target_link_options(intro_link_example PRIVATE
  /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:WINDOWS /MACHINE:X64
  /OPT:REF /OPT:ICF /MERGE:.rdata=.text /MERGE:.pdata=.text
  /DYNAMICBASE:NO /FIXED
  /ALIGN:16 /FILEALIGN:16 /STUB:${intro_link_dos_stub} /EMITTOOLVERSIONINFO:NO /IGNORE:4108
  /INCREMENTAL:NO /DEBUG:NONE /MANIFEST:NO /EMITPOGOPHASEINFO)
set_property(TARGET intro_link_example APPEND PROPERTY LINK_DEPENDS "${intro_link_dos_stub}")
target_link_libraries(intro_link_example PRIVATE kernel32 user32 ${Vulkan_LIBRARY})
