cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS TARGET_NAME FIXTURE_SOURCE GAME_LOGIC_ENTRY
                          USERPUBLIC_INCLUDE CORE_INCLUDE NLOHMANN_INCLUDE
                          HOST_IMPORT_LIBRARY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

project(${TARGET_NAME} LANGUAGES CXX)

add_library(${TARGET_NAME} SHARED
    "${FIXTURE_SOURCE}"
    "${GAME_LOGIC_ENTRY}")
set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_compile_definitions(${TARGET_NAME} PRIVATE PELICAN_GAME_DLL=1)
target_include_directories(${TARGET_NAME} PRIVATE
    "${USERPUBLIC_INCLUDE}"
    "${CORE_INCLUDE}"
    "${CORE_INCLUDE}/../schema"
    "${NLOHMANN_INCLUDE}")
target_link_libraries(${TARGET_NAME} PRIVATE "${HOST_IMPORT_LIBRARY}")
