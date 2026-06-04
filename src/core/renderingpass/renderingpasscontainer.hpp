#pragma once

#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "renderingpass.hpp"
#include <span>
#include <string>
#include <unordered_map>

namespace Pelican {

DECLARE_MODULE(RenderingPassContainer) {
    ResourceContainer<RenderingPassId, CompiledRenderingPass> rendering_passes;
    std::unordered_map<std::string, RenderingPassId> name_to_id;

  public:
    RenderingPassContainer();
    ~RenderingPassContainer();

    RenderingPassId registerCompiledRenderingPass(CompiledRenderingPass pass);
    RenderingPassId getRenderingPassIdByName(const std::string &name) const;
    std::span<const CompiledPass> getCompiledPasses(RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican
