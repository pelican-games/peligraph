#include "materialrender.hpp"
#include "../model/primitivebufcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../vkcore/core.hpp"
#include "battery/embed.hpp"

namespace Pelican {

static vk::UniquePipelineLayout createDefaultPipelineLayout(vk::Device device) {
    vk::PipelineLayoutCreateInfo create_info;
    create_info.setPushConstantRanges({});
    create_info.setSetLayouts({});
    return device.createPipelineLayoutUnique(create_info);
}

static vk::UniquePipeline createDefaultPipeline(vk::Device device, vk::PipelineLayout layout,
                                                vk::ShaderModule vert_shader, vk::ShaderModule frag_shader,
                                                VertBufContainer::CommonVertDataDescription input_descs) {
    vk::PipelineShaderStageCreateInfo vert_stage;
    vert_stage.stage = vk::ShaderStageFlagBits::eVertex;
    vert_stage.module = vert_shader;
    vert_stage.pName = "main";
    vk::PipelineShaderStageCreateInfo frag_stage;
    frag_stage.stage = vk::ShaderStageFlagBits::eFragment;
    frag_stage.module = frag_shader;
    frag_stage.pName = "main";

    const auto stages = {vert_stage, frag_stage};

    vk::PipelineVertexInputStateCreateInfo vertex_input_info;
    vertex_input_info.setVertexAttributeDescriptions(input_descs.attr_descs);
    vertex_input_info.setVertexBindingDescriptions(input_descs.binding_descs);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
    input_assembly.primitiveRestartEnable = false;

    vk::PipelineViewportStateCreateInfo viewport; // dynamic state, only count is specified
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization;
    rasterization.depthClampEnable = false;
    rasterization.rasterizerDiscardEnable = false;
    rasterization.polygonMode = vk::PolygonMode::eFill;
    rasterization.cullMode = vk::CullModeFlagBits::eBack;
    rasterization.frontFace = vk::FrontFace::eClockwise;
    rasterization.depthBiasEnable = false;
    rasterization.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample;
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
    multisample.sampleShadingEnable = false;

    vk::PipelineColorBlendAttachmentState blend_attachment;
    blend_attachment.blendEnable = false;
    blend_attachment.colorWriteMask = vk::ColorComponentFlagBits::eA | vk::ColorComponentFlagBits::eR |
                                      vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB;

    vk::PipelineColorBlendStateCreateInfo blend;
    blend.logicOpEnable = false;
    blend.setAttachments({blend_attachment});

    auto dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};

    vk::PipelineDynamicStateCreateInfo dynamic_state_info;
    dynamic_state_info.setDynamicStates(dynamic_states);

    vk::Format color_format = vk::Format::eB8G8R8A8Unorm;
    vk::PipelineRenderingCreateInfo rendering_info;
    rendering_info.setColorAttachmentFormats(color_format);
    // rendering_info.depthAttachmentFormat = vk::Format::eD32Sfloat;

    vk::GraphicsPipelineCreateInfo create_info;
    create_info.setStages(stages);
    create_info.pVertexInputState = &vertex_input_info;
    create_info.pInputAssemblyState = &input_assembly;
    // create_info.pTessellationState =
    create_info.pViewportState = &viewport;
    create_info.pRasterizationState = &rasterization;
    create_info.pMultisampleState = &multisample;
    // create_info.pDepthStencilState = ;
    create_info.pColorBlendState = &blend;
    create_info.pDynamicState = &dynamic_state_info;
    create_info.layout = layout;
    create_info.subpass = 0;

    vk::StructureChain create_info_chain{
        create_info,
        rendering_info,
    };

    auto result = device.createGraphicsPipelineUnique({}, create_info_chain.get());
    if (result.result != vk::Result::eSuccess) {
        throw std::runtime_error("failed on vkCreateGraphicsPipeline : " + vk::to_string(result.result));
    }
    return std::move(result.value);
}

static vk::UniqueShaderModule createShaderModule(vk::Device device, size_t len, const char *data) {
    vk::ShaderModuleCreateInfo create_info;
    create_info.codeSize = len;
    create_info.pCode = reinterpret_cast<const uint32_t *>(data);

    return device.createShaderModuleUnique(create_info);
}

MaterialRenderer::MaterialRenderer(DependencyContainer &_con)
    : con{_con}, pipeline_layout{createDefaultPipelineLayout(con.get<VulkanManageCore>().getDevice())} {
    const auto device = con.get<VulkanManageCore>().getDevice();

    // for test
    const auto vert_shader = b::embed<"test_simple.vert.spv">();
    shaders.insert({0, createShaderModule(device, vert_shader.length(), vert_shader.data())});

    const auto frag_shader = b::embed<"test_simple.frag.spv">();
    shaders.insert({1, createShaderModule(device, frag_shader.length(), frag_shader.data())});

    const auto &vert_buf_container = con.get<VertBufContainer>();
    pipelines.insert({0, createDefaultPipeline(device, pipeline_layout.get(), shaders[0].get(), shaders[1].get(),
                                               vert_buf_container.getDescription())});
    materials.insert({0, Material{.pipeline_id = 0}});
}

void MaterialRenderer::render(vk::CommandBuffer cmd_buf) const {
    const auto &primitive_buf_container = con.get<PrimitiveBufContainer>();
    const auto &vert_buf_container = con.get<VertBufContainer>();

    vert_buf_container.bindVertexBuffer(cmd_buf);

    for (const auto &[id, material] : materials) {
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines.at(material.pipeline_id).get());
        cmd_buf.draw(3, 1, 0, 0);

        // const auto indirect_draw_buf = primitive_buf_container.getIndirectDrawBuffer(/* TODO */);
        // const auto instance_buf = primitive_buf_container.getInstanceBuffer(/* TODO */);
        // const auto offset = 0; // TODO
        // const auto count = 0;  // TODO

        // cmd_buf.drawIndexedIndirect(indirect_draw_buf, offset, count, sizeof(vk::DrawIndexedIndirectCommand));
    }
}

} // namespace Pelican
