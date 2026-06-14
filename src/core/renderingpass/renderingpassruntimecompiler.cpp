#include "renderingpassruntimecompiler.hpp"
#include "rendertargetimageviewresolver.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../profiler.hpp"
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
};

struct FullscreenShaderModules {
    vk::ShaderModule vert_shader;
    vk::ShaderModule frag_shader;
};

vk::Format resolveFirstColorFormat(const PassDefinition &pass_def, RenderTarget &rt_module,
                                   const RenderTargetMetadataResolver &rt_metadata) {
    if (pass_def.output_color.empty()) {
        throw std::runtime_error("Fullscreen pass has no color output: " + pass_def.name);
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
        dependencies.fullscreen_pass_container == nullptr) {
        throw std::runtime_error(
            "Fullscreen pass runtime compile requires render target, render target metadata, render target views, "
            "shader library, and fullscreen pass container dependencies: " +
            pass_def.name);
    }
    return FullscreenRuntimeDependencies{
        *dependencies.render_target,
        *dependencies.render_target_metadata,
        *dependencies.render_target_views,
        *dependencies.shader_library,
        *dependencies.fullscreen_pass_container,
    };
}

vk::ShaderModule registerShaderFromFile(ShaderLibrary &shader_library, const std::string &shader_path) {
    const auto shader_id = shader_library.loadFromFile(shader_path);
    return shader_library.get(shader_id).module.get();
}

FullscreenShaderModules registerFullscreenShaders(const FullscreenPassInfo &fullscreen_info,
                                                  ShaderLibrary &shader_library) {
    return FullscreenShaderModules{
        registerShaderFromFile(shader_library, fullscreen_info.vert_shader_path),
        registerShaderFromFile(shader_library, fullscreen_info.frag_shader_path),
    };
}

PassId registerFullscreenPipeline(const PassDefinition &pass_def, FullscreenRuntimeDependencies dependencies) {
    const auto color_format =
        resolveFirstColorFormat(pass_def, dependencies.render_target, dependencies.render_target_metadata);
    const auto shaders = registerFullscreenShaders(pass_def.fullscreenInfo(), dependencies.shader_library);
    const auto pipeline_id = dependencies.fullscreen_pass_container.registerFullscreenPass(
        color_format, shaders.vert_shader, shaders.frag_shader);
    return fullscreenPipelineValueToPassId(pipeline_id.value);
}

PassId compileFullscreenPass(const PassDefinition &pass_def, FullscreenRuntimeDependencies dependencies) {
    const auto pass_id = registerFullscreenPipeline(pass_def, dependencies);

    if (!pass_def.input_targets.empty()) {
        dependencies.fullscreen_pass_container.setInputTextures(
            pass_id, pass_def.input_targets, dependencies.render_target_views);
    }

    return pass_id;
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
