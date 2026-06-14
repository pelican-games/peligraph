target_sources(pelican_core PRIVATE
    shadercontainer.cpp
    shadercompiler.cpp
    shaderreflection.cpp
)
target_compile_definitions(pelican_core PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
target_link_libraries(pelican_core PRIVATE spirv-reflect-static)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_core PRIVATE pelican_shaderc)
endif()
