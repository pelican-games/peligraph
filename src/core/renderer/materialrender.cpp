#include "materialrender.hpp"
#include "frameresources.hpp"
#include "../renderingpass/computetask.hpp"
#include "../light/lightcontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "shadowdepthpasscontainer.hpp"
#include "velocitypasscontainer.hpp"
#include "../vkcore/core.hpp"
#include "camera.hpp"
#include "polygoninstancecontainer.hpp"
#include <algorithm>
#include <optional>

namespace Pelican {

namespace {
struct MaterialRange {
    uint32_t start;
    uint32_t count;
};

bool isOutsideMaterialRange(uint32_t material_index, const MaterialRange &range) {
    return material_index < range.start || material_index - range.start >= range.count;
}

void renderMaterialDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                         const PassDefinition &pass,
                         const MaterialRendererDependencies &dependencies,
                         std::optional<MaterialRange> material_range,
                         RenderPassViewInvocation invocation) {
    auto &instance_container = dependencies.instance_container;
    const auto &vert_buf_container = dependencies.vert_buf_container;
    const auto &material_container = dependencies.material_container;

    const auto contract = pass.materialInfo().contract;
    const auto phase =
        contract == MaterialPassContract::legacy_gbuffer_v1
            ? std::optional<MaterialPhase>{}
            : std::optional{materialPassPhase(contract)};
    const auto material_filter =
        pass.materialInfo().material_filter
            ? std::optional{
                  pass.materialInfo()
                      .material_filter->id}
            : std::nullopt;
    const auto &draw_calls = instance_container.getDrawCalls(
        dependencies.first_person_view, phase,
        dependencies.draw_sort_view_index,
        material_filter);
    if (draw_calls.empty()) {
        return;
    }

    const BufferWrapper *indirect_buf =
        &instance_container.getIndirectBuf();
    vk::DeviceSize indirect_offset_bias = 0;
    bool use_prepared_view_family_draws =
        false;
    if (dependencies.secondary_view_index) {
        if (const auto offset =
                instance_container
                    .viewFamilyDrawOffset(
                        dependencies.view_family,
                        *dependencies
                             .secondary_view_index)) {
            indirect_buf =
                &instance_container
                     .viewFamilyIndirectBuffer();
            indirect_offset_bias = *offset;
            use_prepared_view_family_draws =
                true;
        }
    }
    const auto &gpu_draw_source =
        pass.materialInfo().gpu_draw_source;
    if (gpu_draw_source &&
        gpu_draw_source->layout ==
            GpuDrawSourceLayout::
                fixed_state_v1 &&
        pass.materialInfo().material_count != 1) {
        throw std::runtime_error(
            "Material pass '" + pass.name +
            "' gpu_draw_source requires one fixed material range");
    }
    if (gpu_draw_source &&
        gpu_draw_source->layout ==
            GpuDrawSourceLayout::
                draw_queue_segments_v1 &&
        pass.materialInfo().material_count == 0) {
        throw std::runtime_error(
            "Material pass '" + pass.name +
            "' segmented gpu_draw_source requires a non-empty material range");
    }
    std::optional<std::uint32_t>
        gpu_device_draw_limit;
    if (gpu_draw_source &&
        gpu_draw_source->execution ==
            GpuDrawExecutionMode::automatic) {
        const auto &vkcore =
            GET_MODULE(VulkanManageCore);
        const auto device_draw_limit =
            vkcore.getPhysDevice()
                .getProperties()
                .limits.maxDrawIndirectCount;
        if (vkcore.getRuntimeCapabilities()
                .draw_indirect_count &&
            device_draw_limit != 0) {
            gpu_device_draw_limit =
                device_draw_limit;
        }
    }

    const auto selectedByMaterialRange =
        [&](std::uint32_t draw_index) {
            return !material_range ||
                   !isOutsideMaterialRange(
                       draw_index,
                       *material_range);
        };
    auto use_gpu_draws =
        gpu_draw_source.has_value() &&
        gpu_device_draw_limit.has_value() &&
        !use_prepared_view_family_draws;
    if (use_gpu_draws) {
        if (!isValidFrameGraphBufferId(
                gpu_draw_source->commands_id) ||
            !isValidFrameGraphBufferId(
                gpu_draw_source->count_id) ||
            (gpu_draw_source->layout ==
                 GpuDrawSourceLayout::
                     draw_queue_segments_v1 &&
             !isValidFrameGraphBufferId(
                 gpu_draw_source
                     ->segments_id))) {
            throw std::runtime_error(
                "Material pass '" + pass.name +
                "' gpu_draw_source was not pinned to the active GPU "
                "generation");
        }
    }

    if (use_gpu_draws &&
        gpu_draw_source->layout ==
            GpuDrawSourceLayout::
                draw_queue_segments_v1) {
        const auto population =
            dependencies.frame_graph_resources
                .hostBufferPopulation(
                    gpu_draw_source
                        ->segments_id);
        if (!population) {
            use_gpu_draws = false;
        } else {
            const auto command_slots =
                static_cast<std::uint64_t>(
                    (dependencies
                         .frame_graph_resources
                         .bufferSize(
                             gpu_draw_source
                                 ->commands_id) -
                     gpu_draw_source
                         ->command_offset) /
                    frameGraphIndexedDrawCommandBytes);
            const auto count_slots =
                static_cast<std::uint64_t>(
                    (dependencies
                         .frame_graph_resources
                         .bufferSize(
                             gpu_draw_source
                                 ->count_id) -
                     gpu_draw_source
                         ->count_offset) /
                    frameGraphDrawCountBytes);
            std::uint32_t draw_index = 0;
            for (const auto &draw_call :
                 draw_calls) {
                const auto selected =
                    selectedByMaterialRange(
                        draw_index++);
                if (!selected) continue;
                if (draw_call
                        .scene_segment_index ==
                        noSceneDrawSegmentIndex ||
                    draw_call
                            .scene_segment_index >=
                        population
                            ->written_records) {
                    use_gpu_draws = false;
                    break;
                }
                const auto &segment =
                    instance_container
                        .sceneDrawSegment(
                            draw_call
                                .scene_segment_index);
                const auto expected_source =
                    draw_call.offset /
                    sizeof(RenderCommand);
                if (draw_call.offset %
                            sizeof(RenderCommand) !=
                        0 ||
                    segment
                            .source_first_command !=
                        expected_source ||
                    segment.command_capacity !=
                        draw_call.draw_count) {
                    throw std::runtime_error(
                        "Material pass '" +
                        pass.name +
                        "' scene draw segment no longer matches its CPU "
                        "DrawQueue range");
                }
                const auto output_end =
                    static_cast<std::uint64_t>(
                        segment
                            .output_first_command) +
                    segment.command_capacity;
                if (output_end >
                        gpu_draw_source
                            ->max_draw_count ||
                    output_end >
                        command_slots ||
                    segment
                            .output_count_index >=
                        count_slots) {
                    use_gpu_draws = false;
                    break;
                }
            }
        }
    }

    if (use_gpu_draws &&
        gpu_draw_source->layout ==
            GpuDrawSourceLayout::
                fixed_state_v1) {
        gpu_device_draw_limit = std::min(
            gpu_draw_source
                ->max_draw_count,
            *gpu_device_draw_limit);
    }

    GlobalMaterialId current_material_id = invalidMaterialId();
    uint32_t material_index = 0;
    for (const auto &draw_call : draw_calls) {
        const auto draw_index = material_index++;
        if (!material_container.isRenderRequired(pass, draw_call.material)) {
            continue;
        }

        if (!selectedByMaterialRange(draw_index)) {
            continue;
        }

        material_container.bindResource(cmd_buf, pass_id, pass, draw_call.material,
                                        current_material_id, invocation);
        const auto pipeline_layout =
            material_container.pipelineLayout(
                pass, draw_call.material);
        vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, pipeline_layout);
        instance_container.bindMaterialInstanceResources(cmd_buf, pipeline_layout,
                                                         draw_call.skinned);
        const PushConstantStruct engine_push{dependencies.view_projection};
        cmd_buf.pushConstants(pipeline_layout,
                              vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                              sizeof(engine_push), &engine_push);
        const MaterialIndexPushConstant material_push{
            material_container.materialGpuIndexForPass(
                pass, draw_call.material),
            draw_call.source_material_index};
        cmd_buf.pushConstants(pipeline_layout,
                              vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              PELICAN_PUSH_ENGINE_BYTES, sizeof(material_push), &material_push);
        current_material_id = draw_call.material;
        if (use_gpu_draws) {
            auto command_offset =
                gpu_draw_source
                    ->command_offset;
            auto count_offset =
                gpu_draw_source
                    ->count_offset;
            auto draw_limit =
                gpu_draw_source
                    ->max_draw_count;
            if (gpu_draw_source->layout ==
                GpuDrawSourceLayout::
                    draw_queue_segments_v1) {
                const auto &segment =
                    instance_container
                        .sceneDrawSegment(
                            draw_call
                                .scene_segment_index);
                command_offset +=
                    static_cast<vk::DeviceSize>(
                        segment
                            .output_first_command) *
                    frameGraphIndexedDrawCommandBytes;
                count_offset +=
                    static_cast<vk::DeviceSize>(
                        segment
                            .output_count_index) *
                    frameGraphDrawCountBytes;
                draw_limit =
                    segment.command_capacity;
            }
            draw_limit = std::min(
                draw_limit,
                *gpu_device_draw_limit);
            const auto &commands =
                dependencies.frame_graph_resources
                    .buffer(
                        gpu_draw_source
                            ->commands_id);
            const auto &count =
                dependencies.frame_graph_resources
                    .buffer(
                        gpu_draw_source
                            ->count_id);
            cmd_buf.drawIndexedIndirectCount(
                commands.buffer.get(),
                command_offset,
                count.buffer.get(),
                count_offset,
                draw_limit,
                static_cast<std::uint32_t>(
                    frameGraphIndexedDrawCommandBytes));
        } else {
            // The established CPU-compiled draw queue is the semantic
            // fallback when the device cannot consume a GPU-written count or
            // the current segmented publication exceeds authored capacity.
            cmd_buf.drawIndexedIndirect(
                indirect_buf->buffer.get(),
                indirect_offset_bias +
                    draw_call.offset,
                draw_call.draw_count,
                draw_call.stride);
        }
    }
}

void renderShadowDepthDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                            const ShadowDepthPassContainer &shadow_depth_pass_container,
                            const MaterialRendererDependencies &dependencies) {
    auto &instance_container = dependencies.instance_container;
    const auto &vert_buf_container = dependencies.vert_buf_container;

    const auto &draw_calls =
        instance_container.getDrawCalls(
            dependencies.first_person_view,
            std::nullopt,
            dependencies.draw_sort_view_index);
    if (draw_calls.empty()) {
        return;
    }
    const BufferWrapper *indirect_buf =
        &instance_container.getIndirectBuf();
    vk::DeviceSize indirect_offset_bias = 0;
    if (dependencies.secondary_view_index) {
        if (const auto offset =
                instance_container
                    .viewFamilyDrawOffset(
                        dependencies.view_family,
                        *dependencies
                             .secondary_view_index)) {
            indirect_buf =
                &instance_container
                     .viewFamilyIndirectBuffer();
            indirect_offset_bias = *offset;
        }
    }

    for (const auto &draw_call : draw_calls) {
        const auto pipeline_layout = shadow_depth_pass_container.pipelineLayout(pass_id, draw_call.skinned);
        shadow_depth_pass_container.bind(cmd_buf, pass_id, draw_call.skinned);
        vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, pipeline_layout);
        instance_container.bindDeformation(cmd_buf, pipeline_layout, draw_call.skinned);
        PushConstantStruct push_constant{};
        push_constant.mvp =
            dependencies.view_projection;
        cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
                              sizeof(push_constant), &push_constant);
        cmd_buf.drawIndexedIndirect(indirect_buf->buffer.get(),
                                    indirect_offset_bias + draw_call.offset,
                                    draw_call.draw_count,
                                    draw_call.stride);
    }
}

void renderVelocityDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                         const VelocityPassContainer &velocity_pass_container,
                         const MaterialRendererDependencies &dependencies) {
    auto &instances = dependencies.instance_container;
    const auto &draw_calls = instances.getDrawCalls(
        dependencies.first_person_view, std::nullopt,
        dependencies.draw_sort_view_index);
    if (draw_calls.empty()) return;
    const BufferWrapper *indirect =
        &instances.getIndirectBuf();
    vk::DeviceSize indirect_offset_bias = 0;
    if (dependencies.secondary_view_index) {
        if (const auto offset =
                instances.viewFamilyDrawOffset(
                    dependencies.view_family,
                    *dependencies
                         .secondary_view_index)) {
            indirect =
                &instances
                     .viewFamilyIndirectBuffer();
            indirect_offset_bias = *offset;
        }
    }
    for (const auto &draw_call : draw_calls) {
        const auto layout = velocity_pass_container.pipelineLayout(pass_id, draw_call.skinned);
        velocity_pass_container.bind(cmd_buf, pass_id, draw_call.skinned);
        dependencies.vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, layout);
        instances.bindDeformation(cmd_buf, layout, draw_call.skinned);
        cmd_buf.drawIndexedIndirect(indirect->buffer.get(),
                                    indirect_offset_bias + draw_call.offset,
                                    draw_call.draw_count, draw_call.stride);
    }
}
}

MaterialRenderer::MaterialRenderer() = default;

void MaterialRenderer::render(vk::CommandBuffer cmd_buf, PassId pass_id,
                              const PassDefinition &pass,
                              const MaterialRendererDependencies &dependencies,
                              RenderPassViewInvocation invocation) const {
    renderMaterialDraws(cmd_buf, pass_id, pass, dependencies, std::nullopt,
                        invocation);
}

void MaterialRenderer::renderWithMaterialRange(vk::CommandBuffer cmd_buf, PassId pass_id,
                                               const PassDefinition &pass,
                                               uint32_t material_start, uint32_t material_count,
                                               const MaterialRendererDependencies &dependencies,
                                               RenderPassViewInvocation invocation) const {
    if (material_count == 0) {
        renderMaterialDraws(cmd_buf, pass_id, pass, dependencies, std::nullopt,
                            invocation);
        return;
    }

    renderMaterialDraws(cmd_buf, pass_id, pass, dependencies,
                        MaterialRange{material_start, material_count},
                        invocation);
}

void MaterialRenderer::renderShadowDepth(vk::CommandBuffer cmd_buf, PassId pass_id,
                                         const ShadowDepthPassContainer &shadow_depth_pass_container,
                                         const MaterialRendererDependencies &dependencies) const {
    renderShadowDepthDraws(cmd_buf, pass_id, shadow_depth_pass_container, dependencies);
}

void MaterialRenderer::renderVelocity(
    vk::CommandBuffer cmd_buf, PassId pass_id,
    const VelocityPassContainer &velocity_pass_container,
    const MaterialRendererDependencies &dependencies) const {
    renderVelocityDraws(cmd_buf, pass_id, velocity_pass_container, dependencies);
}

} // namespace Pelican
