#include "graphdeformedattribute.hpp"
#include "framegraphruntime.hpp"
#include "computetask.hpp"
#include "../loader/scene.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include <set>
#include <stdexcept>

namespace Pelican {
namespace {
std::optional<std::size_t> resolveAttribute(const GraphDeformedAttribute &attribute,
    const std::vector<RayQueryGeometryInstanceSnapshot> &geometry) {
    const auto source = resolveGraphModelSource(attribute.owner, attribute.asset, attribute.resource);
    if (!source) return std::nullopt;
    const auto identity = source->instance;
    std::optional<std::size_t> match;
    for (std::size_t i = 0; i < geometry.size(); ++i) {
        const auto &candidate = geometry[i];
        if (candidate.instance_index != identity.index || candidate.instance_generation != identity.generation ||
            candidate.scene_epoch != identity.scene_epoch || candidate.node_index != attribute.node ||
            candidate.mesh_index != attribute.mesh || candidate.primitive_index != attribute.primitive) continue;
        if (match) throw std::runtime_error("ambiguous deformed attribute occurrence: " + attribute.resource);
        match = i;
    }
    if (!match) throw std::runtime_error("deformed attribute occurrence is unavailable: " + attribute.resource);
    if (geometry[*match].vertex_offset < 0 || geometry[*match].vertex_count == 0)
        throw std::runtime_error("deformed attribute has an invalid vertex range: " + attribute.resource);
    // A coordinated model reload publishes its model after render preparation.
    // Until recording can use that candidate model, topology changes must be
    // rejected here instead of publishing a buffer incompatible with live data.
    if (geometry[*match].vertex_count != attribute.expected_count)
        throw std::runtime_error("deformed attribute vertex count differs from the live model; "
            "detach the affected graph before changing model topology: " + attribute.resource);
    return match;
}
}
void validateGraphDeformedAttributes(const std::vector<GraphDeformedAttribute> &attributes) {
    if (attributes.empty() || !FastModuleContainer::tryGet<SceneLoader>()) return;
    const auto geometry = GET_MODULE(PolygonInstanceContainer).rayQueryGeometryInstances();
    validateGraphDeformedAttributes(attributes, geometry);
}
void validateGraphDeformedAttributes(const std::vector<GraphDeformedAttribute> &attributes,
    const std::vector<RayQueryGeometryInstanceSnapshot> &geometry) {
    if (attributes.empty() || !FastModuleContainer::tryGet<SceneLoader>()) return;
    for (const auto &attribute : attributes) (void)resolveAttribute(attribute, geometry);
}
void recordGraphDeformedAttributes(vk::CommandBuffer command,
    const CompiledFrameGraphExecution &graph,
    std::vector<RayQueryGeometryInstanceSnapshot> &geometry, bool for_acceleration_structures) {
    if (graph.deformed_attributes.empty()) return;
    auto &resources = GET_MODULE(FrameGraphResourceContainer);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    struct Copy { std::size_t geometry; FrameGraphBufferId destination; std::uint32_t offset; };
    std::vector<Copy> copies;
    std::set<std::size_t> selected;
    // Resolve and validate every destination before recording any writes.
    for (const auto &attribute : graph.deformed_attributes) {
        const auto match = resolveAttribute(attribute, geometry);
        if (!match) continue;
        const auto destination = graph.buffer_bindings.at(attribute.resource);
        if (resources.bufferSize(destination) != vk::DeviceSize{geometry[*match].vertex_count} * 16)
            throw std::runtime_error("deformed attribute count differs from its model generation: " + attribute.resource);
        selected.insert(*match);
        copies.push_back({*match, destination, attribute.byte_offset});
    }
    std::vector<RayQueryGeometryInstanceSnapshot> jobs;
    for (const auto index : selected) jobs.push_back(geometry[index]);
    instances.recordGeometryDeformation(command, jobs, for_acceleration_structures);
    std::size_t job = 0;
    for (const auto index : selected) geometry[index] = std::move(jobs[job++]);
    const vk::MemoryBarrier before{vk::AccessFlagBits::eMemoryWrite,
        vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
        {}, before, {}, {});
    for (const auto &copy : copies) {
        const auto &source = geometry[copy.geometry];
        const bool deformed = bool(source.deformed_vertex_buffer);
        const auto stride = deformed || !source.skinned ? sizeof(CommonVertStruct) : sizeof(CommonSkinningVertStruct);
        const auto base = deformed ? 0 : static_cast<vk::DeviceSize>(source.vertex_offset) * stride;
        const auto buffer = deformed ? source.deformed_vertex_buffer
            : GET_MODULE(VertBufContainer).vertexBuffer(source.skinned).buffer.get();
        std::vector<vk::BufferCopy> regions;
        regions.reserve(source.vertex_count);
        for (std::uint32_t vertex = 0; vertex < source.vertex_count; ++vertex)
            regions.emplace_back(base + vertex * stride + copy.offset, vk::DeviceSize{vertex} * 16, 12);
        command.copyBuffer(buffer, resources.buffer(copy.destination).buffer.get(), regions);
    }
    const vk::MemoryBarrier after{vk::AccessFlagBits::eTransferWrite,
        vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite};
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
        {}, after, {}, {});
}
}
