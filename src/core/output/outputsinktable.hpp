#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// VO-1a: one removable output.file node, as the authoring lowering leaves it.
// The public `reports["output"]` row intentionally carries only the fields the
// editor shows; the source node and socket stay here for the compile-time
// last-writer check.
struct OutputSinkSpec {
    std::string name;
    std::string kind = "file";
    std::uint64_t every_n = 1;
    bool enabled = true;
    std::string graph;
    std::string target;
    std::string source_node;
    std::string source_port;
    // Compile-time refusal, carried to the runtime but never in the public
    // report row. Empty means the sink may read.
    std::string error_code;
    std::string error_message;

    bool operator==(const OutputSinkSpec &) const = default;
};

struct OutputSinkTable {
    std::vector<OutputSinkSpec> sinks;

    bool operator==(const OutputSinkTable &) const = default;
};

// VO-1a: whether a sink's authored graph is the active target plan's graph.
// An inactive plan must not read pixels; the sink reports
// frames_skipped_inactive instead. Inline so the runtime and the compile-time
// writer check use exactly the CPU-tested rule.
inline bool outputSinkGraphMatches(std::string_view sink_graph,
                                   std::string_view plan_graph) {
    return !sink_graph.empty() && sink_graph == plan_graph;
}

} // namespace Pelican
