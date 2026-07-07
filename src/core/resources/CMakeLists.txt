b_embed_proxy_target(pelican_core pelican_resources)

function(embed_shader file_name)
    message(STATUS "embed shader code : ${file_name}")
    add_custom_command(
        COMMAND $ENV{VULKAN_SDK}/bin/glslang
            -I${CMAKE_CURRENT_SOURCE_DIR}/shaders/include
            ${CMAKE_CURRENT_SOURCE_DIR}/${file_name}
            -V
            -o ${CMAKE_CURRENT_SOURCE_DIR}/${file_name}.spv
        DEPENDS ${file_name} shaders/include/pelican_sets.glsl
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/${file_name}.spv
    )
    b_embed(pelican_resources ${file_name}.spv)
endfunction(embed_shader)

embed_shader(default.vert)
if(PELICAN_WITH_VAT)
embed_shader(vat.vert)
endif()
embed_shader(default.frag)
embed_shader(debug_draw.vert)
embed_shader(debug_draw.frag)
embed_shader(fullscreen.vert)
embed_shader(fullscreen.frag)
embed_shader(ui.vert)
embed_shader(ui.frag)
embed_shader(bloom_blur_h.frag)
embed_shader(bloom_blur_v.frag)
embed_shader(bloom_highpass.frag)
embed_shader(bloom_composite.frag)
embed_shader(ssao.frag)
embed_shader(ssao_blur.frag)
embed_shader(debug_texture.frag)
embed_shader(shader_lab_hello.frag)
embed_shader(shader_lab_base_lit.frag)
embed_shader(shader_lab_bloom_threshold.frag)
embed_shader(shader_lab_blur_h.frag)
embed_shader(shader_lab_blur_v.frag)
embed_shader(shader_lab_bloom_composite.frag)
embed_shader(shader_lab_present.frag)
embed_shader(shader_lab_gltf_lighting.frag)


b_embed(pelican_resources default_config.json)
b_embed(pelican_resources features/gpu_timing.json)
b_embed(pelican_resources features/debug_draw.json)
b_embed(pelican_resources features/hdr.json)
b_embed(pelican_resources tonemap.frag)
b_embed(pelican_resources tonemap.vert)
