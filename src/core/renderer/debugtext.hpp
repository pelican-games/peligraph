#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/image.hpp"
#include "../ui/bitmapfont.hpp"
#include "lineoverlaypassregistry.hpp"
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>
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

    // The device, descriptor pool, vertex buffer and pass table are the same
    // plumbing DebugDraw and Gizmo need. The font atlas below is ours, and so
    // is the glyph-to-vertex step. The UI quad renderer stays a separate path
    // on purpose (2026-07-17 debt review); only this plumbing is shared.
    LineOverlayPassRegistry passes{LineOverlayRegistryTraits{
        .label = "DebugText",
        .vertex_stride = sizeof(DebugTextVertex),
        .initial_vertex_count = 64,
        .contract = AuxiliaryDescriptorContract::text_atlas,
        .topology = vk::PrimitiveTopology::eTriangleList,
        .sampled_image = true,
        .tracks_enabled = true,
    }};

    bool font_loaded = false;
    std::optional<ui::BitmapFont> font;
    vk::UniqueSampler atlas_sampler;
    ImageWrapper atlas_image;
    vk::UniqueImageView atlas_view;

    std::vector<QueuedGlyph> queued_glyphs;
    std::vector<DebugTextVertex> vertices;

    void ensureFontResources();
    void buildVertices(vk::Extent2D target_extent);

  public:
    using RegistrationCheckpoint = LineOverlayPassRegistry::Checkpoint;

    DebugText();
    ~DebugText();

    PassId registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                        ShaderBundleId frag_shader,
                        std::vector<std::string> shader_defines = {},
                        vk::SampleCountFlagBits samples =
                            vk::SampleCountFlagBits::e1,
                        bool ray_query = false,
                        std::string acceleration_structure = "scene");
    void text(int x, int y, std::string_view value,
              glm::vec4 color = glm::vec4{1.0f}, int scale = 1);
    void clear();
    void render(vk::CommandBuffer cmd_buf, PassId pass_id, vk::Extent2D target_extent,
                const FrameResources &frame_resources);

    bool isEnabledForTesting() const { return passes.enabled(); }
    size_t queuedGlyphCountForTesting() const { return queued_glyphs.size(); }

    RegistrationCheckpoint checkpointRegistrations() const noexcept {
        return passes.checkpointRegistrations();
    }
    void rollbackRegistrations(RegistrationCheckpoint checkpoint) {
        passes.rollbackRegistrations(checkpoint);
    }
    std::vector<PassId>
    registrationsSince(RegistrationCheckpoint checkpoint) const {
        return passes.registrationsSince(checkpoint);
    }
    std::size_t registrationCount() const noexcept {
        return passes.registrationCount();
    }
    void retireRegistrations(
        const std::vector<PassId> &ids) noexcept {
        passes.retireRegistrations(ids);
    }
};

} // namespace Pelican
