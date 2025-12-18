#include "uirenderer.hpp"
#include "uicontainer.hpp"

#include "../log.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../shader/shadercontainer.hpp"
#include "battery/embed.hpp"
#include <array>
#include <glm/glm.hpp>

namespace Pelican {

namespace {

struct UiPushConstant {
    glm::vec3 pos;
    float _pad1;
    glm::vec2 size;
    float _pad2, _pad3;
};

vk::UniquePipelineLayout createPipelineLayout(vk::Device device, vk::DescriptorSetLayout descset_layout) {
    vk::PushConstantRange push_const_range;
    push_const_range.stageFlags = vk::ShaderStageFlagBits::eVertex;
    push_const_range.offset = 0;
    push_const_range.size = sizeof(UiPushConstant);

    vk::PipelineLayoutCreateInfo ci;
    ci.setSetLayouts(descset_layout);
    ci.setPushConstantRanges(push_const_range);
    return device.createPipelineLayoutUnique(ci);
}

vk::UniquePipeline createPipeline(vk::Device device, vk::PipelineLayout layout, vk::ShaderModule vert_shader,
                                  vk::ShaderModule frag_shader, vk::Format color_format) {
    vk::PipelineShaderStageCreateInfo vert_stage;
    vert_stage.stage = vk::ShaderStageFlagBits::eVertex;
    vert_stage.module = vert_shader;
    vert_stage.pName = "main";

    vk::PipelineShaderStageCreateInfo frag_stage;
    frag_stage.stage = vk::ShaderStageFlagBits::eFragment;
    frag_stage.module = frag_shader;
    frag_stage.pName = "main";

    const std::array stages{vert_stage, frag_stage};

    vk::PipelineVertexInputStateCreateInfo vertex_input;

    vk::PipelineInputAssemblyStateCreateInfo input_asm;
    input_asm.topology = vk::PrimitiveTopology::eTriangleList;
    input_asm.primitiveRestartEnable = false;

    vk::PipelineViewportStateCreateInfo viewport_state;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo raster;
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eNone;
    raster.frontFace = vk::FrontFace::eCounterClockwise;
    raster.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo msaa;
    msaa.rasterizationSamples = vk::SampleCountFlagBits::e1;
    msaa.minSampleShading = 1.0f;

    vk::PipelineDepthStencilStateCreateInfo depth;
    depth.depthTestEnable = false;
    depth.depthWriteEnable = false;

    vk::PipelineColorBlendAttachmentState blend_att;
    blend_att.blendEnable = true;
    blend_att.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blend_att.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blend_att.colorBlendOp = vk::BlendOp::eAdd;
    blend_att.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blend_att.dstAlphaBlendFactor = vk::BlendFactor::eZero;
    blend_att.alphaBlendOp = vk::BlendOp::eAdd;
    blend_att.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo blend;
    blend.logicOpEnable = false;
    blend.setAttachments(blend_att);

    const std::array dyn_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dyn_state;
    dyn_state.setDynamicStates(dyn_states);

    vk::PipelineRenderingCreateInfo rendering;
    rendering.setColorAttachmentFormats(color_format);

    vk::GraphicsPipelineCreateInfo ci;
    ci.setStages(stages);
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &input_asm;
    ci.pViewportState = &viewport_state;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &msaa;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dyn_state;
    ci.layout = layout;

    vk::StructureChain chain{ci, rendering};

    auto result = device.createGraphicsPipelineUnique({}, chain.get());
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed to create UI pipeline");
    }
    return std::move(result.value);
}

} // namespace

UiRenderer::UiRenderer()
    : device{GET_MODULE(VulkanManageCore).getDevice()} {
    
    auto &ui_container = GET_MODULE(UIContainer);
    auto &shader_con = GET_MODULE(ShaderContainer);

    const auto vert = b::embed<"ui.vert.spv">();
    const auto frag = b::embed<"ui.frag.spv">();
    
    if (vert.length() == 0 || frag.length() == 0) {
        throw std::runtime_error("UI shaders not found");
    }
    
    const auto vert_shader = shader_con.registerShader(vert.length(), vert.data());
    const auto frag_shader = shader_con.registerShader(frag.length(), frag.data());

    const auto color_format = GET_MODULE(RenderTarget).getSwapchainFormat();
    pipeline_layout = createPipelineLayout(device, ui_container.getDescriptorSetLayout());
    pipeline = createPipeline(device, pipeline_layout.get(), shader_con.getShader(vert_shader),
                              shader_con.getShader(frag_shader), color_format);
    
    LOG_INFO(logger, "UI Renderer initialized");
}

UiRenderer::~UiRenderer() {}

void UiRenderer::render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request) const {
    auto &ui_container = GET_MODULE(UIContainer);
    const auto &ui_textures = ui_container.getAllTextures();
    
    vk::RenderingAttachmentInfo attachment;
    attachment.imageView = request.target_view;
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = vk::AttachmentLoadOp::eLoad;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingInfo rendering_info;
    rendering_info.renderArea = vk::Rect2D{{0, 0}, request.target_extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(attachment);

    cmd_buf.beginRendering(rendering_info);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get());

    for (const auto &[name, tex] : ui_textures) {
        const float aspect_ratio = static_cast<float>(tex.pixel_size.width) / 
                                  static_cast<float>(tex.pixel_size.height);
        const float target_aspect = static_cast<float>(request.target_extent.width) /
                                   static_cast<float>(request.target_extent.height);

        glm::vec2 normalized_size;
        normalized_size.x = (static_cast<float>(tex.pixel_size.width) / 
                           static_cast<float>(request.target_extent.width)) * tex.scale;
        normalized_size.y = (static_cast<float>(tex.pixel_size.height) / 
                           static_cast<float>(request.target_extent.height)) * tex.scale;

        glm::vec3 adjusted_pos = tex.position;
        adjusted_pos.x -= normalized_size.x * tex.center.x;
        adjusted_pos.y -= normalized_size.y * tex.center.y;

        UiPushConstant pc;
        pc.pos = adjusted_pos;
        pc.size = normalized_size;

        vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(request.target_extent.width),
                             static_cast<float>(request.target_extent.height), 0.0f, 1.0f};
        cmd_buf.setViewport(0, viewport);
        
        vk::Rect2D scissor{{0, 0}, request.target_extent};
        cmd_buf.setScissor(0, scissor);

        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(), 0,
                                   {tex.descset.get()}, {});
        cmd_buf.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(UiPushConstant), &pc);
        cmd_buf.draw(6, 1, 0, 0);
    }

    cmd_buf.endRendering();
}

} // namespace Pelican