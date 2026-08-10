#include "../src/core/vkcore/accelerationstructure.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE(
    "ray-query geometry policy visibly excludes every deformed category",
    "[wp281][ray-query][static-only]") {
    const std::vector<RayQueryGeometryInstanceSnapshot> instances{
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 281,
            .geometry_allocation_id = 1,
            .index_count = 3,
            .vertex_count = 3,
            .mesh_index = 0,
            .primitive_index = 0,
        },
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 281,
            .geometry_allocation_id = 2,
            .skinned = true,
            .mesh_index = 1,
            .primitive_index = 0,
        },
        // A second instance of the same primitive increases the excluded
        // instance count but not the named primitive count.
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 281,
            .geometry_allocation_id = 2,
            .skinned = true,
            .mesh_index = 1,
            .primitive_index = 0,
        },
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 281,
            .geometry_allocation_id = 3,
            .morph_deformed = true,
            .mesh_index = 2,
            .primitive_index = 0,
        },
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 281,
            .geometry_allocation_id = 4,
            .vat_deformed = true,
            .mesh_index = 3,
            .primitive_index = 0,
        },
    };

    const auto classified =
        classifyRayQueryStaticGeometry(instances);
    REQUIRE(classified.static_instance_indices ==
            std::vector<std::size_t>{0});
    CHECK(classified.excluded.primitive_count == 3);
    CHECK(classified.excluded.instance_count == 4);
    CHECK(classified.excluded.skinned_primitive_count == 1);
    CHECK(classified.excluded.morph_primitive_count == 1);
    CHECK(classified.excluded.vat_primitive_count == 1);
    CHECK(classified.excluded.blas_ineligible_primitive_count == 0);
    REQUIRE(classified.excluded.skinned_names.size() == 1);
    REQUIRE(classified.excluded.morph_names.size() == 1);
    REQUIRE(classified.excluded.vat_names.size() == 1);
    CHECK(classified.excluded.skinned_names.front().find(
              "asset[281]/geometry[2]/mesh[1]/primitive[0]") !=
          std::string::npos);
    CHECK(classified.excluded.morph_names.front().find(
              "asset[281]/geometry[3]/mesh[2]/primitive[0]") !=
          std::string::npos);
    CHECK(classified.excluded.vat_names.front().find(
              "asset[281]/geometry[4]/mesh[3]/primitive[0]") !=
          std::string::npos);
}

TEST_CASE(
    "ray-query geometry policy counts BLAS-ineligible static ranges separately",
    "[wp282][ray-query][static-only]") {
    const std::vector<RayQueryGeometryInstanceSnapshot> instances{
        RayQueryGeometryInstanceSnapshot{
            .geometry_allocation_id = 1,
            .index_count = 3,
            .vertex_count = 3,
        },
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 282,
            .geometry_allocation_id = 2,
            .index_count = 4,
            .vertex_count = 4,
            .mesh_index = 1,
        },
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 282,
            .geometry_allocation_id = 3,
            .index_count = 3,
            .vertex_count = 0,
            .mesh_index = 2,
        },
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 282,
            .geometry_allocation_id = 4,
            .index_count = 0,
            .vertex_count = 3,
            .mesh_index = 3,
        },
        RayQueryGeometryInstanceSnapshot{
            .asset_identity = 282,
            .geometry_allocation_id = 5,
            .index_count = 3,
            .vertex_offset = -1,
            .vertex_count = 3,
            .mesh_index = 4,
        },
    };

    const auto classified =
        classifyRayQueryStaticGeometry(instances);
    REQUIRE(classified.static_instance_indices ==
            std::vector<std::size_t>{0});
    CHECK(classified.excluded.primitive_count == 4);
    CHECK(classified.excluded.instance_count == 4);
    CHECK(classified.excluded.skinned_primitive_count == 0);
    CHECK(classified.excluded.morph_primitive_count == 0);
    CHECK(classified.excluded.vat_primitive_count == 0);
    CHECK(classified.excluded.blas_ineligible_primitive_count == 4);
    REQUIRE(classified.excluded.blas_ineligible_names.size() == 4);
    CHECK(classified.excluded.blas_ineligible_names.front().find(
              "asset[282]/geometry[2]/mesh[1]/primitive[0]") !=
          std::string::npos);
}

} // namespace Pelican
