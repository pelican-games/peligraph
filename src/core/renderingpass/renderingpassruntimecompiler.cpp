#include "renderingpassruntimecompiler.hpp"
#include "computetask.hpp"
#include "rendertargetimageviewresolver.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../profiler.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../shader/shaderresourceinterface.hpp"
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

struct ShadowDepthRuntimeDependencies {
    const RenderTargetMetadataResolver &render_target_metadata;
    ShaderLibrary &shader_library;
    ShadowDepthPassContainer &shadow_depth_pass_container;
    const PathResolver &path_resolver;
    std::vector<std::string> shader_defines;
    bool warn_backend_specific_shader_refs = false;
};

struct VelocityRuntimeDependencies {
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
    if (!pass.isFullscreen()) {
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
    VulkanPhysicalAttachmentAspect aspect) {
    const VulkanPhysicalAttachmentPlan *result =
        nullptr;
    for (const auto &attachment :
         plan.attachments) {
        if (attachment.node != pass ||
            attachment.logical_resource !=
                resource) {
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
    const RenderTargetMetadataResolver *metadata) {
    auto result = source;
    result.physical_color_attachment_operations
        .clear();
    result.physical_depth_attachment_operations
        .reset();
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
                VulkanPhysicalAttachmentAspect::
                    depth);
    }
    return result;
}

bool containsTarget(
    std::span<const GlobalRenderTargetId> targets,
    GlobalRenderTargetId target) {
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
        VulkanPhysicalAttachmentAspect::depth);
}

std::array<float, 4> clearColorFloats(
    const vk::ClearColorValue &value) {
    return {
        value.float32[0], value.float32[1],
        value.float32[2], value.float32[3]};
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
        if (!result.local_read_scope) {
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
            clearColorFloats(
                first_writer->clear_color));
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
        const auto color = std::find(
            result.color_attachments.begin(),
            result.color_attachments.end(),
            target);
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
        if (target ==
            result.depth_attachment) {
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

void appendMultiviewShaderDefines(
    std::vector<std::string> &defines,
    const PassDefinition &pass,
    const GraphicsPipelineViewContract &view) {
    if (view.execution !=
        GraphicsPipelineViewExecution::multiview) {
        return;
    }
    defines.push_back("PELICAN_MULTIVIEW=1");
    defines.push_back(
        "PELICAN_VIEW_COUNT=" +
        std::to_string(view.view_count));
    for (std::size_t binding = 0;
         binding < pass.input_target_views.size();
         ++binding) {
        if (pass.input_target_views[binding] ==
            PassInputViewDimension::layered_2d_array) {
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

VelocityRuntimeDependencies requireVelocityDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies) {
    if (dependencies.render_target_metadata == nullptr || dependencies.shader_library == nullptr ||
        dependencies.velocity_pass_container == nullptr || dependencies.path_resolver == nullptr) {
        throw std::runtime_error("Velocity pass runtime compile dependencies are unavailable: " +
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

CompiledFullscreenResourceInterface
compileFullscreenResourceInterface(
    const PassDefinition &pass,
    const FullscreenRuntimeDependencies &dependencies,
    GraphicsPipelineViewContract view,
    const CompiledPassRenderingContract &rendering) {
    CompiledFullscreenResourceInterface result;
    const auto &ports =
        pass.fullscreenInfo().resource_ports;
    if (ports.empty()) {
        result.sampling =
            pass.fullscreenInfo().input_sampling;
        return result;
    }

    result.bindings.reserve(ports.size());
    result.sampling.resize(
        pass.input_targets.size());
    const auto local_reads =
        localReadInputMask(pass, rendering);
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
        if (local_reads[input]) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') cannot bind a tile-local input attachment as a "
                "sampled image");
        }
        if (!(metadata.usage &
              vk::ImageUsageFlagBits::eSampled)) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') requires sampled render-target usage");
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
                *port, physical, consumer);
        if (dimension ==
                ReflectedImageViewDimension::
                    two_d_array &&
            metadata.array_layers < view.view_count) {
            throw std::runtime_error(
                "Shader resource port '" + port->name +
                "' (resource '" + port->resource +
                "') has too few physical array layers");
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
                return true;
            });
        throw std::runtime_error(
            "Shader resource port '" + unmatched->name +
            "' (resource '" + unmatched->resource +
            "') is not an image input; typed frame-graph buffers are "
            "not available yet");
    }
    return result;
}

FullscreenShaderModules registerFullscreenShaders(
    const FullscreenPassInfo &fullscreen_info,
    FullscreenRuntimeDependencies dependencies,
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface) {
    std::vector<std::pair<std::string, std::string>>
        virtual_includes;
    if (!resource_interface.empty()) {
        virtual_includes =
            makeShaderResourcePortVirtualIncludes(
                resource_interface);
    }
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
    appendMultiviewShaderDefines(
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

PassId compileFullscreenPass(
    const PassDefinition &pass_def,
    FullscreenRuntimeDependencies dependencies,
    GraphicsPipelineViewContract view,
    const CompiledPassRenderingContract
        &rendering) {
    const auto resource_interface =
        compileFullscreenResourceInterface(
            pass_def, dependencies, view,
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
                pass_def, rendering));
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
                           VelocityRuntimeDependencies dependencies) {
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

} // namespace

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition,
                                                  RenderingPassRuntimeDependencies dependencies) {
    ScopedLogTimer timer{"compile rendering pass runtime"};

    CompiledRenderingPass compiled_pass;
    compiled_pass.name = definition.name;
    compiled_pass.passes.reserve(definition.passes.size());

    for (size_t i = 0; i < definition.passes.size(); ++i) {
        auto pass_def = applyPhysicalPassContract(
            definition.passes[i],
            dependencies.target_plan,
            dependencies.render_target_metadata);
        const auto view =
            passViewContract(
                dependencies.target_plan, pass_def);
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
                        view, rendering),
                    view,
                    rendering});
        } else if (pass_def.isDebugDraw()) {
            const auto debug_draw_dependencies = requireDebugDrawDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileDebugDrawPass(pass_def, debug_draw_dependencies), view, rendering});
        } else if (pass_def.isDebugText()) {
            const auto debug_text_dependencies = requireDebugTextDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileDebugTextPass(pass_def, debug_text_dependencies), view, rendering});
        } else if (pass_def.isShadowDepth()) {
            const auto shadow_depth_dependencies = requireShadowDepthDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileShadowDepthPass(pass_def, shadow_depth_dependencies), view, rendering});
        } else if (pass_def.isVelocity()) {
            const auto velocity_dependencies = requireVelocityDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileVelocityPass(pass_def, velocity_dependencies), view, rendering});
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
