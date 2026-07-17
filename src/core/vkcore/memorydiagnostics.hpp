#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

class VulkanManageCore;
class VertBufContainer;
class MaterialContainer;

struct DriverMemoryHeapStatus {
    std::uint32_t heap_index = 0;
    bool device_local = false;
    std::uint64_t size = 0;
    std::uint64_t usage = 0;
    std::uint64_t budget = 0;
    std::string source;
};

struct DriverMemoryStatus {
    bool available = false;
    std::string reason;
    std::vector<DriverMemoryHeapStatus> heaps;
};

struct EngineMemoryCategoryStatus {
    std::string category;
    std::optional<std::uint64_t> allocated_bytes;
    std::optional<std::uint64_t> logical_used_bytes;
    std::optional<std::uint64_t> free_bytes;
    std::optional<std::uint64_t> high_water_bytes;
    std::uint64_t object_count = 0;
    std::string source;
    std::optional<std::string> reason;
};

struct MemoryStatusSnapshot {
    DriverMemoryStatus driver;
    std::vector<EngineMemoryCategoryStatus> engine_categories;
};

MemoryStatusSnapshot collectMemoryStatus(const VulkanManageCore &vulkan,
                                         const VertBufContainer *geometry,
                                         const MaterialContainer *materials);
nlohmann::json memoryStatusJson(const MemoryStatusSnapshot &status);

} // namespace Pelican
