#include "uirenderer.hpp"

#include "frameresources.hpp"
#include "uicontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../ui/module.hpp"
#include "../ui/gpuabi.hpp"
#include "../vkcore/core.hpp"

#include "battery/embed.hpp"
#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace Pelican {
namespace {

GraphicsPipelineDesc makeUiPipelineDesc(ShaderBundleId vert, ShaderBundleId frag, vk::Format format) {
    GraphicsPipelineDesc desc;
    desc.vert = vert;
    desc.frag = frag;
    desc.color_formats = {format};
    desc.blend = true;
    desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.color_blend_op = vk::BlendOp::eAdd;
    desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
    desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.alpha_blend_op = vk::BlendOp::eAdd;
    const auto layout = ui::quadVertexLayout();
    desc.vertex_bindings = {layout.binding};
    desc.vertex_attributes.assign(layout.attributes.begin(), layout.attributes.end());
    return desc;
}

vk::DeviceSize nextCapacity(vk::DeviceSize required) {
    vk::DeviceSize result = 4096;
    while (result < required) result *= 2;
    return result;
}

} // namespace

UiRenderer::UiRenderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    const auto vert = b::embed<"ui.vert.spv">();
    const auto frag = b::embed<"ui.frag.spv">();
    if (vert.length() == 0 || frag.length() == 0) throw std::runtime_error("UI shaders not found");
    auto &library = GET_MODULE(ShaderLibrary);
    vert_shader = library.loadFromBytes(vert.length(), vert.data(), "ui.vert.spv");
    frag_shader = library.loadFromBytes(frag.length(), frag.data(), "ui.frag.spv");
}

UiRenderer::~UiRenderer() = default;

PipelineHandle UiRenderer::getPipeline(vk::Format color_format) {
    const auto key = static_cast<int>(color_format);
    if (const auto found = pipelines.find(key); found != pipelines.end()) return found->second;
    return pipelines.emplace(key, GET_MODULE(PipelineFactory).create(
                                      makeUiPipelineDesc(vert_shader, frag_shader, color_format))).first->second;
}

void UiRenderer::ensureBuffers(vk::DeviceSize vertex_bytes, vk::DeviceSize index_bytes) {
    auto &core = GET_MODULE(VulkanManageCore);
    if (vertex_bytes > vertex_capacity) {
        vertex_capacity = nextCapacity(vertex_bytes);
        vertex_buffer = core.allocBuf(vertex_capacity, vk::BufferUsageFlagBits::eVertexBuffer,
                                      vma::MemoryUsage::eAutoPreferHost,
                                      vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    }
    if (index_bytes > index_capacity) {
        index_capacity = nextCapacity(index_bytes);
        index_buffer = core.allocBuf(index_capacity, vk::BufferUsageFlagBits::eIndexBuffer,
                                     vma::MemoryUsage::eAutoPreferHost,
                                     vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
    }
}

void UiRenderer::render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request,
                        const UiRendererDependencies &dependencies) {
    const auto batch = dependencies.ui_module.buildFrame(request.target_extent, request.ui_scale);
    if (batch.indices.empty()) return;
    const auto vertex_bytes = static_cast<vk::DeviceSize>(batch.vertices.size() * sizeof(ui::QuadVertex));
    const auto index_bytes = static_cast<vk::DeviceSize>(batch.indices.size() * sizeof(std::uint16_t));
    ensureBuffers(vertex_bytes, index_bytes);
    auto &core = GET_MODULE(VulkanManageCore);
    core.writeBuf(vertex_buffer, batch.vertices.data(), 0, vertex_bytes);
    core.writeBuf(index_buffer, batch.indices.data(), 0, index_bytes);

    vk::RenderingAttachmentInfo attachment;
    attachment.imageView = request.target_view;
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = request.load_op;
    attachment.storeOp = request.store_op;
    attachment.clearValue.color = request.clear_color;
    vk::RenderingInfo rendering;
    rendering.renderArea = vk::Rect2D{{0, 0}, request.target_extent};
    rendering.layerCount = 1;
    rendering.setColorAttachments(attachment);
    cmd_buf.beginRendering(rendering);

    auto &factory = GET_MODULE(PipelineFactory);
    const auto pipeline = getPipeline(request.target_format);
    const auto layout = factory.layout(pipeline);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, factory.pipeline(pipeline));
    dependencies.frame_resources.bindGraphics(cmd_buf, layout);
    const vk::DeviceSize offset = 0;
    cmd_buf.bindVertexBuffers(0, vertex_buffer.buffer.get(), offset);
    cmd_buf.bindIndexBuffer(index_buffer.buffer.get(), 0, vk::IndexType::eUint16);
    cmd_buf.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(request.target_extent.width),
                                       static_cast<float>(request.target_extent.height), 0.0f, 1.0f});
    for (const auto &run : batch.runs) {
        const auto &r = run.key.scissor_px;
        cmd_buf.setScissor(0, vk::Rect2D{{r.left, r.top},
                                        {static_cast<std::uint32_t>(r.width()), static_cast<std::uint32_t>(r.height())}});
        const auto descriptor = dependencies.ui_container.descriptor(run.key.texture_page, run.key.sampler);
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, PELICAN_SET_PASS_INPUT,
                                   descriptor, {});
        cmd_buf.drawIndexed(run.index_count, 1, run.first_index, 0, 0);
    }
    cmd_buf.endRendering();
}

} // namespace Pelican
