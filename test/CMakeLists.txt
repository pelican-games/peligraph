FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.11.0
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

function(pelican_define_test source_name)
set(target_name pelican_test_${source_name})
add_executable(${target_name} ${source_name}.cpp)
set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20)
target_include_directories(${target_name} PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(${target_name} PRIVATE Catch2::Catch2WithMain nlohmann_json ${ARGN})
catch_discover_tests(${target_name})
endfunction()

# register tests
pelican_define_test(hoge_test)
pelican_define_test(renderingpass_helpers_test pelican_core)
