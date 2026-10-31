#include "memoryregionpacking.hpp"
#include <algorithm>
#include <cassert>
#include <limits>
#include <set>
#include <stdexcept>

namespace Pelican {
namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
std::uint64_t endOf(std::uint64_t offset, std::uint64_t size) {
    require(size <= std::numeric_limits<std::uint64_t>::max() - offset,
            "memory.packing_overflow");
    return offset + size;
}
std::uint64_t alignUp(std::uint64_t offset, std::uint64_t alignment) {
    const auto remainder = offset % alignment;
    return remainder ? endOf(offset, alignment - remainder) : offset;
}
}

MemoryPackingResult packMemoryRegions(
    const MemoryPackingInputs &inputs, const MemoryPackingRequirements &requirements) {
    std::map<std::string, std::size_t, std::less<>> index;
    for (std::size_t i = 0; i < inputs.resources.size(); ++i) {
        const auto &r = inputs.resources[i];
        assert(r.optimal_tiling);
        require(index.emplace(r.resource, i).second, "memory.duplicate_resource");
    }
    std::set<std::pair<std::size_t, std::size_t>> forbidden;
    for (const auto &[a, b] : inputs.forbidden_pairs) {
        const auto x = index.at(a), y = index.at(b);
        require(x != y, "memory.self_forbidden");
        forbidden.emplace(std::min(x, y), std::max(x, y));
    }
    struct Slot {
        std::vector<std::size_t> members;
        std::uint64_t size = 0, alignment = 1;
        std::uint32_t bits = 0;
    };
    std::vector<Slot> slots;
    std::vector<std::size_t> slot_of(index.size(), index.size());
    for (const auto &set : inputs.same_offset_sets) {
        require(!set.empty(), "memory.empty_same_offset_set");
        Slot slot;
        for (const auto &name : set) {
            const auto i = index.at(name);
            require(slot_of[i] == index.size(), "memory.duplicate_same_offset_member");
            slot_of[i] = slots.size();
            slot.members.push_back(i);
        }
        slots.push_back(std::move(slot));
    }
    for (std::size_t i = 0; i < index.size(); ++i) if (slot_of[i] == index.size()) {
        slot_of[i] = slots.size();
        slots.push_back({{i}});
    }
    MemoryPackingResult result;
    std::vector<bool> eligible(slots.size(), true);
    for (std::size_t s = 0; s < slots.size(); ++s) {
        auto &slot = slots[s];
        for (const auto i : slot.members) {
            const auto found = requirements.find(inputs.resources[i].resource);
            if (found == requirements.end() || !found->second ||
                found->second->requires_dedicated || !found->second->memory_type_bits) {
                eligible[s] = false;
                continue;
            }
            const auto &r = *found->second;
            require(r.size && r.alignment && !(r.alignment & (r.alignment - 1)),
                    "memory.invalid_requirements");
            if (slot.bits && slot.bits != r.memory_type_bits)
                throw std::runtime_error("target_planning.alias_generation_contract@1: incompatible same-offset memory keys");
            slot.bits = r.memory_type_bits;
            slot.size = std::max(slot.size, r.size);
            slot.alignment = std::max(slot.alignment, r.alignment);
        }
        for (const auto a : slot.members) for (const auto b : slot.members)
            require(!forbidden.contains({std::min(a, b), std::max(a, b)}),
                    "target_planning.alias_generation_contract@1: forbidden same-offset pair");
        if (!eligible[s]) for (const auto i : slot.members)
            result.excluded.push_back(inputs.resources[i].resource);
    }
    std::vector<std::size_t> order;
    std::set<std::size_t> seen;
    std::set<std::string, std::less<>> ordered_resources;
    for (const auto &name : inputs.order) {
        require(ordered_resources.insert(name).second, "memory.duplicate_order_resource");
        const auto s = slot_of[index.at(name)];
        if (seen.insert(s).second) order.push_back(s);
    }
    require(inputs.order.size() == index.size() && seen.size() == slots.size(),
            "memory.incomplete_order");
    // Explicit Fisher-Yates/LCG: reproducible across standard libraries.
    if (inputs.seed) {
        std::uint32_t state = *inputs.seed;
        for (std::size_t n = order.size(); n > 1; --n) {
            state = state * 1664525u + 1013904223u;
            std::swap(order[n - 1], order[state % n]);
        }
    }
    struct Placed { std::size_t slot; std::uint64_t offset; };
    std::map<std::uint32_t, std::vector<Placed>> placed;
    std::map<std::uint32_t, std::size_t> region_of;
    for (const auto s : order) {
        if (!eligible[s]) continue;
        const auto &slot = slots[s];
        const auto conflicts = [&](std::size_t other) {
            for (const auto a : slot.members) for (const auto b : slots[other].members)
                if (forbidden.contains({std::min(a, b), std::max(a, b)})) return true;
            return false;
        };
        auto &previous = placed[slot.bits];
        std::uint64_t offset = 0;
        for (;;) {
            offset = alignUp(offset, slot.alignment); // RC7C_ALIGNMENT
            const auto end = endOf(offset, slot.size);
            std::uint64_t next = offset;
            for (const auto &p : previous) {
                const auto other_end = endOf(p.offset, slots[p.slot].size);
                if (conflicts(p.slot) && offset < other_end && p.offset < end) // RC7C_OVERLAP
                    next = std::max(next, other_end);
            }
            if (next == offset) break;
            offset = next;
        }
        if (!region_of.contains(slot.bits)) {
            region_of.emplace(slot.bits, result.regions.size());
            result.regions.push_back({.region = static_cast<std::uint32_t>(result.regions.size()),
                                      .memory_type_bits = slot.bits});
        }
        auto &region = result.regions[region_of.at(slot.bits)];
        region.size = std::max(region.size, endOf(offset, slot.size));
        region.alignment = std::max(region.alignment, slot.alignment);
        for (const auto i : slot.members) {
            const auto &name = inputs.resources[i].resource;
            const auto &r = *requirements.at(name);
            region.residents.push_back({name, offset, r.size, r.alignment});
        }
        previous.push_back({s, offset});
    }
    return result;
}
MemoryPackingResult repackMemoryRegions(const MemoryPackingInputs &inputs,
    const MemoryPackingRequirements &requirements,
    const std::vector<std::vector<std::string>> &required_sets,
    std::map<std::string, std::string, std::less<>> &rejections) {
    const auto eligible = [&](const std::string &name) {
        const auto r=requirements.find(name);
        return r!=requirements.end() && r->second && r->second->memory_type_bits && !r->second->requires_dedicated;
    };
    for(const auto &set:required_sets) for(const auto &name:set) {
        if(!eligible(name) || requirements.at(name)->memory_type_bits != requirements.at(set.front())->memory_type_bits)
            throw std::runtime_error("render_target.alias_repack_unsatisfiable@1: " + name + " / " + set.front());
    }
    for(const auto &r:inputs.resources) {
        if(!eligible(r.resource)) rejections[r.resource]="dedicated_or_unknown_requirements";
        else if(r.memory_type_bits && r.memory_type_bits!=requirements.at(r.resource)->memory_type_bits)
            rejections[r.resource]="memory_key_changed";
    }
    auto next=inputs;
    next.same_offset_sets.clear();
    for(const auto &set:inputs.same_offset_sets) {
        std::map<std::uint32_t,std::vector<std::string>> partitions;
        for(const auto &name:set) {
            if(!eligible(name)) { rejections[name]="dedicated_or_unknown_requirements"; continue; }
            partitions[requirements.at(name)->memory_type_bits].push_back(name);
        }
        for(auto &[bits,members]:partitions) {
            if(members.size()!=set.size()) for(const auto &name:members)
                rejections[name]=partitions.size()>1 ? "same_offset_memory_key_changed" : "same_offset_dedicated_or_unknown";
            if(members.size()>1) next.same_offset_sets.push_back(std::move(members));
        }
    }
    return packMemoryRegions(next, requirements);
}
} // namespace Pelican
