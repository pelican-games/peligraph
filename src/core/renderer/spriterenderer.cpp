#include "spriterenderer.hpp"

#include "atlasassetresource.hpp"
#include "frameresources.hpp"
#include "spritedrawdata.hpp"
#include "spritegpuabi.hpp"
#include "spritescene.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../vkcore/core.hpp"

#include "battery/embed.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Pelican {
namespace {

GraphicsPipelineDesc makeSpritePipelineDesc(ShaderBundleId vert, ShaderBundleId frag,
                                            vk::Format color, vk::Format depth) {
    GraphicsPipelineDesc desc;
    desc.vert = vert;
    desc.frag = frag;
    desc.color_formats = {color};
    desc.depth_format = depth;
    desc.depth_test = true;
    desc.depth_write = false;
    desc.cull_mode = vk::CullModeFlagBits::eNone;
    desc.blend = true;
    desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.color_blend_op = vk::BlendOp::eAdd;
    desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
    desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.alpha_blend_op = vk::BlendOp::eAdd;
    const auto layout = sprite::gpuVertexLayout();
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

namespace renderer_detail {

SpriteDrawData buildSpriteDrawData(const sprite::SpriteFrame &frame) {
    SpriteDrawData result;
    result.vertices.reserve(frame.visible_count * 4);
    result.indices.reserve(frame.visible_count * 6);
    static constexpr std::array<std::array<float, 2>, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (const auto &chunk : frame.chunks) {
        const auto chunk_first_index = static_cast<std::uint32_t>(result.indices.size());
        const auto chunk_vertex_offset = static_cast<std::int32_t>(result.vertices.size());
        result.indices.insert(result.indices.end(), chunk.indices.begin(), chunk.indices.end());
        for (std::size_t command_index = 0; command_index < chunk.commands.size(); ++command_index) {
            const auto &command = chunk.commands[command_index];
            const auto first_index =
                chunk_first_index + static_cast<std::uint32_t>(command_index * 6);
            if (result.runs.empty() || result.runs.back().key != command.batch ||
                result.runs.back().vertex_offset != chunk_vertex_offset ||
                result.runs.back().first_index + result.runs.back().index_count != first_index) {
                result.runs.push_back({command.batch, first_index, 0, chunk_vertex_offset});
            }
            result.runs.back().index_count += 6;
            for (std::size_t vertex = 0; vertex < 4; ++vertex) {
                sprite::GpuVertex gpu;
                gpu.corner = {corners[vertex][0] - command.pivot[0],
                              corners[vertex][1] - command.pivot[1]};
                const bool right = vertex == 1 || vertex == 2;
                const bool bottom = vertex >= 2;
                gpu.uv = {command.uv_rect[right ? 2 : 0], command.uv_rect[bottom ? 3 : 1]};
                gpu.world = command.world_transform;
                gpu.color = command.color;
                gpu.billboard = static_cast<std::uint32_t>(command.billboard);
                gpu.snap_anchor = {-command.pivot[0], -command.pivot[1]};
                gpu.pixel_snap = command.pixel_snap == sprite::PixelSnapReason::eligible ? 1u : 0u;
                result.vertices.push_back(gpu);
            }
        }
    }
    return result;
}

} // namespace renderer_detail

SpriteRenderer::SpriteRenderer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    const auto vert = b::embed<"sprite.vert.spv">();
    const auto frag = b::embed<"sprite.frag.spv">();
    if (vert.length() == 0 || frag.length() == 0) throw std::runtime_error("sprite shaders not found");
    auto &library = GET_MODULE(ShaderLibrary);
    vert_shader = library.loadFromBytes(vert.length(), vert.data(), "sprite.vert.spv");
    frag_shader = library.loadFromBytes(frag.length(), frag.data(), "sprite.frag.spv");
}

SpriteRenderer::~SpriteRenderer() = default;

PipelineHandle SpriteRenderer::getPipeline(vk::Format color_format, vk::Format depth_format) {
    const auto key = (std::uint64_t{static_cast<std::uint32_t>(color_format)} << 32) |
                     std::uint32_t(depth_format);
    if (const auto found = pipelines.find(key); found != pipelines.end()) return found->second;
    return pipelines.emplace(key, GET_MODULE(PipelineFactory).create(
                                      makeSpritePipelineDesc(vert_shader, frag_shader,
                                                             color_format, depth_format))).first->second;
}

void SpriteRenderer::ensureBuffers(vk::DeviceSize vertex_bytes, vk::DeviceSize index_bytes) {
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

void SpriteRenderer::render(vk::CommandBuffer cmd_buf, const SpriteDrawRequest &request,
                            const SpriteRendererDependencies &dependencies) {
    dependencies.scene.updatePixelPolicy(request.extent.width, request.extent.height);
    const auto &frame = dependencies.scene.frame();
    if (frame.visible_count == 0) return;

    const auto draw_data = renderer_detail::buildSpriteDrawData(frame);

    const auto vertex_bytes =
        static_cast<vk::DeviceSize>(draw_data.vertices.size() * sizeof(sprite::GpuVertex));
    const auto index_bytes =
        static_cast<vk::DeviceSize>(draw_data.indices.size() * sizeof(std::uint16_t));
    ensureBuffers(vertex_bytes, index_bytes);
    auto &core = GET_MODULE(VulkanManageCore);
    core.writeBuf(vertex_buffer, draw_data.vertices.data(), 0, vertex_bytes);
    core.writeBuf(index_buffer, draw_data.indices.data(), 0, index_bytes);

    vk::RenderingAttachmentInfo color;
    color.imageView = request.color_view;
    color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color.loadOp = vk::AttachmentLoadOp::eLoad;
    color.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingAttachmentInfo depth;
    depth.imageView = request.depth_view;
    depth.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    depth.loadOp = vk::AttachmentLoadOp::eLoad;
    depth.storeOp = vk::AttachmentStoreOp::eStore;
    vk::RenderingInfo rendering;
    rendering.renderArea = vk::Rect2D{{0, 0}, request.extent};
    rendering.layerCount = 1;
    rendering.setColorAttachments(color);
    rendering.pDepthAttachment = &depth;
    cmd_buf.beginRendering(rendering);

    auto &factory = GET_MODULE(PipelineFactory);
    const auto pipeline = getPipeline(request.color_format, request.depth_format);
    const auto layout = factory.layout(pipeline);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, factory.pipeline(pipeline));
    dependencies.frame_resources.bindGraphics(cmd_buf, layout);
    const vk::DeviceSize offset = 0;
    cmd_buf.bindVertexBuffers(0, vertex_buffer.buffer.get(), offset);
    cmd_buf.bindIndexBuffer(index_buffer.buffer.get(), 0, vk::IndexType::eUint16);
    cmd_buf.setViewport(0, vk::Viewport{0.0f, 0.0f, static_cast<float>(request.extent.width),
                                       static_cast<float>(request.extent.height), 0.0f, 1.0f});
    cmd_buf.setScissor(0, vk::Rect2D{{0, 0}, request.extent});
    for (const auto &run : draw_data.runs) {
        const auto descriptor = dependencies.atlas.descriptor(run.key.texture_page, run.key.sampler);
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, PELICAN_SET_PASS_INPUT,
                                   descriptor, {});
        cmd_buf.drawIndexed(run.index_count, 1, run.first_index, run.vertex_offset, 0);
    }
    cmd_buf.endRendering();
}

} // namespace Pelican
