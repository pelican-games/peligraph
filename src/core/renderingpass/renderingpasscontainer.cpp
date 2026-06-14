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
    registered_pass_ids.push_back(id);
    return id;
}

RenderingPassId RenderingPassContainer::getRenderingPassIdByName(const std::string &name) const {
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return invalidRenderingPassId();
}

const CompiledRenderingPass &RenderingPassContainer::getCompiledRenderingPass(RenderingPassId rendering_pass_id) const {
    return rendering_passes.get(rendering_pass_id);
}

const std::vector<RenderingPassId> &RenderingPassContainer::getRegisteredPassIds() const {
    return registered_pass_ids;
}

} // namespace Pelican
