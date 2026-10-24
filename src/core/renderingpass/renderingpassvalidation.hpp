#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

class RenderTargetMetadataResolver;

void validatePassInputs(const PassDefinition &pass_def);
// RC-3c2a: the resource checks (usage, extent, samples, material
// attachments) are project's validateRenderNodeResources, run on the IR and
// the generation's description before the GPU registration; the pass parse
// only resolves.
vk::SampleCountFlagBits passOutputSamples(
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata);

} // namespace Pelican
