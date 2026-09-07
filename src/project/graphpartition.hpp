#pragma once

#include "graphcomposition.hpp"

namespace Pelican {
struct GraphExecutionPartitions {
    ExpandedGraphDocument values;
    ExpandedGraphDocument rendering;
};
GraphOperationContracts valueGraphOperationContracts(const LogicalTypeRegistry &registry);
// Validate the entire authored graph before separating execution domains.
// Connected scalar buffer inputs become CPU publication sinks and host buffers.
GraphExecutionPartitions partitionGraphDocument(const nlohmann::json &document);
// Resolve only instantiated attribute sources. The returned compiler document
// contains derived counts; the authored document remains unchanged.
using GraphAttributeSourceResolver = std::function<std::uint64_t(
    std::string_view node, const nlohmann::json &parameters)>;
nlohmann::json resolveGraphAttributeSources(const nlohmann::json &document,
    const GraphAttributeSourceResolver &resolve);
}
