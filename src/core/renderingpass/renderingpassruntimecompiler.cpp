#include "renderingpassruntimecompiler.hpp"
#include "computetask.hpp"
#include "rendertargetimageviewresolver.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "rasterpassvulkanadapter.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../profiler.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderer/gizmo.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../shader/shaderresourceinterface.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"
#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace Pelican {

namespace {

struct FullscreenRuntimeDependencies {
    RenderTarget &render_target;
    const RenderTargetMetadataResolver &render_target_metadata;
    const RenderTargetImageViewResolver &render_target_views;
    ShaderLibrary &shader_library;
    FullscreenPassContainer &fullscreen_pass_container;
    FrameGraphResourceContainer &frame_graph_resources;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
};

struct DebugDrawRuntimeDependencies {
    RenderTarget &render_target;
    const RenderTargetMetadataResolver &render_target_metadata;
    ShaderLibrary &shader_library;
    DebugDraw &debug_draw;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
};

struct DebugTextRuntimeDependencies {
    RenderTarget &render_target;
    const RenderTargetMetadataResolver &render_target_metadata;
    ShaderLibrary &shader_library;
    DebugText &debug_text;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
};

struct GizmoRuntimeDependencies {
    RenderTarget &render_target;
    const RenderTargetMetadataResolver &render_target_metadata;
    ShaderLibrary &shader_library;
    Gizmo &gizmo;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
};

struct ShadowDepthRuntimeDependencies {
    const RenderTargetMetadataResolver &render_target_metadata;
    ShaderLibrary &shader_library;
    ShadowDepthPassContainer &shadow_depth_pass_container;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
};

struct GeometryPassRuntimeDependencies {
    const RenderTargetMetadataResolver &render_target_metadata;
    ShaderLibrary &shader_library;
    VelocityPassContainer &velocity_pass_container;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
};

struct FullscreenShaderModules {
    ShaderBundleId vert_shader;
    ShaderBundleId frag_shader;
};

struct GenericRasterShaderModules {
    ShaderBundleId vert_shader;
    std::optional<ShaderBundleId> frag_shader;
};

const VulkanPhysicalScopePlan &requirePassScope(
    const VulkanTargetPlan &plan,
    std::string_view pass_name) {
    const VulkanPhysicalScopePlan *result = nullptr;
    for (const auto &scope : plan.scopes) {
        if (std::find(scope.nodes.begin(), scope.nodes.end(),
                      pass_name) == scope.nodes.end()) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "render pass belongs to more than one physical scope: " +
                std::string{pass_name});
        }
        result = &scope;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "render pass has no physical target-plan scope: " +
            std::string{pass_name});
    }
    return *result;
}

std::size_t requirePassScopeIndex(
    const VulkanTargetPlan &plan,
    std::string_view pass_name) {
    std::optional<std::size_t> result;
    for (std::size_t index = 0;
         index < plan.scopes.size(); ++index) {
        const auto &scope = plan.scopes[index];
        if (std::find(scope.nodes.begin(),
                      scope.nodes.end(),
                      pass_name) ==
            scope.nodes.end()) {
            continue;
        }
        if (result) {
            throw std::runtime_error(
                "render pass belongs to more than one physical scope: " +
                std::string{pass_name});
        }
        result = index;
    }
    if (!result) {
        throw std::runtime_error(
            "render pass has no physical target-plan scope: " +
            std::string{pass_name});
    }
    return *result;
}

GraphicsPipelineViewContract passViewContract(
    const VulkanTargetPlan *plan,
    const PassDefinition &pass) {
    if (plan == nullptr) return {};
    const auto &scope =
        requirePassScope(*plan, pass.name);
    if (scope.view_execution !=
        VulkanScopeViewExecution::multiview) {
        return {};
    }
    if (!pass.isFullscreen() &&
        !pass.isGenericRaster()) {
        throw std::runtime_error(
            "physical multiview scope selected an unsupported production "
            "pass implementation: " +
            pass.name);
    }
    const auto view =
        GraphicsPipelineViewContract::multiview(
            scope.view_count);
    if (view.view_mask != scope.view_mask) {
        throw std::runtime_error(
            "physical multiview scope and graphics pipeline view masks "
            "do not match: " +
            pass.name);
    }
    return view;
}

std::uint32_t passLogicalViewCount(
    const VulkanTargetPlan *plan,
    const PassDefinition &pass) {
    if (plan == nullptr) return 1;
    const auto &scope =
        requirePassScope(*plan, pass.name);
    if (scope.view_count == 0) {
        throw std::runtime_error(
            "physical scope has a zero logical view count: " +
            pass.name);
    }
    return scope.view_count;
}

PassInputViewDimension inputViewDimension(
    const VulkanTargetPlan *plan,
    std::string_view resource) {
    if (plan == nullptr) {
        return PassInputViewDimension::shared_2d;
    }
    const auto found = std::find_if(
        plan->resources.begin(), plan->resources.end(),
        [&](const VulkanPhysicalResourcePlan &candidate) {
            return candidate.logical_resource == resource;
        });
    if (found == plan->resources.end()) {
        throw std::runtime_error(
            "render-pass input is absent from the physical target plan: " +
            std::string{resource});
    }
    switch (found->view_layout) {
    case VulkanResourceViewLayout::shared_2d:
        return PassInputViewDimension::shared_2d;
    case VulkanResourceViewLayout::sequential_2d:
        return PassInputViewDimension::sequential_2d;
    case VulkanResourceViewLayout::layered_2d_array:
        return PassInputViewDimension::layered_2d_array;
    case VulkanResourceViewLayout::family_2d_array:
        return PassInputViewDimension::family_2d_array;
    }
    throw std::runtime_error(
        "unknown physical input view layout");
}

vk::AttachmentLoadOp runtimeAttachmentLoadOp(
    VulkanPhysicalAttachmentLoadOp op) {
    switch (op) {
    case VulkanPhysicalAttachmentLoadOp::load:
        return vk::AttachmentLoadOp::eLoad;
    case VulkanPhysicalAttachmentLoadOp::clear:
        return vk::AttachmentLoadOp::eClear;
    case VulkanPhysicalAttachmentLoadOp::discard:
        return vk::AttachmentLoadOp::eDontCare;
    }
    throw std::runtime_error(
        "unknown physical attachment load op");
}

vk::AttachmentStoreOp runtimeAttachmentStoreOp(
    VulkanPhysicalAttachmentStoreOp op) {
    switch (op) {
    case VulkanPhysicalAttachmentStoreOp::store:
        return vk::AttachmentStoreOp::eStore;
    case VulkanPhysicalAttachmentStoreOp::discard:
        return vk::AttachmentStoreOp::eDontCare;
    }
    throw std::runtime_error(
        "unknown physical attachment store op");
}

std::string physicalTargetName(
    GlobalRenderTargetId target,
    const RenderTargetMetadataResolver *metadata) {
    if (isSwapchainRenderTarget(target)) {
        return "swapchain";
    }
    if (!isConcreteRenderTarget(target)) {
        throw std::runtime_error(
            "physical attachment contract references no render target");
    }
    if (metadata == nullptr) {
        throw std::runtime_error(
            "physical attachment contract requires render-target "
            "metadata");
    }
    return metadata->get(target).name;
}

PassAttachmentOperations physicalAttachmentOperations(
    const VulkanTargetPlan &plan,
    std::string_view pass,
    std::string_view resource,
    const std::optional<ImageSubresourceRange>
        &subresource,
    VulkanPhysicalAttachmentAspect aspect) {
    const VulkanPhysicalAttachmentPlan *result =
        nullptr;
    for (const auto &attachment :
         plan.attachments) {
        if (attachment.node != pass ||
            attachment.logical_resource !=
                resource ||
            attachment.subresource !=
                subresource) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "physical target plan has duplicate attachment: " +
                std::string{pass} + " -> " +
                std::string{resource});
        }
        result = &attachment;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "render pass attachment is absent from the physical "
            "target plan: " +
            std::string{pass} + " -> " +
            std::string{resource});
    }
    if (result->aspect != aspect) {
        throw std::runtime_error(
            "render pass attachment aspect disagrees with the "
            "physical target plan: " +
            std::string{pass} + " -> " +
            std::string{resource});
    }
    return {
        runtimeAttachmentLoadOp(
            result->load_op),
        runtimeAttachmentStoreOp(
            result->store_op),
    };
}

PassDefinition applyPhysicalPassContract(
    const PassDefinition &source,
    const VulkanTargetPlan *plan,
    const RenderTargetMetadataResolver *metadata,
    FrameGraphResourceContainer *frame_graph_resources) {
    auto result = source;
    result.physical_color_attachment_operations
        .clear();
    result.physical_color_numeric_classes.clear();
    result.physical_depth_attachment_operations
        .reset();
    if (metadata != nullptr) {
        result.physical_color_numeric_classes.reserve(
            result.output_color.size());
        for (const auto target : result.output_color) {
            if (isConcreteRenderTarget(target)) {
                const auto format_name =
                    vk::to_string(
                        metadata->get(target).format);
                result.physical_color_numeric_classes
                    .push_back(
                        format_name.ends_with("Sint")
                            ? MaterialOutputNumericClass::
                                  signed_integer
                            : format_name.ends_with("Uint")
                                  ? MaterialOutputNumericClass::
                                        unsigned_integer
                                  : MaterialOutputNumericClass::
                                        floating);
            } else {
                // Window/XR composition color formats are normalized
                // floating-point outputs at the shader interface.
                result.physical_color_numeric_classes
                    .push_back(
                        MaterialOutputNumericClass::
                            floating);
            }
        }
    }
    result.input_target_views.clear();
    result.input_target_views.reserve(
        result.input_targets.size());
    for (const auto target : result.input_targets) {
        if (!isConcreteRenderTarget(target) ||
            metadata == nullptr) {
            result.input_target_views.push_back(
                PassInputViewDimension::shared_2d);
            continue;
        }
        result.input_target_views.push_back(
            inputViewDimension(
                plan, metadata->get(target).name));
    }
    if (result.isMaterial()) {
        for (auto &resource :
             result.materialInfo().material_resources) {
            if (!resource.isBuffer()) continue;
            if (frame_graph_resources == nullptr) {
                throw std::runtime_error(
                    "material resource '" +
                    resource.port.name +
                    "' requires frame-graph buffer runtime dependencies");
            }
            resource.buffer_id =
                frame_graph_resources->getBufferIdByName(
                    resource.buffer);
            if (!isValidFrameGraphBufferId(
                    resource.buffer_id)) {
                throw std::runtime_error(
                    "material resource '" +
                    resource.port.name +
                    "' buffer is absent from the active GPU generation: " +
                    resource.buffer);
            }
        }
        if (auto &draw_source =
                result.materialInfo()
                    .gpu_draw_source) {
            if (frame_graph_resources == nullptr) {
                throw std::runtime_error(
                    "material pass '" +
                    result.name +
                    "' gpu_draw_source requires frame-graph buffer "
                    "runtime dependencies");
            }
            draw_source->commands_id =
                frame_graph_resources
                    ->getBufferIdByName(
                        draw_source->commands);
            draw_source->count_id =
                frame_graph_resources
                    ->getBufferIdByName(
                        draw_source->count);
            if (draw_source->layout ==
                GpuDrawSourceLayout::
                    draw_queue_segments_v1) {
                draw_source->segments_id =
                    frame_graph_resources
                        ->getBufferIdByName(
                            draw_source
                                ->segments);
            }
            if (!isValidFrameGraphBufferId(
                    draw_source->commands_id) ||
                !isValidFrameGraphBufferId(
                    draw_source->count_id) ||
                (draw_source->layout ==
                     GpuDrawSourceLayout::
                         draw_queue_segments_v1 &&
                 !isValidFrameGraphBufferId(
                     draw_source
                         ->segments_id))) {
                throw std::runtime_error(
                    "material pass '" +
                    result.name +
                    "' gpu_draw_source buffer is absent from the active "
                    "GPU generation");
            }
            const auto &commands =
                frame_graph_resources->definition(
                    draw_source->commands_id);
            const auto &count =
                frame_graph_resources->definition(
                    draw_source->count_id);
            if (commands.command_layout !=
                    FrameGraphBufferCommandLayout::
                        indexed_draw ||
                count.command_layout !=
                    FrameGraphBufferCommandLayout::
                        draw_count) {
                throw std::runtime_error(
                    "material pass '" +
                    result.name +
                    "' gpu_draw_source runtime command layout changed");
            }
            if (draw_source->layout ==
                GpuDrawSourceLayout::
                    draw_queue_segments_v1) {
                const auto &segments =
                    frame_graph_resources
                        ->definition(
                            draw_source
                                ->segments_id);
                if (segments.host_source !=
                    FrameGraphHostBufferSource::
                        scene_draw_segments_v1) {
                    throw std::runtime_error(
                        "material pass '" +
                        result.name +
                        "' gpu_draw_source runtime segment layout changed");
                }
            }
            const auto command_bytes =
                static_cast<vk::DeviceSize>(
                    draw_source
                        ->max_draw_count) *
                frameGraphIndexedDrawCommandBytes;
            if (draw_source->command_offset >
                    commands.size ||
                commands.size -
                        draw_source->command_offset <
                    command_bytes ||
                draw_source->count_offset >
                    count.size ||
                count.size -
                        draw_source->count_offset <
                    frameGraphDrawCountBytes) {
                throw std::runtime_error(
                    "material pass '" +
                    result.name +
                    "' gpu_draw_source runtime range is invalid");
            }
        }
    }
    if (plan == nullptr ||
        plan->attachments.empty()) {
        return result;
    }
    result.physical_color_attachment_operations
        .reserve(result.output_color.size());
    for (const auto target :
         result.output_color) {
        result.physical_color_attachment_operations
            .push_back(
                physicalAttachmentOperations(
                    *plan, result.name,
                    physicalTargetName(
                        target, metadata),
                    target.subresource,
                    VulkanPhysicalAttachmentAspect::
                        color));
    }
    if (isSwapchainRenderTarget(
            result.output_depth) ||
        isConcreteRenderTarget(
            result.output_depth)) {
        result.physical_depth_attachment_operations =
            physicalAttachmentOperations(
                *plan, result.name,
                physicalTargetName(
                    result.output_depth,
                    metadata),
                result.output_depth.subresource,
                VulkanPhysicalAttachmentAspect::
                    depth);
    }
    return result;
}

bool containsTarget(
    std::span<const RasterAttachmentView> targets,
    const RasterAttachmentView &target) {
    return std::find(
               targets.begin(), targets.end(),
               target) != targets.end();
}

PassAttachmentOperations scopeColorAttachmentOperations(
    const PassDefinition &pass, std::size_t color_index,
    const VulkanTargetPlan &plan,
    const RenderTargetMetadataResolver *metadata) {
    if (plan.attachments.empty()) {
        return pass.colorAttachmentOperations(
            color_index);
    }
    return physicalAttachmentOperations(
        plan, pass.name,
        physicalTargetName(
            pass.output_color.at(color_index),
            metadata),
        pass.output_color.at(color_index)
            .subresource,
        VulkanPhysicalAttachmentAspect::color);
}

PassAttachmentOperations scopeDepthAttachmentOperations(
    const PassDefinition &pass,
    const VulkanTargetPlan &plan,
    const RenderTargetMetadataResolver *metadata) {
    if (plan.attachments.empty()) {
        return pass.depthAttachmentOperations();
    }
    return physicalAttachmentOperations(
        plan, pass.name,
        physicalTargetName(
            pass.output_depth, metadata),
        pass.output_depth.subresource,
        VulkanPhysicalAttachmentAspect::depth);
}

const VulkanPhysicalResourcePlan &
requirePhysicalPassResource(
    const VulkanTargetPlan &plan,
    GlobalRenderTargetId target,
    const RenderTargetMetadataResolver *metadata) {
    const auto name =
        physicalTargetName(target, metadata);
    const auto found = std::find_if(
        plan.resources.begin(), plan.resources.end(),
        [&](const auto &resource) {
            return resource.logical_resource == name;
        });
    if (found == plan.resources.end()) {
        throw std::runtime_error(
            "render-pass resource is absent from the physical "
            "target plan: " +
            name);
    }
    return *found;
}

bool hasMaterializedAttachmentOperations(
    const VulkanPhysicalResourcePlan &resource) {
    return resource.representation ==
               VulkanResourceRepresentation::materialized_image ||
           resource.representation ==
               VulkanResourceRepresentation::external;
}

CompiledPassRenderingContract
compilePassRenderingContract(
    const RenderingPassDefinition &definition,
    const PassDefinition &pass,
    const VulkanTargetPlan *plan,
    const RenderTargetMetadataResolver *metadata) {
    if (plan == nullptr) {
        return {};
    }
    const auto scope_index =
        requirePassScopeIndex(*plan, pass.name);
    const auto &scope = plan->scopes[scope_index];
    // output_transform is a graphics pass whose logical/physical kind marks
    // the terminal presentation boundary. It still owns dynamic-rendering
    // attachments and therefore uses the same concrete attachment contract
    // as an ordinary rendering scope.
    if (scope.kind !=
            VulkanPhysicalScopeKind::rendering &&
        scope.kind !=
            VulkanPhysicalScopeKind::output) {
        throw std::runtime_error(
            "render pass belongs to a non-rendering physical scope: " +
            pass.name);
    }

    CompiledPassRenderingContract result;
    result.scope_index = scope_index;
    result.scope_id = scope.id;
    result.fused_rendering_scope =
        scope.single_rendering_instance;
    result.local_read_scope =
        !scope.local_reads.empty();
    if (result.local_read_scope &&
        !result.fused_rendering_scope) {
        throw std::runtime_error(
            "physical local-read scope is not marked as one "
            "rendering instance: " +
            scope.id);
    }

    std::vector<const PassDefinition *>
        scope_passes;
    scope_passes.reserve(scope.nodes.size());
    for (const auto &node_name : scope.nodes) {
        const auto node = std::find_if(
            definition.passes.begin(),
            definition.passes.end(),
            [&](const auto &candidate) {
                return candidate.name == node_name;
            });
        if (node == definition.passes.end()) {
            throw std::runtime_error(
                "physical rendering scope references a node absent "
                "from its rendering-pass definition: " +
                node_name);
        }
        scope_passes.push_back(&*node);
        for (const auto target :
             node->output_color) {
            if (!containsTarget(
                    result.color_attachments,
                    target)) {
                result.color_attachments.push_back(
                    target);
            }
        }
        if (isConcreteRenderTarget(
                node->output_depth) ||
            isSwapchainRenderTarget(
                node->output_depth)) {
            if (isConcreteRenderTarget(
                    result.depth_attachment) ||
                isSwapchainRenderTarget(
                    result.depth_attachment)) {
                if (result.depth_attachment !=
                    node->output_depth) {
                    throw std::runtime_error(
                        "physical rendering scope uses more than one "
                        "depth attachment: " +
                        scope.id);
                }
            } else {
                result.depth_attachment =
                    node->output_depth;
            }
        }
    }

    if (result.fused_rendering_scope) {
        if (scope_passes.size() < 2) {
            throw std::runtime_error(
                "physical fused rendering scope requires at least "
                "two passes: " +
                scope.id);
        }
        for (const auto *scope_pass : scope_passes) {
            if (scope_pass->isUi()
#if PELICAN_WITH_IMGUI
                || scope_pass->isImGui()
#endif
            ) {
                throw std::runtime_error(
                    "UI pass implementation cannot execute inside "
                    "a fused physical rendering scope: " +
                    scope_pass->name);
            }
        }
        if (result.local_read_scope) {
            std::map<RasterAttachmentView,
                     PassAttachmentOperations>
                previous_color_operations;
            std::optional<PassAttachmentOperations>
                previous_depth_operations;
            for (const auto *scope_pass_ptr :
                 scope_passes) {
                const auto &scope_pass =
                    *scope_pass_ptr;
                for (std::size_t color_index = 0;
                     color_index <
                     scope_pass.output_color.size();
                     ++color_index) {
                    const auto target =
                        scope_pass
                            .output_color[color_index];
                    if (!hasMaterializedAttachmentOperations(
                            requirePhysicalPassResource(
                                *plan, target,
                            metadata))) {
                        continue;
                    }
                    const auto operations =
                        scopeColorAttachmentOperations(
                            scope_pass, color_index,
                            *plan, metadata);
                    const auto previous =
                        previous_color_operations.find(
                            target);
                    if (previous !=
                            previous_color_operations.end() &&
                        (previous->second.store_op !=
                             vk::AttachmentStoreOp::eStore ||
                         operations.load_op !=
                             vk::AttachmentLoadOp::eLoad)) {
                        throw std::runtime_error(
                            "local-read fused rendering scope cannot "
                            "preserve intermediate materialized "
                            "color attachment operations: " +
                            scope.id + " -> " +
                            physicalTargetName(
                                target, metadata));
                    }
                    previous_color_operations[
                        target] = operations;
                }

                if (isConcreteRenderTarget(
                        scope_pass.output_depth) ||
                    isSwapchainRenderTarget(
                        scope_pass.output_depth)) {
                    if (!hasMaterializedAttachmentOperations(
                            requirePhysicalPassResource(
                                *plan,
                                scope_pass.output_depth,
                                metadata))) {
                        continue;
                    }
                    const auto operations =
                        scopeDepthAttachmentOperations(
                            scope_pass, *plan,
                            metadata);
                    if (previous_depth_operations &&
                        (previous_depth_operations
                                 ->store_op !=
                             vk::AttachmentStoreOp::eStore ||
                         operations.load_op !=
                             vk::AttachmentLoadOp::eLoad)) {
                        throw std::runtime_error(
                            "local-read fused rendering scope cannot "
                            "preserve intermediate materialized "
                            "depth attachment operations: " +
                            scope.id);
                    }
                    previous_depth_operations =
                        operations;
                }
            }
        } else {
            if (std::any_of(
                    result.color_attachments.begin(),
                    result.color_attachments.end(),
                    [](GlobalRenderTargetId target) {
                        return !isConcreteRenderTarget(
                            target);
                    }) ||
                (isSwapchainRenderTarget(
                    result.depth_attachment))) {
                throw std::runtime_error(
                    "materialized fused rendering scope currently "
                    "requires internal concrete attachments: " +
                    scope.id);
            }
            for (std::size_t pass_index = 0;
                 pass_index < scope_passes.size();
                 ++pass_index) {
                const auto &scope_pass =
                    *scope_passes[pass_index];
                if (scope_pass.output_color !=
                        result.color_attachments ||
                    scope_pass.output_depth !=
                        result.depth_attachment) {
                    throw std::runtime_error(
                        "materialized fused rendering passes require "
                        "identical ordered attachments: " +
                        scope.id);
                }
                for (std::size_t color_index = 0;
                     color_index <
                     scope_pass.output_color.size();
                     ++color_index) {
                    const auto operations =
                        scopeColorAttachmentOperations(
                            scope_pass, color_index,
                            *plan, metadata);
                    if (pass_index > 0 &&
                        operations.load_op !=
                            vk::AttachmentLoadOp::eLoad) {
                        throw std::runtime_error(
                            "materialized fused rendering pass must "
                            "Load every attachment after the first "
                            "pass: " +
                            scope_pass.name);
                    }
                    if (pass_index + 1 <
                            scope_passes.size() &&
                        operations.store_op !=
                            vk::AttachmentStoreOp::eStore) {
                        throw std::runtime_error(
                            "materialized fused rendering pass must "
                            "Store every attachment before the final "
                            "pass: " +
                            scope_pass.name);
                    }
                }
                if (isConcreteRenderTarget(
                        scope_pass.output_depth) ||
                    isSwapchainRenderTarget(
                        scope_pass.output_depth)) {
                    const auto operations =
                        scopeDepthAttachmentOperations(
                            scope_pass, *plan,
                            metadata);
                    if (pass_index > 0 &&
                        operations.load_op !=
                            vk::AttachmentLoadOp::eLoad) {
                        throw std::runtime_error(
                            "materialized fused rendering pass must "
                            "Load depth after the first pass: " +
                            scope_pass.name);
                    }
                    if (pass_index + 1 <
                            scope_passes.size() &&
                        operations.store_op !=
                            vk::AttachmentStoreOp::eStore) {
                        throw std::runtime_error(
                            "materialized fused rendering pass must "
                            "Store depth before the final pass: " +
                            scope_pass.name);
                    }
                }
            }
        }
    }

    result.scope_color_attachment_operations
        .reserve(result.color_attachments.size());
    result.scope_color_clear_values.reserve(
        result.color_attachments.size());
    for (const auto target :
         result.color_attachments) {
        const PassDefinition *first_writer =
            nullptr;
        const PassDefinition *last_writer =
            nullptr;
        std::size_t first_color_index = 0;
        std::size_t last_color_index = 0;
        for (const auto *scope_pass :
             scope_passes) {
            for (std::size_t color_index = 0;
                 color_index <
                 scope_pass->output_color.size();
                 ++color_index) {
                if (scope_pass
                        ->output_color[color_index] !=
                    target) {
                    continue;
                }
                if (first_writer == nullptr) {
                    first_writer = scope_pass;
                    first_color_index = color_index;
                }
                last_writer = scope_pass;
                last_color_index = color_index;
            }
        }
        if (first_writer == nullptr ||
            last_writer == nullptr) {
            throw std::logic_error(
                "physical scope color attachment union has no "
                "writer");
        }
        const auto first_operations =
            scopeColorAttachmentOperations(
                *first_writer, first_color_index,
                *plan, metadata);
        const auto last_operations =
            scopeColorAttachmentOperations(
                *last_writer, last_color_index,
                *plan, metadata);
        result.scope_color_attachment_operations
            .push_back(PassAttachmentOperations{
                first_operations.load_op,
                last_operations.store_op});
        result.scope_color_clear_values.push_back(
            first_writer->colorClearValue(
                first_color_index));
    }

    if (isConcreteRenderTarget(
            result.depth_attachment) ||
        isSwapchainRenderTarget(
            result.depth_attachment)) {
        const PassDefinition *first_writer =
            nullptr;
        const PassDefinition *last_writer =
            nullptr;
        for (const auto *scope_pass :
             scope_passes) {
            if (scope_pass->output_depth !=
                result.depth_attachment) {
                continue;
            }
            if (first_writer == nullptr) {
                first_writer = scope_pass;
            }
            last_writer = scope_pass;
        }
        if (first_writer == nullptr ||
            last_writer == nullptr) {
            throw std::logic_error(
                "physical scope depth attachment union has no "
                "writer");
        }
        const auto first_operations =
            scopeDepthAttachmentOperations(
                *first_writer, *plan, metadata);
        const auto last_operations =
            scopeDepthAttachmentOperations(
                *last_writer, *plan, metadata);
        result.scope_depth_attachment_operations =
            PassAttachmentOperations{
                first_operations.load_op,
                last_operations.store_op};
    }

    result.color_attachment_locations.assign(
        result.color_attachments.size(),
        unusedPhysicalAttachmentMapping);
    result.color_attachment_input_indices.assign(
        result.color_attachments.size(),
        unusedPhysicalAttachmentMapping);
    for (std::size_t location = 0;
         location < pass.output_color.size();
         ++location) {
        const auto found = std::find(
            result.color_attachments.begin(),
            result.color_attachments.end(),
            pass.output_color[location]);
        if (found ==
            result.color_attachments.end()) {
            throw std::runtime_error(
                "pass color output is absent from its physical "
                "scope attachment union: " +
                pass.name);
        }
        result.color_attachment_locations[
            static_cast<std::size_t>(
                found -
                result.color_attachments.begin())] =
            static_cast<std::uint32_t>(location);
    }

    for (std::size_t input_index = 0;
         input_index < pass.input_targets.size();
         ++input_index) {
        const auto target =
            pass.input_targets[input_index];
        const auto &resource =
            requirePhysicalPassResource(
                *plan, target, metadata);
        if (resource.representation !=
            VulkanResourceRepresentation::
                tile_local_attachment) {
            continue;
        }
        if (std::find(
                scope.local_reads.begin(),
                scope.local_reads.end(),
                resource.logical_resource) ==
            scope.local_reads.end()) {
            throw std::runtime_error(
                "tile-local pass input is not declared by its "
                "physical rendering scope: " +
                pass.name + " -> " +
                resource.logical_resource);
        }
        const auto color = std::find_if(
            result.color_attachments.begin(),
            result.color_attachments.end(),
            [&](const auto &attachment) {
                return attachment.target.value ==
                       target.value;
            });
        if (color !=
            result.color_attachments.end()) {
            result.color_attachment_input_indices[
                static_cast<std::size_t>(
                    color -
                    result.color_attachments.begin())] =
                static_cast<std::uint32_t>(
                    input_index);
            continue;
        }
        if (target.value ==
            result.depth_attachment.target.value) {
            result.depth_attachment_input_index =
                static_cast<std::uint32_t>(
                    input_index);
            continue;
        }
        throw std::runtime_error(
            "tile-local pass input is absent from its physical "
            "scope attachment union: " +
            pass.name + " -> " +
            resource.logical_resource);
    }
    return result;
}

void appendViewShaderDefines(
    std::vector<std::string> &defines,
    const PassDefinition &pass,
    const GraphicsPipelineViewContract &view) {
    if (view.execution ==
        GraphicsPipelineViewExecution::multiview) {
        defines.push_back("PELICAN_MULTIVIEW=1");
        defines.push_back(
            "PELICAN_VIEW_COUNT=" +
            std::to_string(view.view_count));
    }
    for (std::size_t binding = 0;
         binding < pass.input_target_views.size();
         ++binding) {
        const auto dimension =
            pass.input_target_views[binding];
        if ((view.execution ==
                 GraphicsPipelineViewExecution::multiview &&
             dimension ==
                 PassInputViewDimension::layered_2d_array) ||
            dimension ==
                PassInputViewDimension::family_2d_array) {
            defines.push_back(
                "PELICAN_INPUT_" +
                std::to_string(binding) +
                "_LAYERED=1");
        }
    }
}

std::vector<bool> localReadInputMask(
    const PassDefinition &pass,
    const CompiledPassRenderingContract
        &rendering) {
    std::vector<bool> result(
        pass.input_targets.size(), false);
    const auto mark =
        [&](std::uint32_t input_index) {
            if (input_index ==
                unusedPhysicalAttachmentMapping) {
                return;
            }
            if (input_index >= result.size()) {
                throw std::runtime_error(
                    "physical local-read input index is outside the "
                    "pass input contract: " +
                    pass.name);
            }
            result[input_index] = true;
        };
    for (const auto input_index :
         rendering
             .color_attachment_input_indices) {
        mark(input_index);
    }
    mark(rendering
             .depth_attachment_input_index);
    return result;
}

void appendLocalReadShaderDefines(
    std::vector<std::string> &defines,
    const PassDefinition &pass,
    const CompiledPassRenderingContract
        &rendering) {
    const auto local_reads =
        localReadInputMask(pass, rendering);
    for (std::size_t input = 0;
         input < local_reads.size(); ++input) {
        if (local_reads[input]) {
            defines.push_back(
                "PELICAN_INPUT_" +
                std::to_string(input) +
                "_LOCAL_READ=1");
        }
    }
}

void validatePassInputViewContract(
    const PassDefinition &pass,
    const GraphicsPipelineViewContract &view) {
    if (view.execution !=
        GraphicsPipelineViewExecution::multiview) {
        return;
    }
    if (pass.input_target_views.size() !=
        pass.input_targets.size()) {
        throw std::runtime_error(
            "multiview pass input view metadata is incomplete: " +
            pass.name);
    }
    if (std::find(
            pass.input_target_views.begin(),
            pass.input_target_views.end(),
            PassInputViewDimension::sequential_2d) !=
        pass.input_target_views.end()) {
        throw std::runtime_error(
            "multiview pass cannot consume a sequential-only input: " +
            pass.name);
    }
}

vk::Extent2D rasterAttachmentExtent(
    vk::Extent2D extent,
    const std::optional<ImageSubresourceRange>
        &subresource) {
    if (!subresource) return extent;
    const auto mip = subresource->base_mip_level;
    return {
        std::max(1u, extent.width >> mip),
        std::max(1u, extent.height >> mip),
    };
}

void validatePassAttachmentViews(
    const PassDefinition &pass,
    const RenderTargetMetadataResolver *metadata,
    std::uint32_t logical_view_count) {
    if (logical_view_count == 0) {
        throw std::runtime_error(
            "render pass attachment validation requires a "
            "non-zero logical view count: " +
            pass.name);
    }
    std::optional<vk::Extent2D> concrete_extent;
    std::vector<GlobalRenderTargetId> seen_targets;
    const auto validate =
        [&](const RasterAttachmentView &attachment,
            std::string_view role) {
            if (!isConcreteRenderTarget(
                    attachment.target)) {
                if (attachment.subresource) {
                    throw std::runtime_error(
                        std::string{role} +
                        " attachment cannot select a subresource "
                        "of a special target: " +
                        pass.name);
                }
                return;
            }
            if (metadata == nullptr) {
                if (attachment.subresource) {
                    throw std::runtime_error(
                        "render pass subresource attachment "
                        "requires render-target metadata: " +
                        pass.name);
                }
                return;
            }
            if (std::find(
                    seen_targets.begin(),
                    seen_targets.end(),
                    attachment.target) !=
                seen_targets.end()) {
                throw std::runtime_error(
                    "render pass cannot bind the same render "
                    "target to more than one attachment slot: " +
                    pass.name);
            }
            seen_targets.push_back(
                attachment.target);

            const auto target =
                metadata->get(attachment.target);
            if (attachment.subresource) {
                const auto &range =
                    *attachment.subresource;
                if (range.mip_count_mode !=
                        ImageSubresourceMipCountMode::fixed ||
                    range.level_count != 1 ||
                    !validImageSubresourceRange(
                        range, target.mip_levels,
                        target.array_layers)) {
                    throw std::runtime_error(
                        std::string{role} +
                        " attachment has an out-of-range or "
                        "multi-mip subresource: " +
                        pass.name + " -> " +
                        target.name);
                }
                if (range.layer_count !=
                    logical_view_count) {
                    throw std::runtime_error(
                        std::string{role} +
                        " attachment layer_count must match the "
                        "logical view count: " +
                        pass.name + " -> " +
                        target.name);
                }
                if (target.samples > 1 &&
                    range.base_mip_level != 0) {
                    throw std::runtime_error(
                        std::string{role} +
                        " attachment cannot select a non-zero mip "
                        "from an MSAA render target: " +
                        pass.name + " -> " +
                        target.name);
                }
            }
            const auto extent =
                rasterAttachmentExtent(
                    target.extent,
                    attachment.subresource);
            if (concrete_extent &&
                *concrete_extent != extent) {
                throw std::runtime_error(
                    "render pass attachments select different "
                    "raster extents: " +
                    pass.name);
            }
            concrete_extent = extent;
        };

    for (const auto &attachment :
         pass.output_color) {
        validate(attachment, "Color");
    }
    validate(pass.output_depth, "Depth");
}

vk::Format resolveFirstColorFormat(const PassDefinition &pass_def, RenderTarget &rt_module,
                                   const RenderTargetMetadataResolver &rt_metadata) {
    if (pass_def.output_color.empty()) {
        throw std::runtime_error("Render pass has no color output: " + pass_def.name);
    }

    const auto &first_color = pass_def.output_color.front();
    if (isConcreteRenderTarget(first_color)) {
        return rt_metadata.get(first_color).format;
    }
    return rt_module.getSwapchainFormat();
}

vk::Format resolvePhysicalColorFormat(
    GlobalRenderTargetId target,
    RenderTarget &render_target,
    const RenderTargetMetadataResolver
        &metadata) {
    if (isConcreteRenderTarget(target)) {
        return metadata.get(target).format;
    }
    if (isSwapchainRenderTarget(target)) {
        return render_target
            .getSwapchainFormat();
    }
    throw std::runtime_error(
        "physical rendering scope has an invalid color target");
}

std::vector<vk::Format>
resolvePhysicalColorFormats(
    const PassDefinition &pass,
    const CompiledPassRenderingContract
        &rendering,
    RenderTarget &render_target,
    const RenderTargetMetadataResolver
        &metadata) {
    if (rendering.color_attachments.empty()) {
        if (pass.output_color.empty()) {
            return {};
        }
        return {resolveFirstColorFormat(
            pass, render_target, metadata)};
    }
    std::vector<vk::Format> result;
    result.reserve(
        rendering.color_attachments.size());
    for (const auto target :
         rendering.color_attachments) {
        result.push_back(
            resolvePhysicalColorFormat(
                target, render_target,
                metadata));
    }
    return result;
}

std::optional<vk::Format>
resolvePhysicalDepthFormat(
    const CompiledPassRenderingContract
        &rendering,
    const RenderTargetMetadataResolver
        &metadata) {
    if (!isConcreteRenderTarget(
            rendering.depth_attachment)) {
        return std::nullopt;
    }
    return metadata
        .get(rendering.depth_attachment)
        .format;
}

GraphicsPipelineRenderingLocalReadContract
graphicsLocalReadContract(
    const CompiledPassRenderingContract
        &rendering) {
    GraphicsPipelineRenderingLocalReadContract result;
    result.enabled =
        rendering.local_read_scope;
    if (!result.enabled) {
        return result;
    }
    result.color_attachment_locations =
        rendering.color_attachment_locations;
    result.color_attachment_input_indices =
        rendering
            .color_attachment_input_indices;
    result.depth_attachment_input_index =
        rendering.depth_attachment_input_index;
    return result;
}

PassId passIndexToPassId(size_t pass_index) {
    if (pass_index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Rendering pass index is too large");
    }
    return PassId{static_cast<int>(pass_index)};
}

PassId fullscreenPipelineValueToPassId(uint32_t pipeline_value) {
    if (pipeline_value > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Fullscreen pipeline id is too large");
    }
    return PassId{static_cast<int>(pipeline_value)};
}

FullscreenRuntimeDependencies requireFullscreenDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies) {
    if (dependencies.render_target == nullptr || dependencies.render_target_metadata == nullptr ||
        dependencies.render_target_views == nullptr || dependencies.shader_library == nullptr ||
        dependencies.fullscreen_pass_container == nullptr || dependencies.frame_graph_resources == nullptr ||
        dependencies.path_resolver == nullptr) {
        throw std::runtime_error(
            "Fullscreen pass runtime compile requires render target, render target metadata, render target views, "
            "shader library, fullscreen pass container, frame graph resources, and path resolver dependencies: " +
            pass_def.name);
    }
    return FullscreenRuntimeDependencies{
        *dependencies.render_target,
        *dependencies.render_target_metadata,
        *dependencies.render_target_views,
        *dependencies.shader_library,
        *dependencies.fullscreen_pass_container,
        *dependencies.frame_graph_resources,
        *dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
    };
}

DebugDrawRuntimeDependencies requireDebugDrawDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies) {
    if (dependencies.render_target == nullptr || dependencies.render_target_metadata == nullptr ||
        dependencies.shader_library == nullptr || dependencies.path_resolver == nullptr ||
        !dependencies.debug_draw_provider) {
        throw std::runtime_error(
            "DebugDraw pass runtime compile requires render target, render target metadata, "
            "shader library, path resolver, and debug draw dependencies: " +
            pass_def.name);
    }
    return DebugDrawRuntimeDependencies{
        *dependencies.render_target,
        *dependencies.render_target_metadata,
        *dependencies.shader_library,
        dependencies.debug_draw_provider(),
        *dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
    };
}

DebugTextRuntimeDependencies requireDebugTextDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies) {
    if (dependencies.render_target == nullptr || dependencies.render_target_metadata == nullptr ||
        dependencies.shader_library == nullptr || dependencies.path_resolver == nullptr ||
        !dependencies.debug_text_provider) {
        throw std::runtime_error(
            "DebugText pass runtime compile requires render target, render target metadata, "
            "shader library, path resolver, and debug text dependencies: " +
            pass_def.name);
    }
    return DebugTextRuntimeDependencies{
        *dependencies.render_target,
        *dependencies.render_target_metadata,
        *dependencies.shader_library,
        dependencies.debug_text_provider(),
        *dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
    };
}

GizmoRuntimeDependencies requireGizmoDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies) {
    if (dependencies.render_target == nullptr ||
        dependencies.render_target_metadata == nullptr ||
        dependencies.shader_library == nullptr ||
        dependencies.path_resolver == nullptr || !dependencies.gizmo_provider) {
        throw std::runtime_error(
            "Gizmo pass runtime compile requires render target, render target metadata, shader library, path resolver, and gizmo dependencies: " +
            pass_def.name);
    }
    return GizmoRuntimeDependencies{
        *dependencies.render_target,
        *dependencies.render_target_metadata,
        *dependencies.shader_library,
        dependencies.gizmo_provider(),
        *dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
    };
}

ShadowDepthRuntimeDependencies requireShadowDepthDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies) {
    if (dependencies.render_target_metadata == nullptr || dependencies.shader_library == nullptr ||
        dependencies.shadow_depth_pass_container == nullptr || dependencies.path_resolver == nullptr) {
        throw std::runtime_error(
            "Shadow depth pass runtime compile requires render target metadata, shader library, "
            "shadow depth pass container, and path resolver dependencies: " +
            pass_def.name);
    }
    return ShadowDepthRuntimeDependencies{
        *dependencies.render_target_metadata,
        *dependencies.shader_library,
        *dependencies.shadow_depth_pass_container,
        *dependencies.path_resolver,
        dependencies.shader_defines,
        dependencies.warn_backend_specific_shader_refs,
    };
}

GeometryPassRuntimeDependencies requireGeometryPassDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies,
    std::string_view pass_kind) {
    if (dependencies.render_target_metadata == nullptr || dependencies.shader_library == nullptr ||
        dependencies.velocity_pass_container == nullptr || dependencies.path_resolver == nullptr) {
        throw std::runtime_error(
            std::string{pass_kind} +
            " pass runtime compile dependencies are unavailable: " +
            pass_def.name);
    }
    return {*dependencies.render_target_metadata, *dependencies.shader_library,
            *dependencies.velocity_pass_container, *dependencies.path_resolver,
            dependencies.shader_defines, dependencies.warn_backend_specific_shader_refs};
}

void warnBackendSpecificShaderReference(const ShaderReference &reference, bool enabled) {
    if (!enabled || !reference.backend_specific) {
        return;
    }
    LOG_WARNING(logger, "backend-specific shader reference is not portable; use a stem reference: {}",
                reference.ref);
}

ShaderBundleId registerShaderReference(ShaderLibrary &shader_library, const PathResolver &path_resolver,
                                       const ShaderReference &reference, bool warn_backend_specific,
                                       const std::vector<std::string> &shader_defines,
                                       std::vector<std::pair<std::string, std::string>>
                                           virtual_includes = {}) {
    warnBackendSpecificShaderReference(reference, warn_backend_specific);
    return shader_library.loadFromReference(
        reference, path_resolver,
        warn_backend_specific, shader_defines,
        std::move(virtual_includes));
}

struct CompiledFullscreenResourceInterface {
    std::vector<ShaderResourceInterfaceBinding> bindings;
    std::vector<FullscreenInputSampling> sampling;
    std::vector<std::optional<ImageSubresourceRange>>
        subresources;
    std::vector<ImageSubresourceViewDimension>
        view_dimensions;
};

VulkanResourceViewLayout physicalViewLayout(
    PassInputViewDimension view) {
    switch (view) {
    case PassInputViewDimension::shared_2d:
        return VulkanResourceViewLayout::shared_2d;
    case PassInputViewDimension::sequential_2d:
        return VulkanResourceViewLayout::sequential_2d;
    case PassInputViewDimension::layered_2d_array:
        return VulkanResourceViewLayout::layered_2d_array;
    case PassInputViewDimension::family_2d_array:
        return VulkanResourceViewLayout::family_2d_array;
    }
    throw std::runtime_error(
        "unknown fullscreen input view dimension");
}

FullscreenInputSampling fullscreenSampling(
    ShaderResourcePortSampling sampling) {
    return {
        sampling.filter ==
                ShaderResourcePortFilter::nearest
            ? FullscreenInputFilter::nearest
            : FullscreenInputFilter::linear,
        sampling.address_mode ==
                ShaderResourcePortAddressMode::repeat
            ? FullscreenInputAddressMode::repeat
        : sampling.address_mode ==
                ShaderResourcePortAddressMode::
                    mirrored_repeat
            ? FullscreenInputAddressMode::
                  mirrored_repeat
            : FullscreenInputAddressMode::
                  clamp_to_edge,
    };
}

void validateFullscreenSamplingCapabilities(
    const PassDefinition &pass,
    const FullscreenRuntimeDependencies &dependencies,
    std::span<const FullscreenInputSampling> sampling,
    const std::vector<bool> &local_reads) {
    if (sampling.size() !=
            pass.input_targets.size() ||
        local_reads.size() !=
            pass.input_targets.size()) {
        throw std::runtime_error(
            "fullscreen input sampling capability validation "
            "received inconsistent metadata: " +
            pass.name);
    }
    const auto physical_device =
        GET_MODULE(VulkanManageCore)
            .getPhysDevice();
    for (std::size_t input = 0;
         input < pass.input_targets.size(); ++input) {
        if (local_reads[input]) continue;
        const auto metadata =
            dependencies.render_target_metadata.get(
                pass.input_targets[input]);
        if (!(metadata.usage &
              vk::ImageUsageFlagBits::eSampled)) {
            throw std::runtime_error(
                "Fullscreen pass sampled input '" +
                metadata.name +
                "' lacks SAMPLED usage: " +
                pass.name);
        }
        if (sampling[input].filter !=
            FullscreenInputFilter::linear) {
            continue;
        }
        const auto features =
            physical_device
                .getFormatProperties(
                    metadata.format)
                .optimalTilingFeatures;
        if (!(features &
              vk::FormatFeatureFlagBits::
                  eSampledImageFilterLinear)) {
            throw std::runtime_error(
                std::string{
                    pass.isGenericRaster()
                        ? "Raster"
                        : "Fullscreen"} +
                " pass input '" +
                metadata.name + "' format " +
                vk::to_string(metadata.format) +
                " does not support linear filtering; author " +
                std::string{
                    pass.isGenericRaster()
                        ? "resource_ports sampling.filter 'nearest': "
                        : "input_sampling filter 'nearest': "} +
                pass.name);
        }
    }
}

CompiledFullscreenResourceInterface
compileFullscreenResourceInterface(
    const PassDefinition &pass,
    const FullscreenRuntimeDependencies &dependencies,
    GraphicsPipelineViewContract view,
    std::uint32_t logical_view_count,
    const CompiledPassRenderingContract &rendering) {
    CompiledFullscreenResourceInterface result;
    const auto &ports =
        pass.isGenericRaster()
            ? pass.genericRasterInfo()
                  .resource_ports
            : pass.fullscreenInfo()
                  .resource_ports;
    const auto local_reads =
        localReadInputMask(pass, rendering);
    if (ports.empty()) {
        if (pass.isFullscreen() &&
            !pass.fullscreenInfo()
                 .input_sampling.empty()) {
            result.sampling =
                pass.fullscreenInfo()
                    .input_sampling;
        } else {
            result.sampling =
                std::vector<
                    FullscreenInputSampling>(
                    pass.input_targets.size());
        }
        validateFullscreenSamplingCapabilities(
            pass, dependencies,
            result.sampling, local_reads);
        return result;
    }

    result.bindings.reserve(ports.size());
    result.sampling.resize(
        pass.input_targets.size());
    result.subresources.resize(
        pass.input_targets.size());
    result.view_dimensions.resize(
        pass.input_targets.size(),
        ImageSubresourceViewDimension::two_d);
    for (std::size_t input = 0;
         input < pass.input_target_views.size() &&
         input < result.view_dimensions.size();
         ++input) {
        if (pass.input_target_views[input] ==
                PassInputViewDimension::
                    layered_2d_array ||
            pass.input_target_views[input] ==
                PassInputViewDimension::
                    family_2d_array) {
            result.view_dimensions[input] =
                ImageSubresourceViewDimension::
                    two_d_array;
        }
    }
    std::size_t matched_ports = 0;
    for (std::size_t input = 0;
         input < pass.input_targets.size(); ++input) {
        const auto metadata =
            dependencies.render_target_metadata.get(
                pass.input_targets[input]);
        auto authored_resource = metadata.name;
        if (pass.input_target_history[input]) {
            authored_resource += "@history";
        }
        const auto port = std::find_if(
            ports.begin(), ports.end(),
            [&](const ShaderResourcePortDefinition
                    &candidate) {
                return candidate.resource ==
                       authored_resource;
            });
        if (port == ports.end()) continue;
        ++matched_ports;
        if (port->kind ==
            ShaderResourcePortKind::buffer) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') declares a buffer but resolves to an image");
        }
        if (!local_reads[input] &&
            !(metadata.usage &
              vk::ImageUsageFlagBits::eSampled)) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') requires sampled render-target usage");
        }
        if (local_reads[input]) {
            result.bindings.push_back(
                ShaderResourceInterfaceBinding{
                    .port = *port,
                    .binding =
                        static_cast<std::uint32_t>(
                            input),
                    .descriptor =
                        ShaderResourceDescriptorKind::
                            input_attachment,
                    .image_view_dimension =
                        ReflectedImageViewDimension::
                            two_d,
                    .input_attachment_index =
                        static_cast<std::uint32_t>(
                            input),
                    .expected_stages =
                        vk::ShaderStageFlagBits::
                            eFragment,
                    .readable = true,
                    .writable = false,
                });
            result.sampling[input] =
                fullscreenSampling(
                    port->sampling);
            result.subresources[input] =
                port->subresource;
            continue;
        }
        const auto physical =
            input < pass.input_target_views.size()
                ? physicalViewLayout(
                      pass.input_target_views[input])
                : VulkanResourceViewLayout::
                      shared_2d;
        const auto consumer =
            view.execution ==
                    GraphicsPipelineViewExecution::
                        multiview
                ? ShaderResourceConsumerView::
                      graphics_multiview
                : ShaderResourceConsumerView::
                      graphics_sequential;
        const auto dimension =
            resolveShaderResourceImageViewDimension(
                *port, physical, consumer,
                metadata.dimension);
        if (dimension ==
                ReflectedImageViewDimension::
                    two_d_array &&
            physical !=
                VulkanResourceViewLayout::
                    family_2d_array &&
            metadata.array_layers <
                logical_view_count) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') has too few physical array layers");
        }
        if (dimension ==
                ReflectedImageViewDimension::cube &&
            metadata.dimension !=
                ImageResourceDimension::cube) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') requires a cube render target");
        }
        if (port->subresource) {
            if (!validImageSubresourceRange(
                    *port->subresource,
                    metadata.mip_levels,
                    metadata.array_layers)) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port->name + "' (resource '" +
                    port->resource +
                    "') has an out-of-range image subresource");
            }
            if (dimension ==
                    ReflectedImageViewDimension::
                        two_d &&
                port->view ==
                    ShaderResourcePortView::shared_2d &&
                port->subresource->layer_count !=
                    1) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port->name + "' (resource '" +
                    port->resource +
                    "') 2D view requires exactly one array layer");
            }
            if (port->view ==
                    ShaderResourcePortView::per_view) {
                const auto layer_count =
                    port->subresource->layer_count;
                const auto valid_layer_count =
                    physical ==
                            VulkanResourceViewLayout::
                                sequential_2d
                        ? layer_count == 1 ||
                              layer_count ==
                                  logical_view_count
                        : layer_count ==
                              logical_view_count;
                if (!valid_layer_count) {
                    throw std::runtime_error(
                        "Shader resource port '" +
                        port->name + "' (resource '" +
                        port->resource +
                        "') per_view subresource does not match the "
                        "physical view layout");
                }
            }
            if (dimension ==
                    ReflectedImageViewDimension::cube &&
                (port->subresource
                         ->base_array_layer != 0 ||
                 port->subresource
                         ->layer_count != 6)) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    port->name + "' (resource '" +
                    port->resource +
                    "') cube view must select all six faces");
            }
        }
        result.bindings.push_back(
            ShaderResourceInterfaceBinding{
                .port = *port,
                .binding =
                    static_cast<std::uint32_t>(
                        input),
                .descriptor =
                    ShaderResourceDescriptorKind::
                        combined_image_sampler,
                .image_view_dimension = dimension,
                .readable = true,
                .writable = false,
            });
        result.sampling[input] =
            fullscreenSampling(port->sampling);
        result.subresources[input] =
            port->subresource;
        result.view_dimensions[input] =
            dimension ==
                    ReflectedImageViewDimension::cube
                ? ImageSubresourceViewDimension::cube
            : dimension ==
                    ReflectedImageViewDimension::
                        two_d_array
                ? ImageSubresourceViewDimension::
                      two_d_array
                : ImageSubresourceViewDimension::two_d;
    }
    for (std::size_t input = 0;
         input < pass.input_buffers.size(); ++input) {
        const auto &authored_resource =
            pass.input_buffers[input];
        const auto port = std::find_if(
            ports.begin(), ports.end(),
            [&](const ShaderResourcePortDefinition
                    &candidate) {
                return candidate.resource ==
                       authored_resource;
            });
        if (port == ports.end()) continue;
        ++matched_ports;
        if (port->kind !=
                ShaderResourcePortKind::buffer ||
            !port->buffer_element) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') resolves to a frame-graph buffer and requires "
                "kind 'buffer' plus an explicit element");
        }
        if (!dependencies.frame_graph_resources
                 .hasBuffer(authored_resource)) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') references an unavailable frame-graph buffer");
        }
        if (effectiveShaderResourcePortAccess(
                *port, false, false) !=
            ShaderResourcePortAccess::storage) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') requires storage access for a buffer");
        }
        result.bindings.push_back(
            ShaderResourceInterfaceBinding{
                .port = *port,
                .binding =
                    static_cast<std::uint32_t>(
                        pass.input_targets.size() +
                        input),
                .descriptor =
                    ShaderResourceDescriptorKind::
                        storage_buffer,
                .image_view_dimension =
                    ReflectedImageViewDimension::none,
                .buffer_element =
                    *port->buffer_element,
                .readable = true,
                .writable = false,
            });
    }
    if (matched_ports != ports.size()) {
        const auto unmatched = std::find_if(
            ports.begin(), ports.end(),
            [&](const ShaderResourcePortDefinition &port) {
                for (std::size_t input = 0;
                     input < pass.input_targets.size();
                     ++input) {
                    auto resource =
                        dependencies
                            .render_target_metadata
                            .get(pass.input_targets[input])
                            .name;
                    if (pass.input_target_history[input]) {
                        resource += "@history";
                    }
                    if (resource == port.resource) {
                        return false;
                    }
                }
                if (std::find(
                        pass.input_buffers.begin(),
                        pass.input_buffers.end(),
                        port.resource) !=
                    pass.input_buffers.end()) {
                    return false;
                }
                return true;
            });
        throw std::runtime_error(
            "Shader resource port '" + unmatched->name +
            "' (resource '" + unmatched->resource +
            "') is not a fullscreen input");
    }
    validateFullscreenSamplingCapabilities(
        pass, dependencies,
        result.sampling, local_reads);
    return result;
}

std::vector<std::pair<std::string, std::string>>
makeRasterResourceVirtualIncludes(
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface,
    ShaderStage stage) {
    if (resource_interface.empty()) return {};
    auto result =
        makeShaderResourcePortVirtualIncludes(
            resource_interface);
    const auto stage_define =
        stage == ShaderStage::vertex
            ? "#define PELICAN_SURFACE_STAGE_VERTEX 1\n"
            : "#define PELICAN_SURFACE_STAGE_FRAGMENT 1\n";
    for (auto &[name, source] : result) {
        if (name == shaderResourcePortIncludeName) {
            source.insert(0, stage_define);
        }
    }
    return result;
}

FullscreenShaderModules registerFullscreenShaders(
    const FullscreenPassInfo &fullscreen_info,
    FullscreenRuntimeDependencies dependencies,
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface) {
    auto virtual_includes =
        makeRasterResourceVirtualIncludes(
            resource_interface,
            ShaderStage::fragment);
    return FullscreenShaderModules{
        registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                fullscreen_info.vert_shader,
                                dependencies.warn_backend_specific_shader_refs,
                                dependencies.shader_defines),
        registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                fullscreen_info.frag_shader,
                                dependencies.warn_backend_specific_shader_refs,
                                dependencies.shader_defines,
                                std::move(virtual_includes)),
    };
}

GenericRasterShaderModules registerGenericRasterShaders(
    const GenericRasterPassInfo &raster_info,
    FullscreenRuntimeDependencies dependencies,
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface) {
    auto vertex_includes =
        makeRasterResourceVirtualIncludes(
            resource_interface,
            ShaderStage::vertex);
    auto fragment_includes =
        makeRasterResourceVirtualIncludes(
            resource_interface,
            ShaderStage::fragment);
    GenericRasterShaderModules result{
        .vert_shader =
            registerShaderReference(
                dependencies.shader_library,
                dependencies.path_resolver,
                raster_info.vert_shader,
                dependencies
                    .warn_backend_specific_shader_refs,
                dependencies.shader_defines,
                std::move(vertex_includes)),
    };
    if (raster_info.frag_shader) {
        result.frag_shader =
            registerShaderReference(
                dependencies.shader_library,
                dependencies.path_resolver,
                *raster_info.frag_shader,
                dependencies
                    .warn_backend_specific_shader_refs,
                dependencies.shader_defines,
                std::move(fragment_includes));
    }
    return result;
}

PassId registerFullscreenPipeline(
    const PassDefinition &pass_def,
    FullscreenRuntimeDependencies dependencies,
    GraphicsPipelineViewContract view,
    const CompiledPassRenderingContract
        &rendering,
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface) {
    const auto color_formats =
        resolvePhysicalColorFormats(
            pass_def, rendering,
            dependencies.render_target,
            dependencies
                .render_target_metadata);
    const auto depth_format =
        resolvePhysicalDepthFormat(
            rendering,
            dependencies
                .render_target_metadata);
    const auto color_format =
        color_formats.front();
    if (pass_def.name == "output_transform" &&
        (color_format == vk::Format::eR8G8B8A8Unorm || color_format == vk::Format::eB8G8R8A8Unorm)) {
        dependencies.shader_defines.push_back("PELICAN_OUTPUT_UNORM_FALLBACK");
    }
    appendViewShaderDefines(
        dependencies.shader_defines, pass_def, view);
    appendLocalReadShaderDefines(
        dependencies.shader_defines, pass_def,
        rendering);
    const auto shaders = registerFullscreenShaders(
        pass_def.fullscreenInfo(), dependencies,
        resource_interface);
    const auto pipeline_id = dependencies.fullscreen_pass_container.registerFullscreenPass(
        color_formats, depth_format,
        shaders.vert_shader, shaders.frag_shader,
        dependencies.shader_defines,
        pass_def.rasterization_samples,
        view,
        graphicsLocalReadContract(rendering),
        std::vector<ShaderResourceInterfaceBinding>{
            resource_interface.begin(),
            resource_interface.end()});
    return fullscreenPipelineValueToPassId(pipeline_id.value);
}

void validateGenericRasterColorState(
    const PassDefinition &pass) {
    const auto &states =
        pass.genericRasterInfo()
            .contract.state.color_attachments;
    if (!pass.physical_color_numeric_classes.empty() &&
        pass.physical_color_numeric_classes.size() !=
            states.size()) {
        throw std::runtime_error(
            "Raster pass physical color numeric-class count "
            "does not match logical outputs: " +
            pass.name);
    }
    for (std::size_t index = 0;
         index < states.size(); ++index) {
        if (!states[index].blend.enabled ||
            pass.physical_color_numeric_classes.empty()) {
            continue;
        }
        if (pass.physical_color_numeric_classes[index] !=
            MaterialOutputNumericClass::floating) {
            throw std::runtime_error(
                "Raster pass cannot enable blending for integer "
                "color output " +
                std::to_string(index) + ": " +
                pass.name);
        }
    }
}

PassId registerGenericRasterPipeline(
    const PassDefinition &pass_def,
    FullscreenRuntimeDependencies dependencies,
    GraphicsPipelineViewContract view,
    const CompiledPassRenderingContract &rendering,
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface) {
    validateGenericRasterColorState(pass_def);
    auto color_formats =
        resolvePhysicalColorFormats(
            pass_def, rendering,
            dependencies.render_target,
            dependencies.render_target_metadata);
    const auto depth_format =
        resolvePhysicalDepthFormat(
            rendering,
            dependencies.render_target_metadata);
    appendViewShaderDefines(
        dependencies.shader_defines,
        pass_def, view);
    appendLocalReadShaderDefines(
        dependencies.shader_defines,
        pass_def, rendering);
    const auto shaders =
        registerGenericRasterShaders(
            pass_def.genericRasterInfo(),
            dependencies, resource_interface);

    GraphicsPipelineDesc desc;
    desc.vert = shaders.vert_shader;
    desc.frag = shaders.frag_shader;
    desc.color_formats =
        std::move(color_formats);
    desc.depth_format = depth_format;
    desc.shader_defines =
        std::move(dependencies.shader_defines);
    desc.rasterization_samples =
        pass_def.rasterization_samples;
    desc.view = view;
    desc.local_read =
        graphicsLocalReadContract(rendering);
    desc.resource_interface.assign(
        resource_interface.begin(),
        resource_interface.end());
    if (!rendering.local_read_scope) {
        for (std::size_t physical = 0;
             physical <
             rendering.color_attachment_locations
                 .size();
             ++physical) {
            const auto logical =
                rendering
                    .color_attachment_locations[
                        physical];
            if (logical !=
                    unusedPhysicalAttachmentMapping &&
                logical !=
                    static_cast<std::uint32_t>(
                        physical)) {
                throw std::runtime_error(
                    "Raster pass requires non-identity attachment "
                    "location remapping outside a local-read scope: " +
                    pass_def.name);
            }
        }
    }
    applyVulkanRasterPassContract(
        pass_def.genericRasterInfo().contract,
        desc,
        rendering.color_attachment_locations);
    const auto pipeline_id =
        dependencies.fullscreen_pass_container
            .registerRasterPass(std::move(desc));
    return fullscreenPipelineValueToPassId(
        pipeline_id.value);
}

PassId compileFullscreenPass(
    const PassDefinition &pass_def,
    FullscreenRuntimeDependencies dependencies,
    GraphicsPipelineViewContract view,
    std::uint32_t logical_view_count,
    const CompiledPassRenderingContract
        &rendering) {
    const auto resource_interface =
        compileFullscreenResourceInterface(
            pass_def, dependencies, view,
            logical_view_count,
            rendering);
    const auto pass_id =
        registerFullscreenPipeline(
            pass_def, dependencies, view,
            rendering,
            resource_interface.bindings);

    if (!pass_def.input_targets.empty() || !pass_def.input_buffers.empty()) {
        dependencies.fullscreen_pass_container.setInputResources(
            pass_id, pass_def.input_targets, pass_def.input_target_history, pass_def.input_buffers,
            dependencies.render_target_views,
            dependencies.frame_graph_resources,
            resource_interface.sampling,
            pass_def.input_target_views,
            view,
            localReadInputMask(
                pass_def, rendering),
            resource_interface.subresources,
            logical_view_count,
            resource_interface.view_dimensions);
    }

    return pass_id;
}

PassId compileGenericRasterPass(
    const PassDefinition &pass_def,
    FullscreenRuntimeDependencies dependencies,
    GraphicsPipelineViewContract view,
    std::uint32_t logical_view_count,
    const CompiledPassRenderingContract &rendering) {
    const auto resource_interface =
        compileFullscreenResourceInterface(
            pass_def, dependencies, view,
            logical_view_count, rendering);
    const auto pass_id =
        registerGenericRasterPipeline(
            pass_def, dependencies, view,
            rendering,
            resource_interface.bindings);
    if (!pass_def.input_targets.empty() ||
        !pass_def.input_buffers.empty()) {
        dependencies.fullscreen_pass_container
            .setInputResources(
                pass_id,
                pass_def.input_targets,
                pass_def.input_target_history,
                pass_def.input_buffers,
                dependencies.render_target_views,
                dependencies.frame_graph_resources,
                resource_interface.sampling,
                pass_def.input_target_views,
                view,
                localReadInputMask(
                    pass_def, rendering),
                resource_interface.subresources,
                logical_view_count,
                resource_interface.view_dimensions);
    }
    return pass_id;
}

PassId compileDebugDrawPass(const PassDefinition &pass_def, DebugDrawRuntimeDependencies dependencies) {
    const auto color_format =
        resolveFirstColorFormat(pass_def, dependencies.render_target, dependencies.render_target_metadata);
    const auto &debug_info = pass_def.debugDrawInfo();
    const auto vert_shader = registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                                     debug_info.vert_shader,
                                                     dependencies.warn_backend_specific_shader_refs,
                                                     dependencies.shader_defines);
    const auto frag_shader = registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                                     debug_info.frag_shader,
                                                     dependencies.warn_backend_specific_shader_refs,
                                                     dependencies.shader_defines);
    return dependencies.debug_draw.registerPass(color_format, vert_shader, frag_shader,
                                                dependencies.shader_defines,
                                                pass_def.rasterization_samples);
}

PassId compileDebugTextPass(const PassDefinition &pass_def, DebugTextRuntimeDependencies dependencies) {
    const auto color_format =
        resolveFirstColorFormat(pass_def, dependencies.render_target, dependencies.render_target_metadata);
    const auto &debug_info = pass_def.debugTextInfo();
    const auto vert_shader = registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                                     debug_info.vert_shader,
                                                     dependencies.warn_backend_specific_shader_refs,
                                                     dependencies.shader_defines);
    const auto frag_shader = registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                                     debug_info.frag_shader,
                                                     dependencies.warn_backend_specific_shader_refs,
                                                     dependencies.shader_defines);
    return dependencies.debug_text.registerPass(color_format, vert_shader, frag_shader,
                                                dependencies.shader_defines,
                                                pass_def.rasterization_samples);
}

PassId compileGizmoPass(const PassDefinition &pass_def,
                        GizmoRuntimeDependencies dependencies) {
    const auto color_format = resolveFirstColorFormat(
        pass_def, dependencies.render_target,
        dependencies.render_target_metadata);
    const auto &info = pass_def.gizmoInfo();
    const auto vert_shader = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver,
        info.vert_shader, dependencies.warn_backend_specific_shader_refs,
        dependencies.shader_defines);
    const auto frag_shader = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver,
        info.frag_shader, dependencies.warn_backend_specific_shader_refs,
        dependencies.shader_defines);
    return dependencies.gizmo.registerPass(
        color_format, vert_shader, frag_shader, dependencies.shader_defines,
        pass_def.rasterization_samples);
}

PassId compileShadowDepthPass(const PassDefinition &pass_def, ShadowDepthRuntimeDependencies dependencies) {
    const auto depth_rt = dependencies.render_target_metadata.get(pass_def.output_depth);
    const auto vert_shader = registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                                     pass_def.shadowDepthInfo().vert_shader,
                                                     dependencies.warn_backend_specific_shader_refs,
                                                     dependencies.shader_defines);
    return dependencies.shadow_depth_pass_container.registerShadowDepthPass(depth_rt.format, vert_shader,
                                                                           dependencies.shader_defines,
                                                                           pass_def.rasterization_samples);
}

PassId compileVelocityPass(const PassDefinition &pass_def,
                           GeometryPassRuntimeDependencies dependencies) {
    const auto color = dependencies.render_target_metadata.get(pass_def.output_color.front());
    const auto depth = dependencies.render_target_metadata.get(pass_def.output_depth);
    if (color.format != vk::Format::eR16G16Sfloat) {
        throw std::runtime_error("Velocity pass color output must be R16G16_SFLOAT: " + color.name);
    }
    if (depth.format != vk::Format::eD32Sfloat) {
        throw std::runtime_error("Velocity pass depth output must be D32_SFLOAT: " + depth.name);
    }
    const auto &info = pass_def.velocityInfo();
    const auto regular_vert = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver, info.vert_shader,
        dependencies.warn_backend_specific_shader_refs, dependencies.shader_defines);
    const auto skinned_vert = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver, info.skinned_vert_shader,
        dependencies.warn_backend_specific_shader_refs, dependencies.shader_defines);
    const auto frag = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver, info.frag_shader,
        dependencies.warn_backend_specific_shader_refs, dependencies.shader_defines);
    return dependencies.velocity_pass_container.registerVelocityPass(
        color.format, depth.format, regular_vert, skinned_vert, frag,
        dependencies.shader_defines, pass_def.rasterization_samples);
}

PassId compilePickingPass(const PassDefinition &pass_def,
                          GeometryPassRuntimeDependencies dependencies) {
    const auto color =
        dependencies.render_target_metadata.get(pass_def.output_color.front());
    const auto depth =
        dependencies.render_target_metadata.get(pass_def.output_depth);
    if (color.format != vk::Format::eR32Uint) {
        throw std::runtime_error(
            "Picking pass color output must be R32_UINT: " + color.name);
    }
    if (depth.format != vk::Format::eD32Sfloat) {
        throw std::runtime_error(
            "Picking pass depth output must be D32_SFLOAT: " + depth.name);
    }
    const auto &info = pass_def.pickingInfo();
    const auto regular_vert = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver,
        info.vert_shader, dependencies.warn_backend_specific_shader_refs,
        dependencies.shader_defines);
    const auto skinned_vert = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver,
        info.skinned_vert_shader,
        dependencies.warn_backend_specific_shader_refs,
        dependencies.shader_defines);
    const auto frag = registerShaderReference(
        dependencies.shader_library, dependencies.path_resolver,
        info.frag_shader, dependencies.warn_backend_specific_shader_refs,
        dependencies.shader_defines);
    // Picking and velocity are both geometry-only passes with the same
    // regular/skinned pipeline shape. Keeping them in one registry also gives
    // picking the render-generation rollback/retirement contract.
    return dependencies.velocity_pass_container.registerVelocityPass(
        color.format, depth.format, regular_vert, skinned_vert, frag,
        dependencies.shader_defines, pass_def.rasterization_samples);
}

} // namespace

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition,
                                                  RenderingPassRuntimeDependencies dependencies) {
    ScopedLogTimer timer{"compile rendering pass runtime"};

    CompiledRenderingPass compiled_pass;
    compiled_pass.name = definition.name;
    compiled_pass.passes.reserve(definition.passes.size());

    for (size_t i = 0; i < definition.passes.size(); ++i) {
        const auto &source_pass =
            definition.passes[i];
        const auto view =
            passViewContract(
                dependencies.target_plan,
                source_pass);
        const auto logical_view_count =
            passLogicalViewCount(
                dependencies.target_plan,
                source_pass);
        validatePassAttachmentViews(
            source_pass,
            dependencies.render_target_metadata,
            logical_view_count);
        auto pass_def = applyPhysicalPassContract(
            source_pass,
            dependencies.target_plan,
            dependencies.render_target_metadata,
            dependencies.frame_graph_resources);
        validatePassInputViewContract(
            pass_def, view);
        const auto rendering =
            compilePassRenderingContract(
                definition, pass_def,
                dependencies.target_plan,
                dependencies.render_target_metadata);
        if (pass_def.isFullscreen()) {
            const auto fullscreen_dependencies = requireFullscreenDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{
                    pass_def,
                    compileFullscreenPass(
                        pass_def, fullscreen_dependencies,
                        view, logical_view_count,
                        rendering),
                    view,
                    rendering});
        } else if (pass_def.isGenericRaster()) {
            const auto raster_dependencies =
                requireFullscreenDependencies(
                    pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{
                    pass_def,
                    compileGenericRasterPass(
                        pass_def,
                        raster_dependencies,
                        view,
                        logical_view_count,
                        rendering),
                    view,
                    rendering});
        } else if (pass_def.isDebugDraw()) {
            const auto debug_draw_dependencies = requireDebugDrawDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileDebugDrawPass(pass_def, debug_draw_dependencies), view, rendering});
        } else if (pass_def.isGizmo()) {
            const auto gizmo_dependencies =
                requireGizmoDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def,
                             compileGizmoPass(pass_def, gizmo_dependencies),
                             view, rendering});
        } else if (pass_def.isDebugText()) {
            const auto debug_text_dependencies = requireDebugTextDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileDebugTextPass(pass_def, debug_text_dependencies), view, rendering});
        } else if (pass_def.isShadowDepth()) {
            const auto shadow_depth_dependencies = requireShadowDepthDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileShadowDepthPass(pass_def, shadow_depth_dependencies), view, rendering});
        } else if (pass_def.isVelocity()) {
            const auto velocity_dependencies = requireGeometryPassDependencies(
                pass_def, dependencies, "Velocity");
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileVelocityPass(pass_def, velocity_dependencies), view, rendering});
        } else if (pass_def.isPicking()) {
            const auto picking_dependencies =
                requireGeometryPassDependencies(
                    pass_def, dependencies, "Picking");
            compiled_pass.passes.push_back(
                CompiledPass{pass_def,
                             compilePickingPass(pass_def, picking_dependencies),
                             view, rendering});
        } else {
            compiled_pass.passes.push_back(CompiledPass{pass_def, passIndexToPassId(i), view, rendering});
        }
    }

    return compiled_pass;
}

std::vector<CompiledRenderingPass> compileRenderingPassesRuntime(
    const std::vector<RenderingPassDefinition> &definitions,
    RenderingPassRuntimeDependencies dependencies) {
    std::vector<CompiledRenderingPass> compiled_passes;
    compiled_passes.reserve(definitions.size());
    for (const auto &definition : definitions) {
        compiled_passes.push_back(compileRenderingPassRuntime(definition, dependencies));
    }
    return compiled_passes;
}

} // namespace Pelican
