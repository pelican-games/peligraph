#include "renderingpassruntimecompiler.hpp"
#include "rendertargetcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../loader/fileio.hpp"
#include "../profiler.hpp"
#include "../shader/shadercontainer.hpp"
#include "../vkcore/rendertarget.hpp"
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

struct FullscreenRuntimeDependencies {
    RenderTarget &render_target;
    RenderTargetContainer &render_target_container;
    ShaderContainer &shader_container;
    FullscreenPassContainer &fullscreen_pass_container;
};

vk::Format resolveFirstColorFormat(const PassDefinition &pass_def, RenderTarget &rt_module,
                                   RenderTargetContainer &rt_container) {
    if (pass_def.output_color.empty()) {
        throw std::runtime_error("Fullscreen pass has no color output: " + pass_def.name);
    }

    const auto &first_color = pass_def.output_color.front();
    if (isConcreteRenderTarget(first_color)) {
        return rt_container.getMetadata(first_color).format;
    }
    return rt_module.getSwapchainFormat();
}

PassId passIndexToPassId(size_t pass_index) {
    if (pass_index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Rendering pass index is too large");
    }
    return PassId{static_cast<int>(pass_index)};
}

FullscreenRuntimeDependencies requireFullscreenDependencies(
    const PassDefinition &pass_def,
    const RenderingPassRuntimeDependencies &dependencies) {
    if (dependencies.render_target == nullptr || dependencies.render_target_container == nullptr ||
        dependencies.shader_container == nullptr || dependencies.fullscreen_pass_container == nullptr) {
        throw std::runtime_error(
            "Fullscreen pass runtime compile requires render target, render target container, shader container, "
            "and fullscreen pass container dependencies: " +
            pass_def.name);
    }
    return FullscreenRuntimeDependencies{
        *dependencies.render_target,
        *dependencies.render_target_container,
        *dependencies.shader_container,
        *dependencies.fullscreen_pass_container,
    };
}

PassId compileFullscreenPass(const PassDefinition &pass_def, FullscreenRuntimeDependencies dependencies) {
    const auto color_format =
        resolveFirstColorFormat(pass_def, dependencies.render_target, dependencies.render_target_container);
    const auto &fullscreen_info = pass_def.fullscreenInfo();
    const auto vert_shader_data = readBinaryFile(fullscreen_info.vert_shader_path);
    const auto frag_shader_data = readBinaryFile(fullscreen_info.frag_shader_path);
    const auto vert_shader_id =
        dependencies.shader_container.registerShader(vert_shader_data.size(), vert_shader_data.data());
    const auto frag_shader_id =
        dependencies.shader_container.registerShader(frag_shader_data.size(), frag_shader_data.data());
    const auto vert_shader = dependencies.shader_container.getShader(vert_shader_id);
    const auto frag_shader = dependencies.shader_container.getShader(frag_shader_id);

    const auto pipeline_id =
        dependencies.fullscreen_pass_container.registerFullscreenPass(color_format, vert_shader, frag_shader);
    if (pipeline_id.value > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Fullscreen pipeline id is too large");
    }
    const PassId pass_id{static_cast<int>(pipeline_id.value)};

    if (!pass_def.input_targets.empty()) {
        dependencies.fullscreen_pass_container.setInputTextures(
            pass_id, pass_def.input_targets, dependencies.render_target_container);
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
