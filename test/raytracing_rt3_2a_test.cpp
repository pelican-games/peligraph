#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/shader/shaderreference.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/renderer/proceduralgeometrycomponent.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"

#include <array>
#include <limits>
#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

nlohmann::json v3Layout() {
    return {
        {"schema", "pelican.ray_group_layout"},
        {"version", 3},
        {"ray_types", {"primary"}},
        {"default_group", "surface"},
        {"groups", {{{"name", "surface"}, {"type", "triangles"}},
                    {{"name", "sphere"}, {"type", "procedural"}}}},
        {"misses", {{{"name", "sky"}}}},
        {"record", {{{"name", "radius"}, {"type", "f32"}}}},
        {"acceleration_structure",
         {{"defaults", {{"mask", 255}, {"instance_flags", nlohmann::json::array()},
                         {"geometry_flags", nlohmann::json::array()}}},
          {"procedural_defaults", {{"geometry_flags", {"opaque"}}}},
          {"rules", nlohmann::json::array()}}},
    };
}

} // namespace

TEST_CASE("WP386a layout v3 and binding v2 resolve procedural rows without a material join",
          "[WP386a][ray-tracing][rt3-2a][cpu]") {
    const auto layout = parseRayGroupLayout(v3Layout().dump(), "layout.json");
    REQUIRE(layout.semantic_mode == RayLayoutSemanticMode::LayoutV3);
    REQUIRE(requiresRayLayoutV2(layout.semantic_mode));
    REQUIRE(layout.group_types.size() == 2);
    CHECK(layout.group_types[1] == RayGroupType::procedural);
    REQUIRE(layout.acceleration_structure);
    CHECK(layout.acceleration_structure->procedural_defaults.geometry_flags ==
          static_cast<std::uint32_t>(vk::GeometryFlagBitsKHR::eOpaque));

    const auto bindings = parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":2,
      "bindings":[{"procedural":"sphere_asset","group":"sphere"}]
    })json", layout, "bindings.json");
    REQUIRE(bindings.version == 2);

    const std::array inputs{RaySceneRowInput{
        .geometry_kind = RayGeometryKind::aabbs,
        .scene_identity = "41:component-0",
        .geometry_identity = "sphere_asset#0",
        .geometry_match_identity = "aabb:41",
        .geometry_allocation_id = 91,
        .procedural_asset = "sphere_asset",
        .procedural_record = nlohmann::json{{"radius", 1.25}},
        .aabb_buffer_address = 0x1000,
        .authoring_object_id = 41,
    }};
    const auto rows = resolveRaySceneRows(inputs, layout, &bindings);
    REQUIRE(rows.rows.size() == 1);
    CHECK(rows.rows[0].geometry_kind == RayGeometryKind::aabbs);
    CHECK(rows.rows[0].group_index == 1);
    CHECK(rows.rows[0].material_reference.empty());
    CHECK(rows.rows[0].material_index ==
          std::numeric_limits<std::uint32_t>::max());
    CHECK(rows.rows[0].geometry_flags ==
          static_cast<std::uint32_t>(vk::GeometryFlagBitsKHR::eOpaque));
    REQUIRE(rows.rows[0].record_data.size() == sizeof(float));

    CHECK_THROWS_WITH(parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":2,
      "bindings":[{"procedural":"sphere_asset","group":"sphere",
                    "record":{"radius":1.0}}]
    })json", layout, "bad.json"),
      Catch::Matchers::ContainsSubstring(
          "pelican.procedural_binding_record_forbidden@1"));

    CHECK_THROWS_WITH(parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":2,
      "bindings":[{"material":{"asset":"m","source_material_index":0},
                    "procedural":"sphere_asset","group":"sphere"}]
    })json", layout, "both.json"),
      Catch::Matchers::ContainsSubstring(
          "pelican.ray_binding_selector_exactly_one@1"));
    CHECK_THROWS_WITH(parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":2,
      "bindings":[{"group":"sphere"}]
    })json", layout, "none.json"),
      Catch::Matchers::ContainsSubstring(
          "pelican.ray_binding_selector_exactly_one@1"));
    CHECK_THROWS_WITH(parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":2,
      "bindings":[{"procedural":"sphere_asset","group":"sphere"},
                  {"procedural":"sphere_asset","group":"sphere"}]
    })json", layout, "duplicate.json"),
      Catch::Matchers::ContainsSubstring(
          "pelican.procedural_selector_duplicate@1"));

    CHECK_THROWS_WITH(resolveRaySceneRows(inputs, layout, nullptr),
      Catch::Matchers::ContainsSubstring(
          "pelican.procedural_group_unresolved@1"));
    auto wrong_group = bindings;
    wrong_group.bindings[0].group_index = 0;
    CHECK_THROWS_WITH(resolveRaySceneRows(inputs, layout, &wrong_group),
      Catch::Matchers::ContainsSubstring(
          "pelican.procedural_group_kind_conflict@1"));
}

TEST_CASE("WP386a inventory is a canonical typed union and AABB geometry table preserves its address",
          "[WP386a][ray-tracing][rt3-2a][inventory][cpu]") {
    RayQueryGeometryInstanceSnapshot triangle{
        .asset_identity = 2,
        .authoring_object_id = 9,
        .component_occurrence = "model",
        .geometry_allocation_id = 22,
        .index_count = 3,
        .vertex_count = 3,
    };
    RayQueryGeometryInstanceSnapshot procedural{
        .geometry_kind = RayGeometryKind::aabbs,
        .asset_identity = 1,
        .authoring_object_id = 3,
        .component_occurrence = "proc",
        .geometry_allocation_id = 11,
        .procedural_asset = "sphere_asset",
        .aabb_buffer_address = 0x4000,
    };
    const auto inventory = makeRaySceneInventorySnapshot(
        std::array{triangle}, std::array{procedural}, 7);
    REQUIRE(inventory.generation == 7);
    REQUIRE(inventory.rows.size() == 2);
    CHECK(std::holds_alternative<ProceduralRow>(inventory.rows[0]));
    CHECK(std::holds_alternative<TriangleRow>(inventory.rows[1]));
    CHECK(inventory.instances[0].geometry_kind == RayGeometryKind::aabbs);

    const auto geometry = makeRayTracingGeometrySnapshot(
        inventory.instances, 0x10000, 0x20000);
    REQUIRE(geometry.rows.size() == 2);
    CHECK(geometry.rows[0].geometry_kind == RayGeometryKind::aabbs);
    CHECK(geometry.rows[0].gpu.vertex_address == 0x4000);
    CHECK(geometry.rows[0].gpu.index_address == 0);
    CHECK(geometry.rows[0].gpu.vertex_stride == sizeof(vk::AabbPositionsKHR));
}

TEST_CASE("WP386a intersection shader references close the rint vocabulary",
          "[WP386a][ray-tracing][rt3-2a][shader][cpu]") {
    const auto reference = makeShaderReference(
        "engine://procedural_no_report", ShaderStage::intersection);
    CHECK(reference.ref == "engine://procedural_no_report");
    CHECK(std::string{shaderStageSourceExtension(
              ShaderStage::intersection)} == ".rint");
    CHECK(hasKnownShaderExtension("sphere.rint",
                                  ShaderStage::intersection));
    CHECK(shaderStageName(ShaderStage::intersection) == "intersection");
    REQUIRE(inferShaderStageFromPath("sphere.rint"));
    CHECK(*inferShaderStageFromPath("sphere.rint") ==
          vk::ShaderStageFlagBits::eIntersectionKHR);

    PathResolver resolver;
    resolver.setup(std::filesystem::current_path(), false);
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto id = library.loadFromReference(reference, resolver, true);
    const auto &entry = selectRayTracingEntryPoint(
        library.get(id).reflection.ray_tracing_module,
        ShaderStage::intersection, "main", "engine://procedural_no_report");
    CHECK(entry.key.execution_model ==
          spirvExecutionModelForShaderStage(ShaderStage::intersection));
    library.retireRegistrations(std::vector{id});
    CHECK(library.registrationCount() == 0);
}

TEST_CASE("WP386a procedural component is one strict fixed leaf",
          "[WP386a][component][cpu]") {
    const auto *codec = findComponentCodec("pelican_procedural_geometry");
    REQUIRE(codec != nullptr);
    auto decoded = codec->decodeAuthored(nlohmann::json{
        {"name", "pelican_procedural_geometry"},
        {"asset", "sphere_asset"}});
    REQUIRE(decoded.hasValue());
    CHECK(std::any_cast<const ProceduralGeometryComponent &>(
              decoded.value())
              .asset == "sphere_asset");
    CHECK_THROWS_WITH(codec->decodeAuthored(nlohmann::json{
        {"name", "pelican_procedural_geometry"},
        {"asset", "sphere_asset"},
        {"entries", nlohmann::json::array()}}),
        Catch::Matchers::ContainsSubstring("field 'pelican_procedural_geometry.entries' is unknown"));
}

} // namespace Pelican
