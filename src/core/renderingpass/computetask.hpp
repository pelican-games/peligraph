#pragma once

#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../container.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class PathResolver;
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
};

std::vector<FrameGraphBufferDefinition> parseFrameGraphBufferDefinitionsFromJson(const nlohmann::json &config_json);
std::unordered_set<std::string> frameGraphBufferNameSet(const std::vector<FrameGraphBufferDefinition> &definitions);
std::vector<ComputeTaskDefinition> parseComputeTaskDefinitionsFromConfigJson(const nlohmann::json &config_json);

DECLARE_MODULE(FrameGraphResourceContainer) {
    struct BufferRecord {
        FrameGraphBufferDefinition definition;
        BufferWrapper buffer;
    };

    std::unordered_map<std::string, BufferRecord> buffers;

  public:
    FrameGraphResourceContainer();
    ~FrameGraphResourceContainer();

    void registerBuffers(const std::vector<FrameGraphBufferDefinition> &definitions);
    bool hasBuffer(std::string_view name) const;
    const BufferWrapper &buffer(std::string_view name) const;
    vk::DeviceSize bufferSize(std::string_view name) const;
    vk::DescriptorBufferInfo descriptorInfo(std::string_view name) const;
};

DECLARE_MODULE(ComputeTaskContainer) {
    struct TaskRecord {
        ComputeTaskDefinition definition;
        PipelineHandle pipeline;
        vk::UniqueDescriptorSet descriptor_set;
        uint32_t dispatch_x = 1;
        uint32_t dispatch_y = 1;
        uint32_t dispatch_z = 1;
    };

    vk::Device device;
    vk::UniqueDescriptorPool descriptor_pool;
    std::unordered_map<int, TaskRecord> tasks;
    std::unordered_map<std::string, ComputeTaskId> name_to_id;

    vk::UniqueDescriptorSet createDescriptorSet(PipelineHandle pipeline,
                                                const ComputeTaskDefinition &definition,
                                                const ComputeTaskRuntimeDependencies &dependencies) const;

  public:
    ComputeTaskContainer();
    ~ComputeTaskContainer();

    ComputeTaskId registerComputeTask(const ComputeTaskDefinition &definition,
                                      const ComputeTaskRuntimeDependencies &dependencies);
    const ComputeTaskDefinition &definition(ComputeTaskId task_id) const;
    ComputeTaskId getComputeTaskIdByName(const std::string &name) const;
    void setDispatchGroups(ComputeTaskId task_id, uint32_t x, uint32_t y, uint32_t z);
    void setDispatchGroups(const std::string &task_name, uint32_t x, uint32_t y, uint32_t z);

    void transitionResourcesForDispatch(vk::CommandBuffer cmd_buf,
                                        ComputeTaskId task_id,
                                        RenderTargetContainer &render_target_container,
                                        VulkanUtils &vk_utils,
                                        RenderTargetLayoutTracker &layout_tracker) const;
    void dispatch(vk::CommandBuffer cmd_buf, ComputeTaskId task_id) const;
    void bufferReadAfterWriteBarrier(vk::CommandBuffer cmd_buf,
                                     const std::string &resource,
                                     FramePlanNodeKind from_kind,
                                     FramePlanNodeKind to_kind) const;
};

} // namespace Pelican
