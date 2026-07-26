#include "debugtext.hpp"
#include "frameresources.hpp"

#include "../loader/engineresources.hpp"
#include "../loader/imageloader.hpp"
#include "../shader/pelican_sets.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/util.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican {

namespace {

constexpr uint32_t maxDebugTextScale = 64;

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    std::array<vk::DescriptorPoolSize, 2> pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 32},
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler, 32},
    };

    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = 32;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

vk::UniqueSampler createFontSampler(vk::Device device) {
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = vk::Filter::eNearest;
    create_info.minFilter = vk::Filter::eNearest;
    create_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    create_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    create_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    create_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    create_info.maxAnisotropy = 1.0f;
    create_info.anisotropyEnable = false;
    return device.createSamplerUnique(create_info);
}

vk::Format vkFormatForImage(ImagePixelFormat format) {
    switch (format) {
    case ImagePixelFormat::Rgba8Unorm:
        return vk::Format::eR8G8B8A8Unorm;
    case ImagePixelFormat::Rgba16Sfloat:
        return vk::Format::eR16G16B16A16Sfloat;
    case ImagePixelFormat::Rgba32Sfloat:
        return vk::Format::eR32G32B32A32Sfloat;
    }
    throw std::runtime_error("Unknown debug text image pixel format");
}

ImageWrapper createImageFromLoaded(const LoadedImage &loaded) {
    if (loaded.dimension != LoadedImageDimension::TwoD) {
        throw std::runtime_error(
            "DebugText atlas requires texture dimension 2d");
    }
    vk::Extent3D extent{loaded.width, loaded.height, 1};
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto image = vkcore.allocImage(extent, vkFormatForImage(loaded.format),
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferDevice, {});

    GET_MODULE(VulkanUtils).safeTransferMemoryToImage(
        image, loaded.pixels.data(), loaded.pixels.size(),
        VulkanUtils::ImageTransferInfo{
            .old_layout = vk::ImageLayout::eUndefined,
            .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .dst_stage = vk::PipelineStageFlagBits::eFragmentShader,
            .dst_access = vk::AccessFlagBits::eShaderRead,
        });
    return image;
}

vk::UniqueImageView createImageView(vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType = vk::ImageViewType::e2D;
    create_info.format = image.format;
    create_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    create_info.subresourceRange.baseMipLevel = 0;
    create_info.subresourceRange.levelCount = 1;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount = 1;
    return device.createImageViewUnique(create_info);
}

vk::DeviceSize nextCapacity(vk::DeviceSize required) {
    vk::DeviceSize capacity = sizeof(DebugTextVertex) * 64;
    while (capacity < required) {
        capacity *= 2;
    }
    return capacity;
}

std::span<const std::byte> bytesOf(const std::string &data) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte *>(data.data()), data.size()};
}

float clipX(double pixel, uint32_t width) {
    return static_cast<float>((pixel / static_cast<double>(width)) * 2.0 - 1.0);
}

float clipY(double pixel, uint32_t height) {
    return static_cast<float>((pixel / static_cast<double>(height)) * 2.0 - 1.0);
}

DebugTextVertex makeVertex(float x, float y, float u, float v, glm::vec4 color) {
    return DebugTextVertex{
        glm::vec4{x, y, 0.0f, 1.0f},
        color,
        glm::vec2{u, v},
        glm::vec2{0.0f},
    };
}

} // namespace

DebugText::DebugText() = default;
DebugText::~DebugText() = default;

void DebugText::ensureDevice() {
    if (!device) {
        device = GET_MODULE(VulkanManageCore).getDevice();
    }
}

void DebugText::ensureFontResources() {
    ensureDevice();
    if (font_loaded) {
        return;
    }

    font = ui::BitmapFont::bundledDebugFont();

    const auto png_data = engineResourceOrThrow("debug_text_font.png");
    const auto loaded = loadImageMemory(bytesOf(png_data), "engine://debug_text_font.png");
    if (loaded.width != font->atlasWidth() || loaded.height != font->atlasHeight()) {
        throw std::runtime_error("DebugText font atlas dimensions do not match coordinate table");
    }

    atlas_image = createImageFromLoaded(loaded);
    atlas_view = createImageView(device, atlas_image);
    atlas_sampler = createFontSampler(device);
    font_loaded = true;
}

void DebugText::ensureDescriptorPool() {
    ensureDevice();
    if (!descriptor_pool) {
        descriptor_pool = createDescriptorPool(device);
    }
}

void DebugText::ensureVertexCapacity(size_t vertex_count) {
    const auto required_bytes =
        static_cast<vk::DeviceSize>(vertex_count * sizeof(DebugTextVertex));
    if (required_bytes <= vertex_buffer_bytes) {
        return;
    }

    auto &vkcore = GET_MODULE(VulkanManageCore);
    vertex_buffer_bytes = nextCapacity(required_bytes);
    vertex_buffer = vkcore.allocBuf(vertex_buffer_bytes, vk::BufferUsageFlagBits::eStorageBuffer,
                                    vma::MemoryUsage::eAutoPreferHost,
                                    vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

void DebugText::ensureDescriptorSet(PassId pass_id, PipelineRecord &record) {
    if (record.descriptor_set) {
        return;
    }

    ensureDescriptorPool();
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto layout = pipeline_factory.descriptorSetLayout(record.pipeline, PELICAN_SET_FREE);

    vk::DescriptorSetAllocateInfo alloc_info;
    alloc_info.descriptorPool = descriptor_pool.get();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &layout;

    auto descriptor_sets = device.allocateDescriptorSetsUnique(alloc_info);
    if (descriptor_sets.empty()) {
        throw std::runtime_error("DebugText descriptor set allocation failed");
    }
    record.descriptor_set = std::move(descriptor_sets.front());
    (void)pass_id;
}

void DebugText::updateDescriptorSet(const PipelineRecord &record, vk::DeviceSize bytes) {
    vk::DescriptorBufferInfo buffer_info;
    buffer_info.buffer = vertex_buffer.buffer.get();
    buffer_info.offset = 0;
    buffer_info.range = bytes;

    vk::DescriptorImageInfo image_info;
    image_info.sampler = atlas_sampler.get();
    image_info.imageView = atlas_view.get();
    image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    std::array<vk::WriteDescriptorSet, 2> writes{};
    writes[0].dstSet = record.descriptor_set.get();
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[0].pBufferInfo = &buffer_info;

    writes[1].dstSet = record.descriptor_set.get();
    writes[1].dstBinding = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[1].pImageInfo = &image_info;

    device.updateDescriptorSets(writes, {});
}

void DebugText::buildVertices(vk::Extent2D target_extent) {
    vertices.clear();
    if (target_extent.width == 0 || target_extent.height == 0) {
        return;
    }

    for (const auto &queued : queued_glyphs) {
        const auto &glyph = queued.positioned;
        const auto scale = static_cast<int64_t>(std::max<uint32_t>(queued.scale, 1));
        const int64_t x0 = glyph.destination.left;
        const int64_t y0 = glyph.destination.top;
        const int64_t x1 = glyph.destination.right;
        const int64_t y1 = glyph.destination.bottom;

        const int64_t vx0 = std::clamp<int64_t>(x0, 0, target_extent.width);
        const int64_t vy0 = std::clamp<int64_t>(y0, 0, target_extent.height);
        const int64_t vx1 = std::clamp<int64_t>(x1, 0, target_extent.width);
        const int64_t vy1 = std::clamp<int64_t>(y1, 0, target_extent.height);
        if (vx0 >= vx1 || vy0 >= vy1) {
            continue;
        }

        const double src_x0 = glyph.source.left + static_cast<double>(vx0 - x0) / static_cast<double>(scale);
        const double src_y0 = glyph.source.top + static_cast<double>(vy0 - y0) / static_cast<double>(scale);
        const double src_x1 = glyph.source.left + static_cast<double>(vx1 - x0) / static_cast<double>(scale);
        const double src_y1 = glyph.source.top + static_cast<double>(vy1 - y0) / static_cast<double>(scale);

        const float u0 = static_cast<float>(src_x0 / static_cast<double>(font->atlasWidth()));
        const float v0 = static_cast<float>(src_y0 / static_cast<double>(font->atlasHeight()));
        const float u1 = static_cast<float>(src_x1 / static_cast<double>(font->atlasWidth()));
        const float v1 = static_cast<float>(src_y1 / static_cast<double>(font->atlasHeight()));
        const float cx0 = clipX(static_cast<double>(vx0), target_extent.width);
        const float cy0 = clipY(static_cast<double>(vy0), target_extent.height);
        const float cx1 = clipX(static_cast<double>(vx1), target_extent.width);
        const float cy1 = clipY(static_cast<double>(vy1), target_extent.height);

        vertices.push_back(makeVertex(cx0, cy0, u0, v0, queued.color));
        vertices.push_back(makeVertex(cx1, cy0, u1, v0, queued.color));
        vertices.push_back(makeVertex(cx0, cy1, u0, v1, queued.color));
        vertices.push_back(makeVertex(cx0, cy1, u0, v1, queued.color));
        vertices.push_back(makeVertex(cx1, cy0, u1, v0, queued.color));
        vertices.push_back(makeVertex(cx1, cy1, u1, v1, queued.color));
    }
}

PassId DebugText::registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                               ShaderBundleId frag_shader,
                               std::vector<std::string> shader_defines,
                               vk::SampleCountFlagBits samples) {
    ensureDevice();
    ensureFontResources();
    registration_order.reserve(registration_order.size() + 1);
    enabled = true;

    GraphicsPipelineDesc desc;
    desc.vert = vert_shader;
    desc.frag = frag_shader;
    desc.color_formats = {color_format};
    desc.shader_defines = std::move(shader_defines);
    desc.blend = true;
    desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
    desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.topology = vk::PrimitiveTopology::eTriangleList;
    desc.rasterization_samples = samples;

    if (next_pass_id == std::numeric_limits<int>::max()) {
        throw std::runtime_error(
            "DebugText pass handle table is exhausted");
    }
    const auto pass_id = PassId{next_pass_id++};
    if (!pipelines
             .emplace(
                 pass_id,
                 PipelineRecord{
                     GET_MODULE(PipelineFactory).create(desc),
                     {}})
             .second) {
        throw std::runtime_error(
            "DebugText pipeline table changed during registration");
    }
    registration_order.push_back(pass_id);
    return pass_id;
}

void DebugText::text(int x, int y, std::string_view value, glm::vec4 color, int scale) {
    if (!enabled || value.empty()) {
        return;
    }

    const auto safe_scale =
        std::clamp<uint32_t>(scale < 1 ? 1u : static_cast<uint32_t>(scale), 1u, maxDebugTextScale);
    for (auto positioned : font->layout(x, y, value, safe_scale)) {
        queued_glyphs.push_back(QueuedGlyph{std::move(positioned), safe_scale, color});
    }
}

void DebugText::clear() {
    queued_glyphs.clear();
    vertices.clear();
}

void DebugText::render(vk::CommandBuffer cmd_buf, PassId pass_id, vk::Extent2D target_extent,
                       const FrameResources &frame_resources) {
    if (!enabled || queued_glyphs.empty()) {
        return;
    }

    auto found = pipelines.find(pass_id);
    if (found == pipelines.end()) {
        queued_glyphs.clear();
        throw std::runtime_error("DebugText pass pipeline not found");
    }

    buildVertices(target_extent);
    queued_glyphs.clear();
    if (vertices.empty()) {
        return;
    }

    const auto bytes = static_cast<vk::DeviceSize>(vertices.size() * sizeof(DebugTextVertex));
    ensureVertexCapacity(vertices.size());
    ensureDescriptorSet(pass_id, found->second);
    GET_MODULE(VulkanManageCore).writeBuf(vertex_buffer, vertices.data(), 0, bytes);
    updateDescriptorSet(found->second, bytes);

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         pipeline_factory.pipeline(found->second.pipeline));
    frame_resources.bindGraphics(cmd_buf, pipeline_factory.layout(found->second.pipeline));
    cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                               pipeline_factory.layout(found->second.pipeline),
                               PELICAN_SET_FREE, found->second.descriptor_set.get(), {});
    cmd_buf.draw(static_cast<uint32_t>(vertices.size()), 1, 0, 0);
    vertices.clear();
}

DebugText::RegistrationCheckpoint
DebugText::checkpointRegistrations() const noexcept {
    return RegistrationCheckpoint{
        registration_order.size(), enabled, next_pass_id};
}

void DebugText::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "DebugText registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        pipelines.erase(registration_order.back());
        registration_order.pop_back();
    }
    enabled = checkpoint.enabled;
    next_pass_id = checkpoint.next_pass_id;
}

std::vector<PassId> DebugText::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "DebugText registration checkpoint is invalid");
    }
    return {
        registration_order.begin() +
            static_cast<std::ptrdiff_t>(
                checkpoint.registration_count),
        registration_order.end()};
}

void DebugText::retireRegistrations(
    const std::vector<PassId> &ids) noexcept {
    for (const auto id : ids) {
        const auto found = pipelines.find(id);
        if (found != pipelines.end()) {
            auto retired = std::move(found->second);
            pipelines.erase(found);
            try {
                auto *queue =
                    FastModuleContainer::tryGet<DeletionQueue>();
                if (queue != nullptr &&
                    queue->acceptingResources()) {
                    queue->defer(std::move(retired));
                }
            } catch (...) {
            }
        }
        std::erase(registration_order, id);
    }
    enabled = !registration_order.empty();
}

} // namespace Pelican
