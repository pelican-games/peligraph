#include "renderingpasscontainer.hpp"
#include "renderingpassruntimecompiler.hpp"
#include <utility>

namespace Pelican {

RenderingPassContainer::RenderingPassContainer() {}

RenderingPassContainer::~RenderingPassContainer() {}

RenderingPassId RenderingPassContainer::registerRenderingPass(const RenderingPassDefinition &definition) {
    if (auto it = name_to_id.find(definition.name); it != name_to_id.end()) {
        return it->second;
    }

    auto compiled_pass = compileRenderingPassRuntime(definition);
    auto id = rendering_passes.reg(std::move(compiled_pass));
    name_to_id.emplace(definition.name, id);
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
