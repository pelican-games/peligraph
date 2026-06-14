#include "uirenderer.hpp"
#include "uicontainer.hpp"

#include "../log.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../vkcore/core.hpp"
#include "battery/embed.hpp"
#include <glm/glm.hpp>
#include <stdexcept>

namespace Pelican {

namespace {

struct UiPushConstant {
    glm::vec3 pos;
    float _pad1;
    glm::vec2 size;
    float _pad2, _pad3;
};

static_assert(sizeof(UiPushConstant) <= PELICAN_PUSH_ENGINE_BYTES);

GraphicsPipelineDesc makeUiPipelineDesc(ShaderBundleId vert_shader, ShaderBundleId frag_shader,
                                        vk::Format color_format) {
    GraphicsPipelineDesc desc;
    desc.vert = vert_shader;
    desc.frag = frag_shader;
    desc.color_formats = {color_format};
    desc.blend = true;
    desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.color_blend_op = vk::BlendOp::eAdd;
    desc.src_alpha_blend_factor = vk::BlendFactor::eZero;
    desc.dst_alpha_blend_factor = vk::BlendFactor::eOne;
    desc.alpha_blend_op = vk::BlendOp::eAdd;
    return desc;
}

} // namespace

UiRenderer::UiRenderer()
    : device{GET_MODULE(VulkanManageCore).getDevice()} {
    auto &shader_library = GET_MODULE(ShaderLibrary);

    const auto vert = b::embed<"ui.vert.spv">();
    const auto frag = b::embed<"ui.frag.spv">();
    
    if (vert.length() == 0 || frag.length() == 0) {
        throw std::runtime_error("UI shaders not found");
    }
    
    vert_shader = shader_library.loadFromBytes(vert.length(), vert.data(), "ui.vert.spv");
    frag_shader = shader_library.loadFromBytes(frag.length(), frag.data(), "ui.frag.spv");
    
    LOG_INFO(logger, "UI Renderer initialized");
}

UiRenderer::~UiRenderer() {}

PipelineHandle UiRenderer::getPipeline(vk::Format color_format) {
    const auto key = static_cast<int>(color_format);
    if (auto it = pipelines.find(key); it != pipelines.end()) {
        return it->second;
    }

    auto pipeline = GET_MODULE(PipelineFactory).create(makeUiPipelineDesc(vert_shader, frag_shader, color_format));
    auto it = pipelines.emplace(key, std::move(pipeline)).first;
    return it->second;
}

void UiRenderer::render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request,
                        const UiRendererDependencies &dependencies) {
    const auto &ui_textures = dependencies.ui_container.getAllTextures();
    
    vk::RenderingAttachmentInfo attachment;
    attachment.imageView = request.target_view;
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = request.load_op;
    attachment.storeOp = request.store_op;
    attachment.clearValue.color = request.clear_color;

    vk::RenderingInfo rendering_info;
    rendering_info.renderArea = vk::Rect2D{{0, 0}, request.target_extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(attachment);

    cmd_buf.beginRendering(rendering_info);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline = getPipeline(request.target_format);
    const auto pipeline_layout = pipeline_factory.layout(pipeline);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(pipeline));

    for (const auto &entry : ui_textures) {
        const auto &tex = entry.second;
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

        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, PELICAN_SET_PASS_INPUT,
                                   {tex.descset.get()}, {});
        cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(UiPushConstant), &pc);
        cmd_buf.draw(6, 1, 0, 0);
    }

    cmd_buf.endRendering();
}

} // namespace Pelican
