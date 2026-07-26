#include "materialrender.hpp"
#include "frameresources.hpp"
#include "../light/lightcontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "shadowdepthpasscontainer.hpp"
#include "velocitypasscontainer.hpp"
#include "../vkcore/core.hpp"
#include "camera.hpp"
#include "polygoninstancecontainer.hpp"
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

    const auto &indirect_buf = instance_container.getIndirectBuf();
    GlobalMaterialId current_material_id = invalidMaterialId();
    uint32_t material_index = 0;
    for (const auto &draw_call : draw_calls) {
        const auto draw_index = material_index++;
        if (!material_container.isRenderRequired(pass, draw_call.material)) {
            continue;
        }

        if (material_range.has_value() && isOutsideMaterialRange(draw_index, *material_range)) {
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
        cmd_buf.drawIndexedIndirect(indirect_buf.buffer.get(), draw_call.offset, draw_call.draw_count,
                                    draw_call.stride);
    }
}

void renderShadowDepthDraws(vk::CommandBuffer cmd_buf, PassId pass_id,
                            const ShadowDepthPassContainer &shadow_depth_pass_container,
                            const MaterialRendererDependencies &dependencies) {
    auto &instance_container = dependencies.instance_container;
    const auto &vert_buf_container = dependencies.vert_buf_container;

    const auto &draw_calls = instance_container.getDrawCalls(
        dependencies.first_person_view, std::nullopt,
        dependencies.draw_sort_view_index);
    if (draw_calls.empty()) {
        return;
    }

    const auto &indirect_buf = instance_container.getIndirectBuf();
    for (const auto &draw_call : draw_calls) {
        const auto pipeline_layout = shadow_depth_pass_container.pipelineLayout(pass_id, draw_call.skinned);
        shadow_depth_pass_container.bind(cmd_buf, pass_id, draw_call.skinned);
        vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, pipeline_layout);
        instance_container.bindDeformation(cmd_buf, pipeline_layout, draw_call.skinned);
        PushConstantStruct push_constant{};
        push_constant.mvp = dependencies.light_container.shadowViewProjection();
        cmd_buf.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eVertex, 0,
                              sizeof(push_constant), &push_constant);
        cmd_buf.drawIndexedIndirect(indirect_buf.buffer.get(), draw_call.offset, draw_call.draw_count,
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
    const auto &indirect = instances.getIndirectBuf();
    for (const auto &draw_call : draw_calls) {
        const auto layout = velocity_pass_container.pipelineLayout(pass_id, draw_call.skinned);
        velocity_pass_container.bind(cmd_buf, pass_id, draw_call.skinned);
        dependencies.vert_buf_container.bindVertexBuffer(cmd_buf, draw_call.skinned);
        dependencies.frame_resources.bindGraphics(cmd_buf, layout);
        instances.bindDeformation(cmd_buf, layout, draw_call.skinned);
        cmd_buf.drawIndexedIndirect(indirect.buffer.get(), draw_call.offset,
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
