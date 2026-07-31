#include "materialshaderconfig.hpp"

#include "../container.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"

#include <algorithm>

namespace Pelican {

std::vector<std::string> activeMaterialShaderDefines() {
    const auto *passes =
        FastModuleContainer::tryGet<RenderingPassContainer>();
    if (passes == nullptr) {
        return {};
    }
    const auto generation = passes->snapshot();
    if (generation == nullptr) {
        return {};
    }
    for (const auto pass_id : generation->rendering_pass_ids) {
        const auto *program = generation->find(pass_id);
        if (program == nullptr ||
            !program->frame_graph.render_pipeline) {
            continue;
        }
        const auto has_material_pass = std::any_of(
            program->rendering_pass.passes.begin(),
            program->rendering_pass.passes.end(),
            [](const auto &pass) {
                return pass.definition.isMaterial();
            });
        if (has_material_pass) {
            return program->frame_graph.render_pipeline->shader_defines;
        }
    }
    return {};
}

} // namespace Pelican
