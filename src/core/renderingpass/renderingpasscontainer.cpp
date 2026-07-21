#include "renderingpasscontainer.hpp"
#include <algorithm>
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

void RenderingPassContainer::setEnabledFeatures(std::vector<std::string> feature_names) {
    enabled_feature_names = std::move(feature_names);
}

bool RenderingPassContainer::isFeatureEnabled(std::string_view feature_name) const {
    return std::find(enabled_feature_names.begin(), enabled_feature_names.end(), feature_name) !=
           enabled_feature_names.end();
}

const std::vector<std::string> &RenderingPassContainer::getEnabledFeatures() const {
    return enabled_feature_names;
}

bool RenderingPassContainer::hasMaterialPasses() const {
    for (const auto rendering_pass_id : registered_pass_ids) {
        const auto &rendering_pass = rendering_passes.get(rendering_pass_id);
        if (std::any_of(rendering_pass.passes.begin(), rendering_pass.passes.end(),
                        [](const auto &pass) { return pass.definition.isMaterial(); })) {
            return true;
        }
    }
    return false;
}

bool RenderingPassContainer::supportsMaterialPass(
    MaterialRouteClass route, MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) const {
    for (const auto rendering_pass_id : registered_pass_ids) {
        const auto &rendering_pass = rendering_passes.get(rendering_pass_id);
        for (const auto &compiled : rendering_pass.passes) {
            const auto &pass = compiled.definition;
            if (!pass.isMaterial()) continue;
            if (exact_pass && pass.name != *exact_pass) continue;
            if (materialPassAcceptsMaterial(pass.materialInfo().contract, pass.name,
                                            route, shader_contract, exact_pass)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace Pelican
