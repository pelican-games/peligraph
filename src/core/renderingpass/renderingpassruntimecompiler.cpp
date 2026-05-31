#include "renderingpassruntimecompiler.hpp"
#include "rendertargetcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../shader/shadercontainer.hpp"
#include "../vkcore/rendertarget.hpp"
#include <limits>
#include <stdexcept>

namespace Pelican {

namespace {

vk::Format resolveFirstColorFormat(const PassDefinition &pass_def, RenderTarget &rt_module,
                                   RenderTargetContainer &rt_container) {
    if (pass_def.output_color.empty()) {
        throw std::runtime_error("Fullscreen pass has no color output: " + pass_def.name);
    }

    const auto &first_color = pass_def.output_color.front();
    if (isConcreteRenderTarget(first_color)) {
        return rt_container.get(first_color).image.format;
    }
    return rt_module.getSwapchainFormat();
}

PassId passIndexToPassId(size_t pass_index) {
    if (pass_index > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Rendering pass index is too large");
    }
    return PassId{static_cast<int>(pass_index)};
}

PassId compileFullscreenPass(const PassDefinition &pass_def) {
    auto &rt_module = GET_MODULE(RenderTarget);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &shader_container = GET_MODULE(ShaderContainer);
    auto &fs_container = GET_MODULE(FullscreenPassContainer);

    const auto color_format = resolveFirstColorFormat(pass_def, rt_module, rt_container);
    const auto &fullscreen_info = pass_def.fullscreenInfo();
    const auto vert_shader = shader_container.getShader(fullscreen_info.vert_shader);
    const auto frag_shader = shader_container.getShader(fullscreen_info.frag_shader);

    const auto pipeline_id = fs_container.registerFullscreenPass(color_format, vert_shader, frag_shader);
    if (pipeline_id.value > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Fullscreen pipeline id is too large");
    }
    const PassId pass_id{static_cast<int>(pipeline_id.value)};

    if (!pass_def.input_targets.empty()) {
        fs_container.setInputTextures(pass_id, pass_def.input_targets);
    }

    return pass_id;
}

} // namespace

std::vector<PassId> compileRenderingPassRuntime(const RenderingPassDefinition &definition) {
    std::vector<PassId> pass_ids;
    pass_ids.reserve(definition.passes.size());

    for (size_t i = 0; i < definition.passes.size(); ++i) {
        const auto &pass_def = definition.passes[i];
        if (pass_def.isFullscreen()) {
            pass_ids.push_back(compileFullscreenPass(pass_def));
        } else {
            pass_ids.push_back(passIndexToPassId(i));
        }
    }

    return pass_ids;
}

} // namespace Pelican
