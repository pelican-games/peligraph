#include "renderingpassruntimecompiler.hpp"
#include "computetask.hpp"
#include "rendertargetimageviewresolver.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"
#include "../profiler.hpp"
#include "../renderer/debugdraw.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../vkcore/rendertarget.hpp"
#include <limits>
#include <stdexcept>

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

struct FullscreenShaderModules {
    ShaderBundleId vert_shader;
    ShaderBundleId frag_shader;
};

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

PassId registerFullscreenPipeline(const PassDefinition &pass_def, FullscreenRuntimeDependencies dependencies) {
    const auto color_format =
        resolveFirstColorFormat(pass_def, dependencies.render_target, dependencies.render_target_metadata);
    const auto shaders = registerFullscreenShaders(pass_def.fullscreenInfo(), dependencies);
    const auto pipeline_id = dependencies.fullscreen_pass_container.registerFullscreenPass(
        color_format, shaders.vert_shader, shaders.frag_shader, dependencies.shader_defines);
    return fullscreenPipelineValueToPassId(pipeline_id.value);
}

PassId compileFullscreenPass(const PassDefinition &pass_def, FullscreenRuntimeDependencies dependencies) {
    const auto pass_id = registerFullscreenPipeline(pass_def, dependencies);

    if (!pass_def.input_targets.empty() || !pass_def.input_buffers.empty()) {
        dependencies.fullscreen_pass_container.setInputResources(
            pass_id, pass_def.input_targets, pass_def.input_buffers, dependencies.render_target_views,
            dependencies.frame_graph_resources);
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
                                                dependencies.shader_defines);
}

} // namespace

CompiledRenderingPass compileRenderingPassRuntime(const RenderingPassDefinition &definition,
                                                  RenderingPassRuntimeDependencies dependencies) {
    ScopedLogTimer timer{"compile rendering pass runtime"};

    CompiledRenderingPass compiled_pass;
    compiled_pass.name = definition.name;
    compiled_pass.passes.reserve(definition.passes.size());

    for (size_t i = 0; i < definition.passes.size(); ++i) {
        const auto &pass_def = definition.passes[i];
        if (pass_def.isFullscreen()) {
            const auto fullscreen_dependencies = requireFullscreenDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileFullscreenPass(pass_def, fullscreen_dependencies)});
        } else if (pass_def.isDebugDraw()) {
            const auto debug_draw_dependencies = requireDebugDrawDependencies(pass_def, dependencies);
            compiled_pass.passes.push_back(
                CompiledPass{pass_def, compileDebugDrawPass(pass_def, debug_draw_dependencies)});
        } else {
            compiled_pass.passes.push_back(CompiledPass{pass_def, passIndexToPassId(i)});
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
