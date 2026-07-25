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
#include "../vkcore/rendertarget.hpp"
#include <algorithm>
#include <limits>
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

PassDefinition applyPhysicalPassContract(
    const PassDefinition &source,
    const VulkanTargetPlan *plan,
    const RenderTargetMetadataResolver *metadata) {
    auto result = source;
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
                                       const std::vector<std::string> &shader_defines) {
    warnBackendSpecificShaderReference(reference, warn_backend_specific);
    return shader_library.loadFromReference(reference, path_resolver, warn_backend_specific, shader_defines);
}

FullscreenShaderModules registerFullscreenShaders(const FullscreenPassInfo &fullscreen_info,
                                                  FullscreenRuntimeDependencies dependencies) {
    return FullscreenShaderModules{
        registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                fullscreen_info.vert_shader,
                                dependencies.warn_backend_specific_shader_refs,
                                dependencies.shader_defines),
        registerShaderReference(dependencies.shader_library, dependencies.path_resolver,
                                fullscreen_info.frag_shader,
                                dependencies.warn_backend_specific_shader_refs,
                                dependencies.shader_defines),
    };
}

PassId registerFullscreenPipeline(
    const PassDefinition &pass_def,
    FullscreenRuntimeDependencies dependencies,
    GraphicsPipelineViewContract view) {
    const auto color_format =
        resolveFirstColorFormat(pass_def, dependencies.render_target, dependencies.render_target_metadata);
    if (pass_def.name == "output_transform" &&
        (color_format == vk::Format::eR8G8B8A8Unorm || color_format == vk::Format::eB8G8R8A8Unorm)) {
        dependencies.shader_defines.push_back("PELICAN_OUTPUT_UNORM_FALLBACK");
    }
    appendMultiviewShaderDefines(
        dependencies.shader_defines, pass_def, view);
    const auto shaders = registerFullscreenShaders(pass_def.fullscreenInfo(), dependencies);
    const auto pipeline_id = dependencies.fullscreen_pass_container.registerFullscreenPass(
        color_format, shaders.vert_shader, shaders.frag_shader,
        dependencies.shader_defines, pass_def.rasterization_samples,
        view);
    return fullscreenPipelineValueToPassId(pipeline_id.value);
}

PassId compileFullscreenPass(
    const PassDefinition &pass_def,
    FullscreenRuntimeDependencies dependencies,
    GraphicsPipelineViewContract view) {
    const auto pass_id =
        registerFullscreenPipeline(
            pass_def, dependencies, view);

    if (!pass_def.input_targets.empty() || !pass_def.input_buffers.empty()) {
        dependencies.fullscreen_pass_container.setInputResources(
            pass_id, pass_def.input_targets, pass_def.input_target_history, pass_def.input_buffers,
            dependencies.render_target_views,
            dependencies.frame_graph_resources,
            pass_def.fullscreenInfo().input_sampling,
            pass_def.input_target_views,
            view);
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
        if (pass_def.isFullscreen()) {
            const auto fullscreen_dependencies = requireFullscreenDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{
                    pass_def,
                    compileFullscreenPass(
                        pass_def, fullscreen_dependencies,
                        view),
                    view});
        } else if (pass_def.isDebugDraw()) {
            const auto debug_draw_dependencies = requireDebugDrawDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileDebugDrawPass(pass_def, debug_draw_dependencies), view});
        } else if (pass_def.isDebugText()) {
            const auto debug_text_dependencies = requireDebugTextDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileDebugTextPass(pass_def, debug_text_dependencies), view});
        } else if (pass_def.isShadowDepth()) {
            const auto shadow_depth_dependencies = requireShadowDepthDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileShadowDepthPass(pass_def, shadow_depth_dependencies), view});
        } else if (pass_def.isVelocity()) {
            const auto velocity_dependencies = requireVelocityDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileVelocityPass(pass_def, velocity_dependencies), view});
        } else {
            compiled_pass.passes.push_back(CompiledPass{pass_def, passIndexToPassId(i), view});
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
