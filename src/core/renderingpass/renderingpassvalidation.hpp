#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace Pelican {

class RenderTargetMetadataResolver;

using ProducedRenderTargetSet = std::unordered_set<GlobalRenderTargetId, GlobalRenderTargetId::Hash>;

void validatePassInputs(const PassDefinition &pass_def);
void validatePassTargetUsage(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);
void validateUniqueRenderTargets(const std::vector<GlobalRenderTargetId> &targets,
                                 const std::string &target_kind,
                                 const PassDefinition &pass_def,
                                 const RenderTargetMetadataResolver &rt_metadata);
void validatePassOutputExtents(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);
vk::SampleCountFlagBits resolvePassOutputSamples(
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata);
void validateMaterialPassAttachments(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);
void validatePassOutputs(const PassDefinition &pass_def);
void validatePassSpecificFields(const PassDefinition &pass_def, const nlohmann::json &pass_json);
void validatePassInputsProduced(const PassDefinition &pass_def,
                                const ProducedRenderTargetSet &produced_targets,
                                const RenderTargetMetadataResolver &rt_metadata);
void recordPassOutputs(const PassDefinition &pass_def, ProducedRenderTargetSet &produced_targets);

} // namespace Pelican
