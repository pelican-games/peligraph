#pragma once

// RC-3c2b1: a config's compute tasks as the production makes them: the
// config read into the IR (renderGraphOf: the normal form, the test-only
// single-graph form wrapped), the config's task tables applied
// (applyRayTaskTables, the one place they are), then core's
// computeTaskDefinitionsOf against `ray_tracing_project`.

#include "render_graph_ir_test_support.hpp"
#include "../src/core/renderingpass/computetaskdefinition.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace Pelican::TestSupport {

inline std::vector<ComputeTaskDefinition> computeTaskDefinitionsOf(
    const nlohmann::json &config, const RayTracingProjectConfig *ray_tracing_project = nullptr) {
    return Pelican::computeTaskDefinitionsOf(applyRayTaskTables(renderGraphOf(config)), ray_tracing_project);
}

} // namespace Pelican::TestSupport
