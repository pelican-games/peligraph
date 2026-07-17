#include "memorydiagnostics.hpp"

#include "core.hpp"
#include "../material/materialcontainer.hpp"
#include "../model/vertbufcontainer.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

nlohmann::json optionalBytes(const std::optional<std::uint64_t> value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

template <typename Element>
EngineMemoryCategoryStatus logicalRangeCategory(std::string category,
                                                std::uint64_t element_count) {
    const auto bytes = element_count * sizeof(Element);
    return {
        .category = std::move(category),
        .allocated_bytes = std::nullopt,
        .logical_used_bytes = bytes,
        .free_bytes = std::nullopt,
        .high_water_bytes = std::nullopt,
        .object_count = element_count,
        .source = "vertbuf_free_range",
        .reason = "pool_capacity_and_high_water_not_exposed_by_read_only_surface",
    };
}

EngineMemoryCategoryStatus countOnlyCategory(std::string category,
                                             std::uint64_t object_count) {
    return {
        .category = std::move(category),
        .allocated_bytes = std::nullopt,
        .logical_used_bytes = std::nullopt,
        .free_bytes = std::nullopt,
        .high_water_bytes = std::nullopt,
        .object_count = object_count,
        .source = "existing_count_surface",
        .reason = "byte_accounting_not_exposed_by_read_only_surface",
    };
}

} // namespace

MemoryStatusSnapshot collectMemoryStatus(const VulkanManageCore &vulkan,
                                         const VertBufContainer *geometry,
                                         const MaterialContainer *materials) {
    MemoryStatusSnapshot result;
    result.driver = vulkan.driverMemoryStatus();

    if (geometry != nullptr) {
        result.engine_categories.push_back(logicalRangeCategory<std::uint32_t>(
            "model_indices", geometry->allocatedIndexCountForTesting()));
        result.engine_categories.push_back(logicalRangeCategory<CommonVertStruct>(
            "model_vertices", geometry->allocatedVertexCountForTesting()));
        result.engine_categories.push_back(logicalRangeCategory<CommonSkinningVertStruct>(
            "model_skinned_vertices", geometry->allocatedVertexCountForTesting(true)));
        result.engine_categories.push_back(logicalRangeCategory<MorphDeltaGpuData>(
            "model_morph_deltas", geometry->allocatedMorphDeltaCountForTesting()));
    }
    if (materials != nullptr) {
        result.engine_categories.push_back(countOnlyCategory(
            "textures", materials->textureCountForTesting()));
        result.engine_categories.push_back(countOnlyCategory(
            "materials", materials->materialCountForTesting()));
    }
    return result;
}

nlohmann::json memoryStatusJson(const MemoryStatusSnapshot &status) {
    nlohmann::json heaps = nlohmann::json::array();
    for (const auto &heap : status.driver.heaps) {
        heaps.push_back({
            {"heap_index", heap.heap_index},
            {"device_local", heap.device_local},
            {"size", heap.size},
            {"usage", heap.usage},
            {"budget", heap.budget},
            {"source", heap.source},
        });
    }

    nlohmann::json engine_categories = nlohmann::json::array();
    for (const auto &category : status.engine_categories) {
        engine_categories.push_back({
            {"category", category.category},
            {"allocated_bytes", optionalBytes(category.allocated_bytes)},
            {"logical_used_bytes", optionalBytes(category.logical_used_bytes)},
            {"free_bytes", optionalBytes(category.free_bytes)},
            {"high_water_bytes", optionalBytes(category.high_water_bytes)},
            {"object_count", category.object_count},
            {"source", category.source},
            {"reason", category.reason ? nlohmann::json(*category.reason)
                                         : nlohmann::json(nullptr)},
        });
    }

    return {
        {"schema_version", 1},
        {"driver_available", status.driver.available},
        {"driver_reason", status.driver.reason.empty()
                              ? nlohmann::json(nullptr)
                              : nlohmann::json(status.driver.reason)},
        {"heaps", std::move(heaps)},
        {"engine_categories", std::move(engine_categories)},
    };
}

} // namespace Pelican
