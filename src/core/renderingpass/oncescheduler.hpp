#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// PS-1: which `schedule: "once"` nodes a frame records. A once node runs on
// the frame where something it depends on changed and is skipped (nothing
// recorded) on every other frame:
//   - generation:       it has not run in this runtime generation
//   - requested:        rerun_once named it
//   - output_recreated: a resource it writes is not the one it wrote
//                       (a persistent image made anew: extent, rebuild)
//   - input_changed:    a host-written buffer it reads holds other bytes
//                       than when it ran (a render.scalar sink, runtime_f32)
//   - upstream:         a once node earlier in the frame that writes what
//                       it reads runs this frame (h0 -> twiddle)
// The decision is pure so a test can drive it without a device.

struct OnceScheduleNode {
    std::string name;
    // What the node reads this frame (history reads excluded) and writes.
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

struct OnceRunRecord {
    std::uint64_t generation = 0;
    std::uint64_t frame = 0;
    // The identity (image / buffer handle) of each resource it wrote.
    std::map<std::string, std::uint64_t, std::less<>> written;
    // The host digest of each host-written resource it read.
    std::map<std::string, std::uint64_t, std::less<>> read;

    bool operator==(const OnceRunRecord &) const = default;
};

struct OnceResourceFacts {
    // The physical object behind a resource name, when there is one.
    std::function<std::optional<std::uint64_t>(std::string_view)> identity;
    // The digest of the bytes the host last wrote, for a host-written buffer.
    std::function<std::optional<std::uint64_t>(std::string_view)> host_digest;
};

struct OncePlan {
    // Parallel to the nodes given: the reason a node runs, empty when it is
    // skipped this frame.
    std::vector<std::optional<std::string>> reasons;

    bool runs(std::size_t index) const {
        return index < reasons.size() && reasons[index].has_value();
    }
};

// `nodes` are the frame's once nodes in plan order. `last` is what each ran
// with last time, by name; `requested` the names rerun_once asked for.
OncePlan planOnceRuns(
    std::span<const OnceScheduleNode> nodes,
    const std::map<std::string, OnceRunRecord, std::less<>> &last,
    std::uint64_t generation,
    const OnceResourceFacts &facts,
    const std::set<std::string, std::less<>> &requested);

// What a node that ran this frame keeps for the next decision.
OnceRunRecord onceRunRecord(const OnceScheduleNode &node,
                            std::uint64_t generation,
                            std::uint64_t frame,
                            const OnceResourceFacts &facts);

} // namespace Pelican
