#pragma once

#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "renderingpass.hpp"
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Pelican {

DECLARE_MODULE(RenderingPassContainer) {
    ResourceContainer<RenderingPassId, CompiledRenderingPass> rendering_passes;
    std::unordered_map<std::string, RenderingPassId> name_to_id;
    std::vector<RenderingPassId> registered_pass_ids;
    std::vector<std::string> enabled_feature_names;

  public:
    RenderingPassContainer();
    ~RenderingPassContainer();

    RenderingPassId registerCompiledRenderingPass(CompiledRenderingPass pass);
    RenderingPassId getRenderingPassIdByName(const std::string &name) const;
    const CompiledRenderingPass &getCompiledRenderingPass(RenderingPassId rendering_pass_id) const;
    const std::vector<RenderingPassId> &getRegisteredPassIds() const;
    void setEnabledFeatures(std::vector<std::string> feature_names);
    bool isFeatureEnabled(std::string_view feature_name) const;
    const std::vector<std::string> &getEnabledFeatures() const;
    bool hasMaterialPasses() const;
    bool supportsMaterialPass(MaterialRouteClass route,
                              MaterialShaderContract shader_contract,
                              const std::optional<std::string> &exact_pass = std::nullopt) const;
};

} // namespace Pelican
