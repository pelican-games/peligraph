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

#include <algorithm>
#include <array>
#include <cstring>
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

TEST_CASE("Triangle AS coordinate space is explicit and changes only the AS digest",
          "[ray-tracing][rt6][cpu]") {
    auto encoded = v3Layout();
    const auto implicit = parseRayGroupLayout(encoded.dump(), "layout.json");
    REQUIRE(implicit.acceleration_structure);
    CHECK_FALSE(implicit.acceleration_structure->triangle_world_space);
    encoded["acceleration_structure"]["triangle_space"] = "geometry";
    const auto geometry = parseRayGroupLayout(encoded.dump(), "layout.json");
    CHECK_FALSE(geometry.acceleration_structure->triangle_world_space);
    encoded["acceleration_structure"]["triangle_space"] = "world";
    const auto world = parseRayGroupLayout(encoded.dump(), "layout.json");
    CHECK(world.acceleration_structure->triangle_world_space);
    CHECK(world.acceleration_structure_digest_sha256 != geometry.acceleration_structure_digest_sha256);
    CHECK(world.digest_sha256 == geometry.digest_sha256);
    for (const auto &invalid : nlohmann::json::array({"local", "", 1, true, nullptr, nlohmann::json::object()})) {
        encoded["acceleration_structure"]["triangle_space"] = invalid;
        CHECK_THROWS_WITH(parseRayGroupLayout(encoded.dump(), "layout.json"),
                          Catch::Matchers::ContainsSubstring("triangle_space"));
    }
}

TEST_CASE("Procedural world coordinates require an explicit transform ABI",
          "[ray-tracing][rt6][cpu][procedural-world]") {
    auto encoded = v3Layout();
    encoded["acceleration_structure"]["procedural_space"] = "world";
    CHECK_THROWS_WITH(parseRayGroupLayout(encoded.dump()), Catch::Matchers::ContainsSubstring("requires engine_record procedural_transform"));
    encoded["engine_record"] = {"procedural_transform"};
    const auto world = parseRayGroupLayout(encoded.dump());
    REQUIRE(world.acceleration_structure);
    CHECK(world.acceleration_structure->procedural_world_space);
    REQUIRE(world.physical_record.size() == 7);
    CHECK(world.record[0].offset == 96);
    CHECK(world.engineRecordOffset(RayEngineRecordField::procedural_transform) == 0);
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(world.physical_record[i].type == RayRecordType::vec4);
        CHECK(world.physical_record[i].offset == i * 16);
    }
    encoded["acceleration_structure"]["procedural_space"] = "geometry";
    const auto geometry = parseRayGroupLayout(encoded.dump());
    CHECK_FALSE(geometry.acceleration_structure->procedural_world_space);
    CHECK(world.digest_sha256 == geometry.digest_sha256);
    CHECK(canonicalRayRecordAbiJson(world) == canonicalRayRecordAbiJson(geometry));
    CHECK(world.acceleration_structure_digest_sha256 != geometry.acceleration_structure_digest_sha256);
    for (const auto &invalid : nlohmann::json::array({"local", false, 0})) {
        encoded["acceleration_structure"]["procedural_space"] = invalid;
        CHECK_THROWS_WITH(parseRayGroupLayout(encoded.dump()), Catch::Matchers::ContainsSubstring("procedural_space"));
    }
    encoded["acceleration_structure"]["procedural_space"] = "world";
    encoded["record"].push_back({{"name", "world_to_geometry_0"}, {"type", "vec4"}});
    CHECK_THROWS_WITH(parseRayGroupLayout(encoded.dump()), Catch::Matchers::ContainsSubstring("record name conflicts with engine field"));
    encoded["record"].erase(encoded["record"].end() - 1);
    // MG-2a: an older version is refused as a version, not for what it lacks.
    encoded["version"] = 2;
    CHECK_THROWS_WITH(parseRayGroupLayout(encoded.dump()), Catch::Matchers::ContainsSubstring("pelican.ray_group_layout.version_unsupported@1"));
    encoded["version"] = 3;
    const auto source = makeRayGroupLayoutVirtualInclude(world);
    CHECK(source.find("#define PELICAN_RAY_HAS_PROCEDURAL_TRANSFORM 1") != std::string::npos);
    CHECK(source.find("vec4 geometry_to_world_0;") != std::string::npos);
    CHECK(source.find("pelicanProceduralRayDirection(record)") != std::string::npos);
}

TEST_CASE("Procedural transform records track each occurrence and preserve ray parameters",
          "[ray-tracing][rt6][cpu][procedural-world]") {
    auto encoded = v3Layout();
    encoded["default_group"] = "sphere";
    encoded["engine_record"] = {"procedural_transform"};
    const auto layout = parseRayGroupLayout(encoded.dump());
    RaySceneRowInput first{
        .geometry_kind = RayGeometryKind::aabbs,
        .scene_identity = "first", .geometry_identity = "sphere#0", .geometry_match_identity = "first-transform",
        .geometry_allocation_id = 91, .procedural_asset = "sphere_asset",
        .procedural_record = nlohmann::json{{"radius", 1.25}}, .aabb_buffer_address = 0x1000};
    auto second = first;
    second.scene_identity = "second";
    second.geometry_identity = "sphere#1";
    second.geometry_match_identity = "second-transform";
    second.source_index = 1;
    second.world_transform[0][0] = 2.0F;
    second.world_transform[1][1] = 3.0F;
    second.world_transform[2][2] = -4.0F;
    second.world_transform[3] = glm::vec4{2.0F, -3.0F, 4.0F, 1.0F};
    const std::array inputs{first, second};
    const auto snapshot = resolveRaySceneRows(inputs, layout, nullptr);
    REQUIRE(snapshot.rows.size() == 2);
    CHECK(bool(snapshot.rows[0].record_data != snapshot.rows[1].record_data));
    const auto transformed = std::find_if(snapshot.rows.begin(), snapshot.rows.end(), [](const auto &row) { return row.source_index == 1; });
    REQUIRE(transformed != snapshot.rows.end());
    std::array<float, 24> matrix_rows{};
    std::memcpy(matrix_rows.data(), transformed->record_data.data(), sizeof(matrix_rows));
    CHECK(matrix_rows[0] == 2.0F);
    CHECK(matrix_rows[3] == 2.0F);
    CHECK(matrix_rows[7] == -3.0F);
    CHECK(matrix_rows[11] == 4.0F);
    CHECK(matrix_rows[12] == 0.5F);
    CHECK(matrix_rows[15] == -1.0F);
    CHECK(matrix_rows[19] == 1.0F);
    CHECK(matrix_rows[22] == -0.25F);
    CHECK(matrix_rows[23] == 1.0F);
    // World ray z=12-t maps to local z=-2+t/4. Retaining its
    // unnormalized local direction makes t=8 reach local z=0.
    CHECK(matrix_rows[22] * 12.0F + matrix_rows[23] + 8.0F * (-matrix_rows[22]) == 0.0F);
    auto moved = inputs;
    moved[1].world_transform[3][0] += 1.0F;
    CHECK(resolveRaySceneRows(moved, layout, nullptr).identity_digest_sha256 != snapshot.identity_digest_sha256);
    for (const auto invalid : {0.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto bad = inputs;
        bad[1].world_transform[0][0] = invalid;
        CHECK_THROWS_WITH(resolveRaySceneRows(bad, layout, nullptr), Catch::Matchers::ContainsSubstring("procedural_transform_invalid"));
    }
    auto negative_zero = inputs;
    negative_zero[1].world_transform[0][1] = -0.0F;
    CHECK(resolveRaySceneRows(negative_zero, layout, nullptr).identity_digest_sha256 == snapshot.identity_digest_sha256);
}

TEST_CASE("An authored transform-like name does not enable engine transform encoding",
          "[ray-tracing][rt6][cpu][procedural-world]") {
    auto encoded = v3Layout();
    encoded["default_group"] = "sphere";
    encoded["record"] = {{{"name", "geometry_to_world_0"}, {"type", "vec4"}}};
    const auto layout = parseRayGroupLayout(encoded.dump());
    REQUIRE(layout.engine_record.empty());
    CHECK_FALSE(layout.engineRecordOffset(RayEngineRecordField::procedural_transform).has_value());
    REQUIRE(layout.data_size == 16);
    const std::array inputs{RaySceneRowInput{
        .geometry_kind = RayGeometryKind::aabbs,
        .scene_identity = "sphere", .geometry_identity = "sphere#0", .geometry_match_identity = "sphere",
        .geometry_allocation_id = 91, .procedural_asset = "sphere_asset",
        .procedural_record = nlohmann::json{{"geometry_to_world_0", {2.0, 3.0, 4.0, 5.0}}},
        .aabb_buffer_address = 0x1000}};
    const auto snapshot = resolveRaySceneRows(inputs, layout, nullptr);
    REQUIRE(snapshot.rows.size() == 1);
    REQUIRE(snapshot.rows[0].record_data.size() == 16);
    std::array<float, 4> values{};
    std::memcpy(values.data(), snapshot.rows[0].record_data.data(), sizeof(values));
    CHECK(values == std::array<float, 4>{2.0F, 3.0F, 4.0F, 5.0F});
    CHECK(makeRayGroupLayoutVirtualInclude(layout).find("PELICAN_RAY_HAS_PROCEDURAL_TRANSFORM") == std::string::npos);
}

TEST_CASE("WP386a layout v3 and binding v2 resolve procedural rows without a material join",
          "[WP386a][ray-tracing][rt3-2a][cpu]") {
    const auto layout = parseRayGroupLayout(v3Layout().dump(), "layout.json");
    REQUIRE(layout.group_types.size() == 2);
    CHECK(layout.group_types[1] == RayGroupType::procedural);
    REQUIRE(layout.acceleration_structure);
    CHECK(layout.acceleration_structure->procedural_defaults.geometry_flags ==
          static_cast<std::uint32_t>(vk::GeometryFlagBitsKHR::eOpaque));

    const auto bindings = parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":2,
      "bindings":[{"procedural":"sphere_asset","group":"sphere"}]
    })json", layout, "bindings.json");

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
              .assets == std::vector<std::string>{"sphere_asset"});
    CHECK_THROWS_WITH(codec->decodeAuthored(nlohmann::json{
        {"name", "pelican_procedural_geometry"},
        {"asset", "sphere_asset"},
        {"entries", nlohmann::json::array()}}),
        Catch::Matchers::ContainsSubstring("field 'pelican_procedural_geometry.entries' is unknown"));
}

} // namespace Pelican
