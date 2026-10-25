#pragma once

#include "render_graph_ir_test_support.hpp"
#include "../src/core/renderingpass/passdefinitionfromir.hpp"

namespace Pelican::TestSupport {

// The production path: normalize and parse once, then the common conversion
// with the declared generation resources. Tests supply their image/buffer
// declarations explicitly, and may inject concrete registry IDs.
inline std::vector<RenderingPassDefinition> renderingPassDefinitionsOf(
    const nlohmann::json &config, const RenderTargetNameResolver &names) {
    const auto ir = renderGraphOf(config);
    const auto resources = resolveDeclaredGenerationResources(ir, {1280, 720});
    return Pelican::renderingPassDefinitionsOf(ir, resources, names);
}

inline std::vector<RenderingPassDefinition> renderingPassDefinitionsOf(
    const nlohmann::json &config) {
    const auto ir = renderGraphOf(config);
    const auto resources = resolveDeclaredGenerationResources(ir, {1280, 720});
    return Pelican::renderingPassDefinitionsOf(ir, resources, generationRenderTargetNames(resources));
}

} // namespace Pelican::TestSupport
