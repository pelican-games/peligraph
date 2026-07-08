cmake_minimum_required(VERSION 3.22)
project(pelican_spvlink_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)

add_executable(spvlink_verify tools/verify_pipeline.cpp)
target_link_libraries(spvlink_verify PRIVATE Vulkan::Vulkan)
