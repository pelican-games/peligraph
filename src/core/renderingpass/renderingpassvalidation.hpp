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
void validatePassOutputExtents(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);
vk::SampleCountFlagBits resolvePassOutputSamples(
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata);
void validateMaterialPassAttachments(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);
void validatePassInputsProduced(const PassDefinition &pass_def,
                                const ProducedRenderTargetSet &produced_targets,
                                const RenderTargetMetadataResolver &rt_metadata);
void recordPassOutputs(const PassDefinition &pass_def, ProducedRenderTargetSet &produced_targets);

} // namespace Pelican
