#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

class RenderTargetMetadataResolver;

void validatePassInputs(const PassDefinition &pass_def);
void validatePassTargetUsage(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);
void validatePassOutputExtents(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);
vk::SampleCountFlagBits resolvePassOutputSamples(
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata);
void validateMaterialPassAttachments(const PassDefinition &pass_def, const RenderTargetMetadataResolver &rt_metadata);

} // namespace Pelican
