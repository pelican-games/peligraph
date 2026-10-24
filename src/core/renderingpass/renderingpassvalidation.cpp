#include "renderingpassvalidation.hpp"
#include "materialpassattachments.hpp"
#include "rendertargetmetadataresolver.hpp"
#include "vulkanimagevocabulary.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>

namespace Pelican {

void validatePassInputs(const PassDefinition &pass_def) {
    if (pass_def.isMaterial()) {
        if (!pass_def.input_buffers.empty()) {
            for (const auto &buffer :
                 pass_def.input_buffers) {
                const auto named = std::find_if(
                    pass_def.materialInfo()
                        .material_resources.begin(),
                    pass_def.materialInfo()
                        .material_resources.end(),
                    [&](const auto &resource) {
                        return resource.buffer == buffer;
                    });
                if (named ==
                    pass_def.materialInfo()
                        .material_resources.end()) {
                    throw std::runtime_error(
                        "Material pass buffer inputs must use named "
                        "material_resources: " +
                        pass_def.name);
                }
            }
        }
        if (!pass_def.input_targets.empty() &&
            pass_def.materialInfo().screen_inputs.empty() &&
            pass_def.materialInfo().surface_resources.empty() &&
            pass_def.materialInfo().material_resources.empty()) {
            throw std::runtime_error(
                "Material pass inputs must use named screen_inputs or "
                "surface_resources/material_resources: " +
                pass_def.name);
        }
    }

    if (pass_def.input_target_history.size() != pass_def.input_targets.size()) {
        throw std::runtime_error("Pass input history metadata is inconsistent: " + pass_def.name);
    }
}

// RC-3c2a: the pass's rasterization sample count, from its attachments
// (the swapchain's is 1). That the attachments agree is the resource check's
// (validateRenderNodeResources), before anything is registered.
vk::SampleCountFlagBits passOutputSamples(
    const PassDefinition &pass_def,
    const RenderTargetMetadataResolver &rt_metadata) {
    std::optional<std::uint32_t> samples;
    for (const auto &attachment : pass_def.output_color) {
        if (samples) break;
        if (isSwapchainRenderTarget(attachment.target)) {
            samples = 1;
        } else if (isConcreteRenderTarget(attachment.target)) {
            samples = rt_metadata.get(attachment.target).samples;
        }
    }
    if (!samples && isConcreteRenderTarget(pass_def.output_depth)) {
        samples = rt_metadata.get(pass_def.output_depth).samples;
    }
    switch (samples.value_or(1)) {
    case 1: return vk::SampleCountFlagBits::e1;
    case 2: return vk::SampleCountFlagBits::e2;
    case 4: return vk::SampleCountFlagBits::e4;
    case 8: return vk::SampleCountFlagBits::e8;
    case 16: return vk::SampleCountFlagBits::e16;
    case 32: return vk::SampleCountFlagBits::e32;
    case 64: return vk::SampleCountFlagBits::e64;
    default:
        throw std::runtime_error(
            "Pass output target has invalid sample count in pass: " +
            pass_def.name);
    }
}

} // namespace Pelican
