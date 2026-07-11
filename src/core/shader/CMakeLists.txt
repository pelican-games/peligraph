target_sources(pelican_core PRIVATE
    pipelinefactory.cpp
    shadercontainer.cpp
    shadercompiler.cpp
    shaderlibrary.cpp
    shaderreference.cpp
    shaderreflection.cpp
    spvlink.cpp
    surfacecompiler.cpp
)
if(PELICAN_USE_FETCHCONTENT_SHADERC)
    set(PELICAN_SHADERC_CACHE_VERSION "fetch-v2025.1")
else()
    set(PELICAN_SHADERC_CACHE_VERSION "vulkan-sdk-${Vulkan_VERSION}")
endif()
target_compile_definitions(pelican_core PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_SHADERC_VERSION="${PELICAN_SHADERC_CACHE_VERSION}"
)
target_link_libraries(pelican_core PRIVATE
    spirv-reflect-static
    SPIRV-Tools
    SPIRV-Tools-link
    SPIRV-Tools-opt
    picosha2
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_core PRIVATE pelican_shaderc)
endif()
