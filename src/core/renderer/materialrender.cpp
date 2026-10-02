#include "materialrender.hpp"
#include "frameresources.hpp"
#include "../renderingpass/computetask.hpp"
#include "../light/lightcontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/materialdrawlooptable.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "shadowdepthpasscontainer.hpp"
#include "velocitypasscontainer.hpp"
#include "../vkcore/core.hpp"
#include "camera.hpp"
#include "polygoninstancecontainer.hpp"
#include <algorithm>
#include <optional>
#include <span>

namespace Pelican {

namespace {
struct MaterialRange {
    uint32_t start;
    uint32_t count;
};

bool isOutsideMaterialRange(uint32_t material_index, const MaterialRange &range) {
    return material_index < range.start || material_index - range.start >= range.count;
}

// Where one pass reads its indirect commands from, and which compiled draw
// queue goes with them. Every pass renderer below asks the same question: a
// secondary view of a family that prepared its own per-view region reads that
// region, and everything else reads the canonical queue.
struct ResolvedDrawSource {
    const BufferWrapper *indirect_buf = nullptr;
    vk::DeviceSize indirect_offset_bias = 0;
    bool prepared_view_family = false;
    std::span<const DrawIndirectInfo> draw_calls;
};

ResolvedDrawSource resolveDrawSource(
    const MaterialRendererDependencies &dependencies,
    std::optional<MaterialPhase> phase = std::nullopt,
    std::optional<MaterialDrawTagFilterId> material_filter = std::nullopt) {
    auto &instances = dependencies.instance_container;

    ResolvedDrawSource source;
    source.indirect_buf = &instances.getIndirectBuf();
    if (dependencies.secondary_view_index) {
        if (const auto offset = instances.viewFamilyDrawOffset(
                dependencies.view_family,
                *dependencies.secondary_view_index)) {
            source.indirect_buf = &instances.viewFamilyIndirectBuffer();
            source.indirect_offset_bias = *offset;
            source.prepared_view_family = true;
        }
    }
    source.draw_calls =
        source.prepared_view_family
            ? std::span<const DrawIndirectInfo>{
                  instances.getViewFamilyDrawCalls(
                      dependencies.view_family,
                      *dependencies.secondary_view_index,
                      dependencies.first_person_view, phase, material_filter)}
            : std::span<const DrawIndirectInfo>{instances.getDrawCalls(
                  dependencies.first_person_view, phase,
                  dependencies.draw_sort_view_index, material_filter)};
    return source;
}

void renderMaterialDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                         const PassDefinition &pass,
                         const MaterialRendererDependencies &dependencies,
                         std::optional<MaterialRange> material_range,
                         RenderPassViewInvocation invocation,
                         std::vector<MaterialDrawRecord> *draw_record_sink) {
    (void)pass_id;
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

    const auto source =
        resolveDrawSource(dependencies, phase, material_filter);
    const auto *indirect_buf = source.indirect_buf;
    const auto indirect_offset_bias = source.indirect_offset_bias;
    const auto use_prepared_view_family_draws =
        source.prepared_view_family;
    const auto &draw_calls = source.draw_calls;
    if (draw_calls.empty()) {
        return;
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

    // LP-2: the last bound pipeline and descriptor set, by what was bound
    // rather than by the material that asked. A range drawn twice with an
    // override and then the material shares the material but not the
    // pipeline, and the second draw must bind the material's own again.
    MaterialContainer::MaterialDrawBindState bound;
    // MP-1: this pass's count in the frame's tally, when the frame keeps one.
    auto *drawn =
        dependencies.drawn_materials != nullptr
            ? &dependencies.drawn_materials->try_emplace(pass.name).first->second
            : nullptr;
    const auto *draw_loops =
        dependencies.draw_loops != nullptr &&
                !dependencies.draw_loops->empty()
            ? dependencies.draw_loops
            : nullptr;
    const auto draw_step =
        [&](const DrawIndirectInfo &draw_call,
            GlobalMaterialId effective_material,
            std::optional<PipelineHandle> pipeline) {
        const auto binding = material_container.bindMaterialDraw(
            cmd_buf, pass, effective_material, pipeline, bound,
            invocation);
        if (drawn != nullptr) {
            drawn->add(effective_material.value, draw_call.draw_count);
        }
        const auto pipeline_layout = binding.layout;
        vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, pipeline_layout, pass.ray_query_acceleration_structure);
        instance_container.bindMaterialInstanceResources(cmd_buf, pipeline_layout,
                                                         draw_call.skinned);
        const PushConstantStruct engine_push{dependencies.view_projection};
        cmd_buf.pushConstants(pipeline_layout,
                              vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                              sizeof(engine_push), &engine_push);
        if (effective_material.value < 0) {
            throw std::runtime_error(
                "material pass resolved an invalid GPU material index");
        }
        const MaterialIndexPushConstant material_push{
            static_cast<std::uint32_t>(effective_material.value),
            draw_call.source_material_index};
        cmd_buf.pushConstants(pipeline_layout,
                              vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              PELICAN_PUSH_ENGINE_BYTES, sizeof(material_push), &material_push);
        if (draw_record_sink != nullptr) {
            draw_record_sink->push_back(MaterialDrawRecord{
                .pass = pass.name,
                .material = draw_call.material.value,
                .effective_material = effective_material.value,
                .pipeline = binding.pipeline.value,
                .descriptor_set = reinterpret_cast<std::uint64_t>(
                    static_cast<VkDescriptorSet>(
                        binding.descriptor_set)),
                .pipeline_bound = binding.pipeline_bound,
                .descriptor_set_bound =
                    binding.descriptor_set_bound,
                .gpu_material_index = material_push.material_index,
                .source_material_index =
                    material_push.source_material_index,
                .offset = draw_call.offset,
                .draw_count = draw_call.draw_count,
                .gpu_draw = use_gpu_draws,
            });
        }
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
    };

    // A range is the consecutive draws of one material. Its steps are
    // drawn in order, each over the whole range. Without a table entry the
    // range has one step, the pass's effective material, and the loop
    // below issues exactly the commands it issued before draw steps.
    const auto draw_count = draw_calls.size();
    for (std::size_t range_begin = 0; range_begin < draw_count;) {
        const auto material = draw_calls[range_begin].material;
        auto range_end = range_begin + 1;
        while (range_end < draw_count &&
               draw_calls[range_end].material == material) {
            ++range_end;
        }
        const auto *steps =
            draw_loops != nullptr
                ? draw_loops->find(pass.name, material)
                : nullptr;
        const auto step_count = steps != nullptr ? steps->size() : 1;
        for (std::size_t step = 0; step < step_count; ++step) {
            for (auto index = range_begin; index < range_end; ++index) {
                const auto &draw_call = draw_calls[index];
                if (!material_container.isRenderRequired(
                        pass, draw_call.material)) {
                    continue;
                }
                if (!selectedByMaterialRange(
                        static_cast<std::uint32_t>(index))) {
                    continue;
                }
                if (steps == nullptr) {
                    draw_step(draw_call,
                              material_container.resolveMaterialForPass(
                                  pass, draw_call.material),
                              std::nullopt);
                } else {
                    const auto &resolved = (*steps)[step];
                    draw_step(draw_call, resolved.material,
                              resolved.pipeline);
                }
            }
        }
        range_begin = range_end;
    }
}

void recordQueueDraw(std::vector<MaterialDrawRecord> *sink,
                     std::string_view kind,
                     const DrawIndirectInfo &draw_call) {
    if (sink == nullptr) return;
    sink->push_back(MaterialDrawRecord{
        .kind = std::string{kind},
        .material = draw_call.material.value,
        .effective_material = draw_call.material.value,
        .source_material_index = draw_call.source_material_index,
        .offset = draw_call.offset,
        .draw_count = draw_call.draw_count,
    });
}

void renderShadowDepthDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                            const ShadowDepthPassContainer &shadow_depth_pass_container,
                            const MaterialRendererDependencies &dependencies,
                            std::vector<MaterialDrawRecord> *draw_record_sink) {
    auto &instance_container = dependencies.instance_container;
    const auto &vert_buf_container = dependencies.vert_buf_container;

    const auto source = resolveDrawSource(dependencies);
    const auto *indirect_buf = source.indirect_buf;
    const auto indirect_offset_bias = source.indirect_offset_bias;
    const auto &draw_calls = source.draw_calls;
    if (draw_calls.empty()) {
        return;
    }

    for (const auto &draw_call : draw_calls) {
        const auto pipeline_layout = shadow_depth_pass_container.pipelineLayout(pass_id, draw_call.skinned);
        shadow_depth_pass_container.bind(cmd_buf, pass_id, draw_call.skinned);
        vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, pipeline_layout,
            shadow_depth_pass_container.accelerationStructure(pass_id));
        instance_container.bindDeformation(cmd_buf, pipeline_layout, draw_call.skinned);
        PushConstantStruct push_constant{};
        push_constant.mvp =
            dependencies.view_projection;
        cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
                              sizeof(push_constant), &push_constant);
        recordQueueDraw(draw_record_sink, "shadow_depth", draw_call);
        cmd_buf.drawIndexedIndirect(indirect_buf->buffer.get(),
                                    indirect_offset_bias + draw_call.offset,
                                    draw_call.draw_count,
                                    draw_call.stride);
    }
}

void renderVelocityDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                         const VelocityPassContainer &velocity_pass_container,
                         const MaterialRendererDependencies &dependencies,
                         std::vector<MaterialDrawRecord> *draw_record_sink) {
    auto &instances = dependencies.instance_container;
    const auto source = resolveDrawSource(dependencies);
    const auto *indirect = source.indirect_buf;
    const auto indirect_offset_bias = source.indirect_offset_bias;
    const auto &draw_calls = source.draw_calls;
    if (draw_calls.empty()) return;
    for (const auto &draw_call : draw_calls) {
        const auto layout = velocity_pass_container.pipelineLayout(pass_id, draw_call.skinned);
        velocity_pass_container.bind(cmd_buf, pass_id, draw_call.skinned);
        dependencies.vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, layout,
            velocity_pass_container.accelerationStructure(pass_id));
        instances.bindDeformation(cmd_buf, layout, draw_call.skinned);
        recordQueueDraw(draw_record_sink, "velocity", draw_call);
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
                        invocation, draw_record_sink);
}

void MaterialRenderer::renderWithMaterialRange(vk::CommandBuffer cmd_buf, PassId pass_id,
                                               const PassDefinition &pass,
                                               uint32_t material_start, uint32_t material_count,
                                               const MaterialRendererDependencies &dependencies,
                                               RenderPassViewInvocation invocation) const {
    if (material_count == 0) {
        renderMaterialDraws(cmd_buf, pass_id, pass, dependencies, std::nullopt,
                            invocation, draw_record_sink);
        return;
    }

    renderMaterialDraws(cmd_buf, pass_id, pass, dependencies,
                        MaterialRange{material_start, material_count},
                        invocation, draw_record_sink);
}

void MaterialRenderer::renderShadowDepth(vk::CommandBuffer cmd_buf, PassId pass_id,
                                         const ShadowDepthPassContainer &shadow_depth_pass_container,
                                         const MaterialRendererDependencies &dependencies) const {
    renderShadowDepthDraws(cmd_buf, pass_id, shadow_depth_pass_container, dependencies,
                           draw_record_sink);
}

void MaterialRenderer::renderVelocity(
    vk::CommandBuffer cmd_buf, PassId pass_id,
    const VelocityPassContainer &velocity_pass_container,
    const MaterialRendererDependencies &dependencies) const {
    renderVelocityDraws(cmd_buf, pass_id, velocity_pass_container, dependencies,
                        draw_record_sink);
}

} // namespace Pelican
