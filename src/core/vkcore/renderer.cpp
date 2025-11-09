#include "renderer.hpp"
#include "../log.hpp"
#include "core.hpp"

namespace Pelican {

Renderer::Renderer(DependencyContainer &con)
    : rt{con.get<RenderTarget>()}, mat_renderer{con.get<MaterialRenderer>()},
      device{con.get<VulkanManageCore>().getDevice()} {}

Renderer::~Renderer() {}

void Renderer::render() {
    const auto render_ctx = rt.render_begin();

    const auto cmd_buf = render_ctx.cmd_buf;

    vk::RenderingAttachmentInfo color_attachment;
    color_attachment.imageView = render_ctx.color_attachment;
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.clearValue = vk::ClearValue{{0.0f, 0.0f, 0.0f, 0.0f}};
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;

    vk::RenderingInfo render_info;
    render_info.renderArea = vk::Rect2D{{0, 0}, render_ctx.extent};
    render_info.layerCount = 1;
    render_info.setColorAttachments(color_attachment);

    cmd_buf.beginRendering(render_info);

    mat_renderer.render(cmd_buf);

    cmd_buf.endRendering();

    rt.render_end();
}

} // namespace Pelican