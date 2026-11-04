#pragma once

#include "../../core/output/outputsinktable.hpp"

namespace Pelican {

struct VulkanTargetPlan;

// Compile-time last-writer check. A target's later writer makes the sink
// refused with output.source_overwritten@1 before it ever reads a pixel.
// Only the plan of the sink's graph is consulted.
void validateOutputSinkWriters(OutputSinkTable &table,
                               const VulkanTargetPlan &plan);

} // namespace Pelican
