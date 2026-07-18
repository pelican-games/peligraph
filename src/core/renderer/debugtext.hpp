#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include "../ui/bitmapfont.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FrameResources;

struct DebugTextVertex {
    glm::vec4 position;
    glm::vec4 color;
    glm::vec2 uv;
    glm::vec2 _pad;
};

DECLARE_MODULE(DebugText) {
    struct QueuedGlyph {
        ui::PositionedGlyph positioned;
        uint32_t scale = 1;
        glm::vec4 color{1.0f};
    };

    struct PipelineRecord {
        PipelineHandle pipeline;
        vk::UniqueDescriptorSet descriptor_set;
    };

    vk::Device device;
    bool enabled = false;
    bool font_loaded = false;
    std::optional<ui::BitmapFont> font;

    vk::UniqueDescriptorPool descriptor_pool;
    vk::UniqueSampler atlas_sampler;
    ImageWrapper atlas_image;
    vk::UniqueImageView atlas_view;
    BufferWrapper vertex_buffer;
    vk::DeviceSize vertex_buffer_bytes = 0;

    std::vector<QueuedGlyph> queued_glyphs;
    std::vector<DebugTextVertex> vertices;
    std::unordered_map<PassId, PipelineRecord, PassId::Hash> pipelines;

    void ensureDevice();
    void ensureFontResources();
    void ensureDescriptorPool();
    void ensureVertexCapacity(size_t vertex_count);
    void ensureDescriptorSet(PassId pass_id, PipelineRecord &record);
    void updateDescriptorSet(const PipelineRecord &record, vk::DeviceSize bytes);
    void buildVertices(vk::Extent2D target_extent);

  public:
    DebugText();
    ~DebugText();

    PassId registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                        ShaderBundleId frag_shader, std::vector<std::string> shader_defines = {});
    void text(int x, int y, std::string_view value,
              glm::vec4 color = glm::vec4{1.0f}, int scale = 1);
    void clear();
    void render(vk::CommandBuffer cmd_buf, PassId pass_id, vk::Extent2D target_extent,
                const FrameResources &frame_resources);

    bool isEnabledForTesting() const { return enabled; }
    size_t queuedGlyphCountForTesting() const { return queued_glyphs.size(); }
};

} // namespace Pelican
