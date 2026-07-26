#pragma once

#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

struct FrameGraphBufferDefinition {
    std::string name;
    vk::DeviceSize size = 0;
    bool persistent = true;
};

struct ComputeTaskRuntimeDependencies {
    ShaderLibrary &shader_library;
    const PathResolver &path_resolver;
    RenderTargetContainer &render_target_container;
    FrameGraphResourceContainer &frame_graph_resources;
    const std::unordered_map<
        std::string, VulkanResourceViewLayout>
        *resource_views = nullptr;
};

struct ResolvedComputeResourceBinding {
    std::string authored_name;
    std::string name;
    bool history_read = false;
    GlobalRenderTargetId render_target = noRenderTargetId();
    FrameGraphBufferId buffer = noFrameGraphBufferId();
    VulkanResourceViewLayout physical_view =
        VulkanResourceViewLayout::shared_2d;
};

std::vector<FrameGraphBufferDefinition> parseFrameGraphBufferDefinitionsFromJson(const nlohmann::json &config_json);
std::unordered_set<std::string> frameGraphBufferNameSet(const std::vector<FrameGraphBufferDefinition> &definitions);
std::vector<ComputeTaskDefinition> parseComputeTaskDefinitionsFromConfigJson(const nlohmann::json &config_json);

DECLARE_MODULE(FrameGraphResourceContainer) {
    struct BufferRecord {
        FrameGraphBufferDefinition definition;
        BufferWrapper buffer;
    };

    ResourceContainer<FrameGraphBufferId, BufferRecord> buffers;
    std::unordered_map<std::string, FrameGraphBufferId> name_to_id;
    std::vector<FrameGraphBufferId> registration_order;

  public:
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
    vk::DescriptorBufferInfo descriptorInfo(std::string_view name) const;
    vk::DescriptorBufferInfo descriptorInfo(FrameGraphBufferId id) const;

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
    struct TaskRecord {
        ComputeTaskDefinition definition;
        std::vector<ResolvedComputeResourceBinding> resource_bindings;
        std::vector<ShaderResourceInterfaceBinding>
            resource_interface;
        PipelineHandle pipeline;
        std::array<vk::UniqueDescriptorSet, 2> descriptor_sets;
        std::array<std::vector<vk::ImageView>, 2> bound_image_views;
        std::uint64_t binding_revision = 0;
        uint32_t dispatch_x = 1;
        uint32_t dispatch_y = 1;
        uint32_t dispatch_z = 1;
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
        std::uint32_t frame_index);
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
                  const FrameResources &frame_resources) const;
    void bufferReadAfterWriteBarrier(vk::CommandBuffer cmd_buf,
                                     FrameGraphBufferId resource,
                                     FramePlanNodeKind from_kind,
                                     FramePlanNodeKind to_kind) const;
    std::vector<vk::ImageView> boundImageViewsForTesting(
        ComputeTaskId task_id, std::uint32_t frame_index) const;
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
