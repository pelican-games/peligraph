#include "../src/core/vkcore/memorydiagnostics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace Pelican {

TEST_CASE("memory diagnostics keep driver budgets separate from engine logical accounting",
          "[memory][diagnostics]") {
    MemoryStatusSnapshot snapshot{
        .driver = {
            .available = true,
            .reason = "VK_EXT_memory_budget_enabled",
            .heaps = {{.heap_index = 0,
                       .device_local = true,
                       .size = 8'000,
                       .usage = 2'000,
                       .budget = 6'000,
                       .source = "vk_ext_memory_budget"}},
        },
        .engine_categories = {{.category = "model_vertices",
                               .allocated_bytes = std::nullopt,
                               .logical_used_bytes = 960,
                               .free_bytes = std::nullopt,
                               .high_water_bytes = std::nullopt,
                               .object_count = 10,
                               .source = "vertbuf_free_range",
                               .reason = "pool_capacity_not_exposed"},
                              {.category = "textures",
                               .allocated_bytes = std::nullopt,
                               .logical_used_bytes = std::nullopt,
                               .free_bytes = std::nullopt,
                               .high_water_bytes = std::nullopt,
                               .object_count = 3,
                               .source = "existing_count_surface",
                               .reason = "byte_accounting_not_exposed"}},
    };

    const auto status = memoryStatusJson(snapshot);
    REQUIRE(status.at("schema_version") == 1);
    REQUIRE(status.at("driver_available") == true);
    REQUIRE(status.at("heaps").at(0).at("budget") == 6'000);
    REQUIRE(status.at("heaps").at(0).at("source") == "vk_ext_memory_budget");
    REQUIRE(status.at("engine_categories").at(0).at("logical_used_bytes") == 960);
    REQUIRE(status.at("engine_categories").at(0).at("allocated_bytes").is_null());
    REQUIRE(status.at("engine_categories").at(1).at("object_count") == 3);
}

TEST_CASE("unsupported memory budget is a named absence and never a zero-byte heap",
          "[memory][diagnostics]") {
    const auto status = memoryStatusJson({
        .driver = {.available = false,
                   .reason = "VK_EXT_memory_budget_not_supported",
                   .heaps = {}},
        .engine_categories = {},
    });
    REQUIRE(status.at("driver_available") == false);
    REQUIRE(status.at("driver_reason") == "VK_EXT_memory_budget_not_supported");
    REQUIRE(status.at("heaps").empty());
}

} // namespace Pelican
