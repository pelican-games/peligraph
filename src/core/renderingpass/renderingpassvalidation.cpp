#include "renderingpassvalidation.hpp"
#include <algorithm>
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

} // namespace Pelican
