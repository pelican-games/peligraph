b_embed_proxy_target(pelican_core pelican_resources)

function(embed_shader file_name)
    message(STATUS "embed shader code : ${file_name}")
    add_custom_command(
        COMMAND $ENV{VULKAN_SDK}/bin/glslang
            ${CMAKE_CURRENT_SOURCE_DIR}/${file_name}
            -V
            -o ${CMAKE_CURRENT_SOURCE_DIR}/${file_name}.spv
        DEPENDS ${file_name}
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/${file_name}.spv
    )
    b_embed(pelican_resources ${file_name}.spv)
endfunction(embed_shader)

embed_shader(default.vert)
embed_shader(default.frag)
embed_shader(fullscreen.vert)
embed_shader(fullscreen.frag)
embed_shader(ui.vert)
embed_shader(ui.frag)

b_embed(pelican_resources default_config.json)
