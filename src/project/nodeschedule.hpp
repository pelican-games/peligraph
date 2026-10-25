#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Pelican {

// When a node runs: once for the frame, once per logical view (a compute
// task only), or only on a frame where its inputs or outputs changed
// (PS-1). The one definition of the set, its names and its default: the
// render graph IR, the execution (ComputeTaskSchedule is this) and the graph
// parameter schema read these.
enum class NodeSchedule : std::uint8_t {
    per_frame,
    per_view,
    once,
};

inline constexpr NodeSchedule defaultNodeSchedule = NodeSchedule::per_frame;

constexpr std::string_view nodeScheduleName(NodeSchedule schedule) {
    switch (schedule) {
    case NodeSchedule::per_frame:
        return "per_frame";
    case NodeSchedule::per_view:
        return "per_view";
    case NodeSchedule::once:
        return "once";
    }
    return "per_frame";
}

constexpr std::optional<NodeSchedule> nodeScheduleFromName(std::string_view name) {
    if (name == "per_frame") return NodeSchedule::per_frame;
    if (name == "per_view") return NodeSchedule::per_view;
    if (name == "once") return NodeSchedule::once;
    return std::nullopt;
}

} // namespace Pelican
