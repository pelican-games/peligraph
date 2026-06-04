#include "renderingpasscontainer.hpp"
#include <utility>

namespace Pelican {

RenderingPassContainer::RenderingPassContainer() {}

RenderingPassContainer::~RenderingPassContainer() {}

RenderingPassId RenderingPassContainer::registerCompiledRenderingPass(CompiledRenderingPass pass) {
    if (auto it = name_to_id.find(pass.name); it != name_to_id.end()) {
        return it->second;
    }

    const auto pass_name = pass.name;
    auto id = rendering_passes.reg(std::move(pass));
    name_to_id.emplace(pass_name, id);
    return id;
}

RenderingPassId RenderingPassContainer::getRenderingPassIdByName(const std::string &name) const {
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return invalidRenderingPassId();
}

std::span<const CompiledPass> RenderingPassContainer::getCompiledPasses(RenderingPassId rendering_pass_id) const {
    const auto &pass = rendering_passes.get(rendering_pass_id);
    return pass.passes;
}

} // namespace Pelican
