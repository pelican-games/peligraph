#pragma once

#include "framegraphbufferdefinition.hpp"
#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class PathResolver;
class FrameGraphResourceContainer;
class FrameResources;
class RenderTargetContainer;
class RenderTargetLayoutTracker;
class ShaderLibrary;
class VulkanUtils;

// Descriptor caches backed by shared pools and whole-pool generations both
// retire through DeletionQueue submission leases. The pool is declared before
// the descriptor owner so reverse member destruction releases descriptor sets
// before destroying a moved pool.
template <typename DescriptorOwner>
struct RetiredDescriptorResources {
    vk::UniqueDescriptorPool pool;
    DescriptorOwner descriptors;
};

template <typename Queue, typename DescriptorOwner>
void deferRetiredDescriptorResources(
    Queue &queue,
    vk::UniqueDescriptorPool pool,
    DescriptorOwner &&descriptors) {
    queue.defer(
        RetiredDescriptorResources<
            std::decay_t<DescriptorOwner>>{
            std::move(pool),
            std::forward<DescriptorOwner>(descriptors),
        });
}

template <typename Queue, typename DescriptorOwner>
void deferRetiredDescriptorResources(
    Queue &queue,
    DescriptorOwner &&descriptors) {
    deferRetiredDescriptorResources(
        queue, vk::UniqueDescriptorPool{},
        std::forward<DescriptorOwner>(descriptors));
}

struct ComputeTaskRuntimeDependencies {
    ShaderLibrary &shader_library;
    const PathResolver &path_resolver;
    RenderTargetContainer &render_target_container;
    FrameGraphResourceContainer &frame_graph_resources;
    const std::unordered_map<
        std::string, VulkanResourceViewLayout>
        *resource_views = nullptr;
    const std::unordered_map<
        std::string, std::uint32_t>
        *resource_view_counts = nullptr;
    // Feature/pipeline defines are part of the compute shader recipe just as
    // they are for graphics passes. Keeping the borrowed immutable set here
    // also lets project-owned compute algorithms participate in coordinated
    // reload without a second parameter transport.
    const std::vector<std::string>
        *shader_defines = nullptr;
};

struct ResolvedComputeResourceBinding {
    std::string authored_name;
    std::string name;
    bool history_read = false;
    GlobalRenderTargetId render_target = noRenderTargetId();
    FrameGraphBufferId buffer = noFrameGraphBufferId();
    VulkanResourceViewLayout physical_view =
        VulkanResourceViewLayout::shared_2d;
    std::uint32_t physical_view_count = 1;
};

std::vector<FrameGraphBufferDefinition> parseFrameGraphBufferDefinitionsFromJson(const nlohmann::json &config_json);
std::unordered_set<std::string> frameGraphBufferNameSet(const std::vector<FrameGraphBufferDefinition> &definitions);
std::vector<ComputeTaskDefinition> parseComputeTaskDefinitionsFromConfigJson(const nlohmann::json &config_json);
void validateComputeTaskBufferContracts(
    std::span<const FrameGraphBufferDefinition>
        buffer_definitions,
    std::span<const ComputeTaskDefinition>
        task_definitions);

DECLARE_MODULE(FrameGraphResourceContainer) {
  public:
    struct HostBufferPopulation {
        std::uint64_t source_records = 0;
        std::uint64_t written_records = 0;

        bool operator==(
            const HostBufferPopulation &) const = default;
    };

  private:
    struct BufferRecord {
        FrameGraphBufferDefinition definition;
        BufferWrapper buffer;
        std::optional<HostBufferPopulation>
            host_population;
    };

    ResourceContainer<FrameGraphBufferId, BufferRecord> buffers;
    std::unordered_map<std::string, FrameGraphBufferId> name_to_id;
    std::vector<FrameGraphBufferId> registration_order;

  public:
    struct HostBufferTarget {
        FrameGraphBufferId id = noFrameGraphBufferId();
        std::string name;
        vk::DeviceSize size = 0;
        FrameGraphHostBufferSource source =
            FrameGraphHostBufferSource::scene_lights_v2;
    };

    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
        std::unordered_map<std::string, FrameGraphBufferId>
            name_to_id;
    };

    FrameGraphResourceContainer();
    ~FrameGraphResourceContainer();

    void registerBuffers(const std::vector<FrameGraphBufferDefinition> &definitions);
    bool hasBuffer(std::string_view name) const;
    bool hasBuffer(FrameGraphBufferId id) const;
    FrameGraphBufferId getBufferIdByName(std::string_view name) const;
    const BufferWrapper &buffer(std::string_view name) const;
    const BufferWrapper &buffer(FrameGraphBufferId id) const;
    vk::DeviceSize bufferSize(std::string_view name) const;
    vk::DeviceSize bufferSize(FrameGraphBufferId id) const;
    const FrameGraphBufferDefinition &definition(
        FrameGraphBufferId id) const;
    vk::DescriptorBufferInfo descriptorInfo(std::string_view name) const;
    vk::DescriptorBufferInfo descriptorInfo(FrameGraphBufferId id) const;
    bool hasHostBufferSource(
        FrameGraphHostBufferSource source) const;
    std::vector<HostBufferTarget> hostBufferTargets(
        FrameGraphHostBufferSource source) const;
    void writeHostBuffer(
        FrameGraphBufferId id,
        std::span<const std::byte> bytes,
        std::optional<HostBufferPopulation>
            population = std::nullopt);
    std::optional<HostBufferPopulation>
    hostBufferPopulation(
        FrameGraphBufferId id) const;

    RegistrationCheckpoint checkpointRegistrations() const;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<std::tuple<std::string, FrameGraphBufferId,
                           vk::DeviceSize>>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
    std::unordered_map<std::string, FrameGraphBufferId>
    currentNameBindings() const {
        return name_to_id;
    }
    void hideRegistrationName(const std::string &name,
                              FrameGraphBufferId expected);
    void retireRegistrations(
        const std::vector<FrameGraphBufferId> &ids) noexcept;
};

DECLARE_MODULE(ComputeTaskContainer) {
    struct IndirectDispatchRecord {
        FrameGraphBufferId buffer =
            noFrameGraphBufferId();
        vk::DeviceSize offset = 0;
    };

    struct ImageExtentDispatchRecord {
        GlobalRenderTargetId render_target =
            noRenderTargetId();
        std::uint32_t mip_level = 0;
    };

    struct TaskRecord {
        ComputeTaskDefinition definition;
        std::vector<ResolvedComputeResourceBinding> resource_bindings;
        std::vector<ShaderResourceInterfaceBinding>
            resource_interface;
        PipelineHandle pipeline;
        std::vector<
            std::array<vk::UniqueDescriptorSet, 2>>
            descriptor_sets;
        std::vector<
            std::array<
                std::vector<vk::ImageView>, 2>>
            bound_image_views;
        std::uint64_t binding_revision = 0;
        uint32_t dispatch_x = 1;
        uint32_t dispatch_y = 1;
        uint32_t dispatch_z = 1;
        std::optional<IndirectDispatchRecord>
            indirect_dispatch;
        std::optional<ImageExtentDispatchRecord>
            image_extent_dispatch;
    };

    vk::Device device;
    vk::UniqueDescriptorPool descriptor_pool;
    std::unordered_map<int, TaskRecord> tasks;
    std::unordered_map<std::string, ComputeTaskId> name_to_id;
    std::vector<ComputeTaskId> registration_order;
    int next_task_id = 0;
    std::uint64_t next_binding_revision = 1;
    std::array<vk::UniqueSampler, 6> sampled_image_samplers;

    struct DescriptorSetRecord {
        vk::UniqueDescriptorSet descriptor_set;
        std::vector<vk::ImageView> bound_image_views;
    };
    DescriptorSetRecord createDescriptorSet(
        vk::DescriptorPool pool, PipelineHandle pipeline,
        const std::vector<ResolvedComputeResourceBinding> &resources,
        const std::vector<ShaderResourceInterfaceBinding>
            &resource_interface,
        RenderTargetContainer &render_target_container,
        const FrameGraphResourceContainer &frame_graph_resources,
        std::uint32_t frame_index,
        std::uint32_t view_index);
    vk::Sampler samplerFor(
        ShaderResourcePortSampling sampling);

  public:
    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
        std::uint64_t next_binding_revision = 1;
        int next_task_id = 0;
        std::unordered_map<std::string, ComputeTaskId>
            name_to_id;
    };

    ComputeTaskContainer();
    ~ComputeTaskContainer();

    ComputeTaskId registerComputeTask(const ComputeTaskDefinition &definition,
                                      const ComputeTaskRuntimeDependencies &dependencies);
    void rebindRenderTargets(RenderTargetContainer &render_target_container);
    const ComputeTaskDefinition &definition(ComputeTaskId task_id) const;
    ComputeTaskId getComputeTaskIdByName(const std::string &name) const;
    void setDispatchGroups(ComputeTaskId task_id, uint32_t x, uint32_t y, uint32_t z);
    void setDispatchGroups(const std::string &task_name, uint32_t x, uint32_t y, uint32_t z);

    void transitionResourcesForDispatch(vk::CommandBuffer cmd_buf,
                                        ComputeTaskId task_id,
                                        RenderTargetContainer &render_target_container,
                                        VulkanUtils &vk_utils,
                                        RenderTargetLayoutTracker &layout_tracker) const;
    void dispatch(vk::CommandBuffer cmd_buf, ComputeTaskId task_id,
                  const FrameResources &frame_resources,
                  std::uint32_t view_index = 0) const;
    void bufferReadAfterWriteBarrier(vk::CommandBuffer cmd_buf,
                                     FrameGraphBufferId resource,
                                     FramePlanNodeKind from_kind,
                                     FramePlanNodeKind to_kind) const;
    std::vector<vk::ImageView> boundImageViewsForTesting(
        ComputeTaskId task_id, std::uint32_t frame_index,
        std::uint32_t view_index = 0) const;
    std::array<std::uint32_t, 3>
    dispatchGroupsForTesting(
        ComputeTaskId task_id) const;
    std::uint64_t bindingRevisionForTesting(ComputeTaskId task_id) const;

    RegistrationCheckpoint checkpointRegistrations() const;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<std::pair<std::string, ComputeTaskId>>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
    void hideRegistrationName(const std::string &name,
                              ComputeTaskId expected);
    void retireRegistrations(
        const std::vector<ComputeTaskId> &ids) noexcept;
};

} // namespace Pelican
