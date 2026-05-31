#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../shader/shadercontainer.hpp"
#include "../vkcore/rendertarget.hpp"
#include <stdexcept>
#include <utility>

namespace Pelican {

RenderingPassContainer::RenderingPassContainer() {}

RenderingPassContainer::~RenderingPassContainer() {}

RenderingPassId RenderingPassContainer::registerRenderingPass(const RenderingPassDefinition &definition) {
    if (auto it = name_to_id.find(definition.name); it != name_to_id.end()) {
        return it->second;
    }

    InternalRenderingPass internal_pass;
    internal_pass.definition = definition;

    for (int i = 0; i < definition.passes.size(); ++i) {
        const auto &pass_def = definition.passes[i];

        if (pass_def.isFullscreen()) {
            auto &rt_module = GET_MODULE(RenderTarget);
            auto &rt_container = GET_MODULE(RenderTargetContainer);

            if (pass_def.output_color.empty()) {
                throw std::runtime_error("Fullscreen pass has no color output: " + pass_def.name);
            }

            vk::Format color_fmt{};
            const auto &first_color = pass_def.output_color[0];
            if (isConcreteRenderTarget(first_color)) {
                color_fmt = rt_container.get(first_color).image.format;
            } else {
                color_fmt = rt_module.getSwapchainFormat();
            }

            auto &shader_container = GET_MODULE(ShaderContainer);
            const auto &fullscreenInfo = pass_def.fullscreenInfo();
            auto vert_shader = shader_container.getShader(fullscreenInfo.vert_shader);
            auto frag_shader = shader_container.getShader(fullscreenInfo.frag_shader);

            auto &fs_container = GET_MODULE(FullscreenPassContainer);
            auto pipeline_id = fs_container.registerFullscreenPass(color_fmt, vert_shader, frag_shader);

            PassId pass_id{static_cast<int>(pipeline_id.value)};
            if (!pass_def.input_targets.empty()) {
                fs_container.setInputTextures(pass_id, pass_def.input_targets);
            }

            internal_pass.pass_ids.push_back(pass_id);
        } else {
            internal_pass.pass_ids.push_back(PassId{i});
        }
    }

    auto id = rendering_passes.reg(std::move(internal_pass));
    name_to_id.emplace(definition.name, id);
    return id;
}

RenderingPassId RenderingPassContainer::getRenderingPassIdByName(const std::string &name) const {
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return invalidRenderingPassId();
}

std::span<const PassId> RenderingPassContainer::getPasses(RenderingPassId rendering_pass_id) const {
    const auto &pass = rendering_passes.get(rendering_pass_id);
    return pass.pass_ids;
}

const PassDefinition &RenderingPassContainer::getPassDefinition(RenderingPassId rendering_pass_id,
                                                                size_t pass_index) const {
    const auto &pass = rendering_passes.get(rendering_pass_id);
    return pass.definition.passes[pass_index];
}

} // namespace Pelican
