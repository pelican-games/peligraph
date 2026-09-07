#pragma once

#include "../userpublic/valuegraph.hpp"

namespace Pelican {
class SceneLoader;
// Validate source names without reading scene state or evaluating the graph.
void validateGraphInputs(const ValueGraph::Graph &graph);

// Resolve external values on the owner thread against the current scene.
ValueGraph::Inputs collectGraphInputs(const ValueGraph::Graph &graph, const SceneLoader &scene,
    const ValueGraph::InactiveScopes &inactive, double time, double delta_time);
}
