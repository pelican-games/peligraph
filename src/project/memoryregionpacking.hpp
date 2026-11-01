#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>


namespace Pelican {

struct ImageMemoryType {
    std::uint32_t property_flags = 0, heap_index = 0;
    bool operator==(const ImageMemoryType &) const = default;
};
enum class ImageMemoryKind { ordinary, lazy, unknown };
// Only TRANSIENT_ATTACHMENT images may use Vulkan LAZILY_ALLOCATED (0x10).
ImageMemoryKind imageMemoryKind(bool transient, std::uint32_t bits,
                                const std::vector<ImageMemoryType> &types);
inline const char *imageMemoryKindName(ImageMemoryKind kind) {
    return kind==ImageMemoryKind::lazy ? "lazy" : kind==ImageMemoryKind::unknown ? "unknown" : "ordinary";
}

struct ImageMemoryRequirements {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
    std::uint32_t memory_type_bits = 0;
    bool requires_dedicated = false;
    bool prefers_dedicated = false;
    bool operator==(const ImageMemoryRequirements &) const = default;
};

struct MemoryPackingResource {
    std::string resource;
    // Snapshot for the plan. Each pack uses the new requirements' key instead.
    std::uint32_t memory_type_bits = 0;
    bool optimal_tiling = true;
    bool operator==(const MemoryPackingResource &) const = default;
};

// Value input retained by a generation. Packing never changes these fields.
// order is the planner's first_use/size/name order, fixed across repacking.
struct MemoryPackingInputs {
    std::vector<MemoryPackingResource> resources;
    std::vector<std::pair<std::string, std::string>> forbidden_pairs;
    std::vector<std::vector<std::string>> same_offset_sets;
    std::vector<std::string> order;
    std::optional<std::uint32_t> seed;
    bool operator==(const MemoryPackingInputs &) const = default;
};
using MemoryPackingRequirements =
    std::map<std::string, std::optional<ImageMemoryRequirements>, std::less<>>;

struct MemoryRegionResident {
    std::string resource;
    std::uint64_t offset = 0, size = 0, alignment = 1;
    bool operator==(const MemoryRegionResident &) const = default;
};
struct PackedMemoryRegion {
    std::uint32_t region = 0, memory_type_bits = 0;
    std::uint64_t size = 0, alignment = 1;
    std::vector<MemoryRegionResident> residents;
    bool operator==(const PackedMemoryRegion &) const = default;
};
struct MemoryPackingResult {
    std::vector<PackedMemoryRegion> regions;
    // Unknown facts, zero bits, or required dedicated allocation: ordinary path.
    std::vector<std::string> excluded;
    bool operator==(const MemoryPackingResult &) const = default;
};

MemoryPackingResult packMemoryRegions(
    const MemoryPackingInputs &inputs, const MemoryPackingRequirements &requirements);

// Repacking keeps required subsets; preferred slots split when a new key or
// dedicated requirement makes their old same-offset relation impossible.
MemoryPackingResult repackMemoryRegions(const MemoryPackingInputs &inputs,
    const MemoryPackingRequirements &requirements,
    const std::vector<std::vector<std::string>> &required_sets,
    std::map<std::string, std::string, std::less<>> &rejections);

} // namespace Pelican
