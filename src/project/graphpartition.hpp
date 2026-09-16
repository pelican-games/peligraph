#pragma once

#include "graphcomposition.hpp"
#include "graphmodule.hpp"

namespace Pelican {
struct GraphExecutionPartitions {
    ExpandedGraphDocument values;
    ExpandedGraphDocument rendering;
    // Evaluated inside the animation phase, one step earlier in the same frame
    // than `values`. Every scalar an animation node consumes is published by
    // `values` as a `$pose/<node>/<port>` output, so the feed is one-way.
    ExpandedGraphDocument animation;
};
// The scalar output name the values partition publishes for one animation-node
// input port. Both sides spell it here.
std::string graphAnimationScalarOutput(std::string_view node, std::string_view port);
// Validate the entire authored graph before separating execution domains.
// Connected scalar buffer inputs become CPU publication sinks and host buffers.
// Every operation comes from a registered module; the overload taking a registry
// is how a test, or a build without some module, sees a different set.
GraphExecutionPartitions partitionGraphDocument(const nlohmann::json &document);
GraphExecutionPartitions partitionGraphDocument(const nlohmann::json &document,
                                                const GraphModuleRegistry &modules);
// Resolve only instantiated attribute sources. The returned compiler document
// contains derived counts; the authored document remains unchanged.
using GraphAttributeSourceResolver = std::function<std::uint64_t(
    std::string_view node, const nlohmann::json &parameters)>;
nlohmann::json resolveGraphAttributeSources(const nlohmann::json &document,
    const GraphAttributeSourceResolver &resolve);
}
