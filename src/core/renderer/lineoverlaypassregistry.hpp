#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FrameResources;

// One overlay pass: the pipeline the pass was registered with and the free-set
// descriptor that points at the shared vertex buffer.
struct LineOverlayPipelineRecord {
    PipelineHandle pipeline;
    vk::UniqueDescriptorSet descriptor_set;
    std::string acceleration_structure = "scene";
};

// What one overlay class fixes once, at construction. Everything else about
// the three overlays' pass plumbing is the same, which is why they share this
// registry instead of each keeping a copy.
struct LineOverlayRegistryTraits {
    // Names the owner in every message this registry throws, so a failure
    // still says which overlay raised it.
    std::string_view label = "LineOverlay";
    // Bytes of one vertex of the owner's own vertex type.
    vk::DeviceSize vertex_stride = 0;
    // Vertices the first allocation covers; capacity doubles from there.
    vk::DeviceSize initial_vertex_count = 64;
    AuxiliaryDescriptorContract contract =
        AuxiliaryDescriptorContract::vertex_buffer;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eLineList;
    // A sampled atlas takes a second pool size and a second descriptor write.
    bool sampled_image = false;
    // DebugDraw and DebugText gate their queueing on "some pass is
    // registered"; Gizmo keeps no such flag and ignores this one.
    bool tracks_enabled = false;
};

// The device, descriptor pool, vertex buffer and pass table that DebugDraw,
// DebugText and Gizmo used to each hold privately. Held by composition: each
// overlay keeps only its own vertex generation.
class LineOverlayPassRegistry {
  public:
    struct Checkpoint {
        std::size_t registration_count = 0;
        bool enabled = false;
        int next_pass_id = 0;
    };

    explicit LineOverlayPassRegistry(LineOverlayRegistryTraits traits) noexcept
        : traits_{traits} {}

    bool enabled() const noexcept { return enabled_; }
    bool empty() const noexcept { return registration_order_.empty(); }
    std::size_t registrationCount() const noexcept {
        return registration_order_.size();
    }

    // Resolves the device on first use, so an overlay that needs one for its
    // own resources does not repeat the lookup.
    vk::Device device();

    // The atlas binding a sampled-image overlay writes at binding 1. The owner
    // publishes it once its atlas resources exist.
    void setSampledImage(vk::DescriptorImageInfo image) noexcept {
        sampled_image_ = image;
    }

    PassId registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                        ShaderBundleId frag_shader,
                        std::vector<std::string> shader_defines,
                        vk::SampleCountFlagBits samples, bool ray_query,
                        std::string acceleration_structure);

    // Null when the pass was never registered here. The owner throws its own
    // message, because the two overlays that can be asked for a missing pass
    // do different bookkeeping first.
    LineOverlayPipelineRecord *findPass(PassId pass_id) noexcept;

    // Grows the shared vertex buffer to hold `vertex_count` vertices, makes
    // sure `record` owns a descriptor set pointed at it, uploads the vertices
    // and records the bind + draw for them.
    void drawVertices(vk::CommandBuffer cmd_buf,
                      LineOverlayPipelineRecord &record, const void *vertices,
                      std::size_t vertex_count,
                      const FrameResources &frame_resources);

    Checkpoint checkpointRegistrations() const noexcept;
    void rollbackRegistrations(Checkpoint checkpoint);
    std::vector<PassId> registrationsSince(Checkpoint checkpoint) const;
    void retireRegistrations(const std::vector<PassId> &ids) noexcept;

  private:
    void ensureDevice();
    void ensureDescriptorPool();
    void ensureVertexCapacity(std::size_t vertex_count);
    void ensureDescriptorSet(LineOverlayPipelineRecord &record);
    void updateDescriptorSet(const LineOverlayPipelineRecord &record,
                             vk::DeviceSize bytes);

    LineOverlayRegistryTraits traits_;
    vk::Device device_;
    bool enabled_ = false;
    vk::UniqueDescriptorPool descriptor_pool_;
    BufferWrapper vertex_buffer_;
    vk::DeviceSize vertex_buffer_bytes_ = 0;
    std::optional<vk::DescriptorImageInfo> sampled_image_;
    std::unordered_map<PassId, LineOverlayPipelineRecord, PassId::Hash>
        pipelines_;
    std::vector<PassId> registration_order_;
    int next_pass_id_ = 0;
};

} // namespace Pelican
