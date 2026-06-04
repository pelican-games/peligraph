#pragma once

#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "renderingpass.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Pelican {

DECLARE_MODULE(RenderingPassContainer) {
    ResourceContainer<RenderingPassId, CompiledRenderingPass> rendering_passes;
    std::unordered_map<std::string, RenderingPassId> name_to_id;

  public:
    RenderingPassContainer();
    ~RenderingPassContainer();

    RenderingPassId registerRenderingPass(const RenderingPassDefinition &definition);
    RenderingPassId getRenderingPassIdByName(const std::string &name) const;
    std::span<const CompiledPass> getPasses(RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican
