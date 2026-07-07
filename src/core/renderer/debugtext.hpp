#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include <array>
#include <glm/glm.hpp>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct DebugTextVertex {
    glm::vec4 position;
    glm::vec4 color;
    glm::vec2 uv;
    glm::vec2 _pad;
};

DECLARE_MODULE(DebugText) {
    struct Glyph {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int advance = 0;
    };

    struct QueuedGlyph {
        uint32_t code = 0;
        int x = 0;
        int y = 0;
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
    uint32_t atlas_width = 0;
    uint32_t atlas_height = 0;
    uint32_t cell_width = 8;
    uint32_t cell_height = 16;
    std::array<Glyph, 95> glyphs{};

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
    void render(vk::CommandBuffer cmd_buf, PassId pass_id, vk::Extent2D target_extent);

    bool isEnabledForTesting() const { return enabled; }
    size_t queuedGlyphCountForTesting() const { return queued_glyphs.size(); }
};

} // namespace Pelican
