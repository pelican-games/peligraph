#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace Pelican {

class RenderTargetContainer;

using ProducedColorTargetSet = std::unordered_set<GlobalRenderTargetId, GlobalRenderTargetId::Hash>;

void validatePassInputs(const PassDefinition &pass_def);
void validatePassTargetUsage(const PassDefinition &pass_def, RenderTargetContainer &rt_container);
void validateUniqueRenderTargets(const std::vector<GlobalRenderTargetId> &targets,
                                 const std::string &target_kind,
                                 const PassDefinition &pass_def,
                                 RenderTargetContainer &rt_container);
void validatePassOutputExtents(const PassDefinition &pass_def, RenderTargetContainer &rt_container);
void validateMaterialPassAttachments(const PassDefinition &pass_def, RenderTargetContainer &rt_container);
void validatePassOutputs(const PassDefinition &pass_def);
void validatePassSpecificFields(const PassDefinition &pass_def, const nlohmann::json &pass_json);
void validatePassInputsProduced(const PassDefinition &pass_def,
                                const ProducedColorTargetSet &produced_color_targets,
                                RenderTargetContainer &rt_container);
void recordPassOutputs(const PassDefinition &pass_def, ProducedColorTargetSet &produced_color_targets);

} // namespace Pelican
