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
}
