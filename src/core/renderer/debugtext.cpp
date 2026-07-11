#include "debugtext.hpp"
#include "frameresources.hpp"

#include "../loader/engineresources.hpp"
#include "../loader/imageloader.hpp"
#include "../shader/pelican_sets.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican {

namespace {

constexpr uint32_t firstPrintableCode = 32;
constexpr uint32_t lastPrintableCode = 126;
constexpr uint32_t fallbackCode = static_cast<uint32_t>('?');
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

uint32_t normalizedCode(unsigned char ch) {
    const auto code = static_cast<uint32_t>(ch);
    if (code < firstPrintableCode || code > lastPrintableCode) {
        return fallbackCode;
    }
    return code;
}

size_t glyphIndex(uint32_t code) {
    if (code < firstPrintableCode || code > lastPrintableCode) {
        code = fallbackCode;
    }
    return static_cast<size_t>(code - firstPrintableCode);
}

int requireIntField(const nlohmann::json &json, const std::string &field_name,
                    const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_number_integer()) {
        throw std::runtime_error(context + " requires integer field: " + field_name);
    }
    return json.at(field_name).get<int>();
}

uint32_t requireUintField(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context) {
    const auto value = requireIntField(json, field_name, context);
    if (value < 0) {
        throw std::runtime_error(context + " requires non-negative field: " + field_name);
    }
    return static_cast<uint32_t>(value);
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

    const auto table = nlohmann::json::parse(engineResourceOrThrow("debug_text_font.json"));
    if (table.value("schema", std::string{}) != "pelican.debug_text_font" ||
        table.value("version", 0) != 1) {
        throw std::runtime_error("DebugText font table schema is not supported");
    }

    atlas_width = requireUintField(table, "atlas_width", "DebugText font table");
    atlas_height = requireUintField(table, "atlas_height", "DebugText font table");
    cell_width = requireUintField(table, "cell_width", "DebugText font table");
    cell_height = requireUintField(table, "cell_height", "DebugText font table");

    const auto first_code = requireUintField(table, "first_code", "DebugText font table");
    const auto last_code = requireUintField(table, "last_code", "DebugText font table");
    if (first_code != firstPrintableCode || last_code != lastPrintableCode) {
        throw std::runtime_error("DebugText font table must contain ASCII 32..126");
    }
    if (!table.contains("glyphs") || !table.at("glyphs").is_array()) {
        throw std::runtime_error("DebugText font table requires glyphs array");
    }
    for (const auto &glyph_json : table.at("glyphs")) {
        const auto code = requireUintField(glyph_json, "code", "DebugText glyph");
        if (code < firstPrintableCode || code > lastPrintableCode) {
            throw std::runtime_error("DebugText glyph code is outside ASCII 32..126");
        }
        glyphs[glyphIndex(code)] = Glyph{
            requireIntField(glyph_json, "x", "DebugText glyph"),
            requireIntField(glyph_json, "y", "DebugText glyph"),
            requireIntField(glyph_json, "w", "DebugText glyph"),
            requireIntField(glyph_json, "h", "DebugText glyph"),
            requireIntField(glyph_json, "advance", "DebugText glyph"),
        };
    }

    const auto png_data = engineResourceOrThrow("debug_text_font.png");
    const auto loaded = loadImageMemory(bytesOf(png_data), "engine://debug_text_font.png");
    if (loaded.width != atlas_width || loaded.height != atlas_height) {
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
        const auto &glyph = glyphs[glyphIndex(queued.code)];
        const auto scale = static_cast<int64_t>(std::max<uint32_t>(queued.scale, 1));
        const int64_t x0 = queued.x;
        const int64_t y0 = queued.y;
        const int64_t x1 = x0 + static_cast<int64_t>(glyph.w) * scale;
        const int64_t y1 = y0 + static_cast<int64_t>(glyph.h) * scale;

        const int64_t vx0 = std::clamp<int64_t>(x0, 0, target_extent.width);
        const int64_t vy0 = std::clamp<int64_t>(y0, 0, target_extent.height);
        const int64_t vx1 = std::clamp<int64_t>(x1, 0, target_extent.width);
        const int64_t vy1 = std::clamp<int64_t>(y1, 0, target_extent.height);
        if (vx0 >= vx1 || vy0 >= vy1) {
            continue;
        }

        const double src_x0 = glyph.x + static_cast<double>(vx0 - x0) / static_cast<double>(scale);
        const double src_y0 = glyph.y + static_cast<double>(vy0 - y0) / static_cast<double>(scale);
        const double src_x1 = glyph.x + static_cast<double>(vx1 - x0) / static_cast<double>(scale);
        const double src_y1 = glyph.y + static_cast<double>(vy1 - y0) / static_cast<double>(scale);

        const float u0 = static_cast<float>(src_x0 / static_cast<double>(atlas_width));
        const float v0 = static_cast<float>(src_y0 / static_cast<double>(atlas_height));
        const float u1 = static_cast<float>(src_x1 / static_cast<double>(atlas_width));
        const float v1 = static_cast<float>(src_y1 / static_cast<double>(atlas_height));
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
                               ShaderBundleId frag_shader, std::vector<std::string> shader_defines) {
    ensureDevice();
    ensureFontResources();
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

    const auto pass_id = PassId{static_cast<int>(pipelines.size())};
    pipelines.emplace(pass_id, PipelineRecord{GET_MODULE(PipelineFactory).create(desc), {}});
    return pass_id;
}

void DebugText::text(int x, int y, std::string_view value, glm::vec4 color, int scale) {
    if (!enabled || value.empty()) {
        return;
    }

    const auto safe_scale =
        std::clamp<uint32_t>(scale < 1 ? 1u : static_cast<uint32_t>(scale), 1u, maxDebugTextScale);
    int cursor_x = x;
    int cursor_y = y;
    const int line_height = static_cast<int>(cell_height * safe_scale);

    for (const auto raw_ch : value) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += line_height;
            continue;
        }
        if (ch == '\t') {
            cursor_x += static_cast<int>(cell_width * safe_scale * 4);
            continue;
        }

        const auto code = normalizedCode(ch);
        const auto &glyph = glyphs[glyphIndex(code)];
        if (code != static_cast<uint32_t>(' ')) {
            queued_glyphs.push_back(QueuedGlyph{code, cursor_x, cursor_y, safe_scale, color});
        }
        cursor_x += glyph.advance * static_cast<int>(safe_scale);
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

} // namespace Pelican
