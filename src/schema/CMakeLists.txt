add_library(pelican_schema_leaf STATIC
    schema.cpp
    schema.hpp
)

set_target_properties(pelican_schema_leaf PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
target_include_directories(pelican_schema_leaf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pelican_schema_leaf PUBLIC nlohmann_json)

