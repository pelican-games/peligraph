#include "oncescheduler.hpp"

namespace Pelican {

OncePlan planOnceRuns(
    std::span<const OnceScheduleNode> nodes,
    const std::map<std::string, OnceRunRecord, std::less<>> &last,
    std::uint64_t generation,
    const OnceResourceFacts &facts,
    const std::set<std::string, std::less<>> &requested) {
    OncePlan plan;
    plan.reasons.resize(nodes.size());
    // What the once nodes that run this frame write, so far in plan order.
    std::set<std::string, std::less<>> written_this_frame;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto &node = nodes[index];
        const auto record = last.find(node.name);
        std::optional<std::string> reason;
        if (record == last.end() || record->second.generation != generation) {
            reason = "generation";
        } else if (requested.contains(node.name)) {
            reason = "requested";
        }
        if (!reason) {
            for (const auto &resource : node.reads) {
                if (written_this_frame.contains(resource)) {
                    reason = "upstream";
                    break;
                }
            }
        }
        if (!reason && facts.identity) {
            for (const auto &resource : node.writes) {
                const auto identity = facts.identity(resource);
                if (!identity) continue;
                const auto kept = record->second.written.find(resource);
                if (kept == record->second.written.end() || kept->second != *identity) {
                    reason = "output_recreated";
                    break;
                }
            }
        }
        if (!reason && facts.host_digest) {
            for (const auto &resource : node.reads) {
                const auto digest = facts.host_digest(resource);
                if (!digest) continue;
                const auto kept = record->second.read.find(resource);
                if (kept == record->second.read.end() || kept->second != *digest) {
                    reason = "input_changed";
                    break;
                }
            }
        }
        if (reason) {
            written_this_frame.insert(node.writes.begin(), node.writes.end());
            plan.reasons[index] = std::move(reason);
        }
    }
    return plan;
}

OnceRunRecord onceRunRecord(const OnceScheduleNode &node,
                            std::uint64_t generation,
                            std::uint64_t frame,
                            const OnceResourceFacts &facts) {
    OnceRunRecord record;
    record.generation = generation;
    record.frame = frame;
    if (facts.identity) {
        for (const auto &resource : node.writes) {
            if (const auto identity = facts.identity(resource)) record.written.emplace(resource, *identity);
        }
    }
    if (facts.host_digest) {
        for (const auto &resource : node.reads) {
            if (const auto digest = facts.host_digest(resource)) record.read.emplace(resource, *digest);
        }
    }
    return record;
}

} // namespace Pelican
