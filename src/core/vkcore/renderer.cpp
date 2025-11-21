#include "renderer.hpp"
#include "../log.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/uirenderer.hpp"
#include "core.hpp"
#include "rendertarget.hpp"

namespace Pelican {

Renderer::Renderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {}

Renderer::~Renderer() {}

void Renderer::render() {
    auto &rt = GET_MODULE(RenderTarget);
    auto &mat_renderer = GET_MODULE(MaterialRenderer);
    auto &ui_renderer = GET_MODULE(UiRenderer);

    const auto render_ctx = rt.render_begin();

    const auto cmd_buf = render_ctx.cmd_buf;

    vk::RenderingAttachmentInfo color_attachment;
    color_attachment.imageView = render_ctx.color_attachment;
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.clearValue = vk::ClearColorValue{0.0f, 0.0f, 0.0f, 0.0f};
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingAttachmentInfo depth_attachment;
    depth_attachment.imageView = render_ctx.depth_attachment;
    depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth_attachment.clearValue = vk::ClearDepthStencilValue{1.0};
    depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eDontCare;

    vk::RenderingInfo render_info;
    render_info.renderArea = vk::Rect2D{{0, 0}, render_ctx.extent};
    render_info.layerCount = 1;
    render_info.setColorAttachments(color_attachment);
    render_info.pDepthAttachment = &depth_attachment;

    cmd_buf.beginRendering(render_info);

    mat_renderer.render(cmd_buf);
    ui_renderer.render(cmd_buf);

    cmd_buf.endRendering();

    rt.render_end();
}

} // namespace Pelican