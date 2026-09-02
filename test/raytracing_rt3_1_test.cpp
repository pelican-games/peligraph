#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <components/predefined.hpp>

#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/framephase.hpp"
#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/editorruntimefactory.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpassconfigloader.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"
#include "../src/core/userpublic/details/ecs/componentdeclare.hpp"
#include "gltf_fragment_fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include "raytracing_rt3_1_fixture.hpp"

namespace Pelican {

struct Wp374ExternalRayInstanceComponent {};
DECLARE_COMPONENT(Wp374ExternalRayInstanceComponent, 63);

namespace {

using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::json;
using RayTracingRt31Fixture::accelerationDeclaration;
using RayTracingRt31Fixture::layoutRoot;
using RayTracingRt31Fixture::oakRule;
using RayTracingRt31Fixture::rowInput;
using RayTracingRt31Fixture::v2Layout;
using RayTracingRt31Fixture::rowByAuthor;
using RayTracingRt31Fixture::resolveProductionScene;

} // namespace

TEST_CASE("WP374 layout v2 owns AS parsing and separates SBT/AS digests",
          "[WP374][ray-tracing][rt3-1][layout][digest][mutation]") {
    auto root = layoutRoot();
    root["acceleration_structure"] = accelerationDeclaration();
    const auto base = parseRayGroupLayout(root.dump(), "base.json");
    REQUIRE(base.version == 2);
    REQUIRE(base.semantic_mode == RayLayoutSemanticMode::LayoutV2);
    REQUIRE(base.acceleration_structure);
    CHECK(base.acceleration_structure->defaults.mask == 127);
    CHECK(base.acceleration_structure->defaults.instance_flags ==
          rayInstanceTriangleFacingCullDisable);
    REQUIRE(base.acceleration_structure->rules.size() == 1);
    CHECK(base.acceleration_structure->rules.front().geometry_flags ==
          rayGeometryOpaque);

    auto changed = root;
    changed["acceleration_structure"]["rules"][0]["geometry_flags"] =
        Json::array({"no_duplicate_any_hit_invocation"});
    const auto reloaded =
        parseRayGroupLayout(changed.dump(), "changed.json");
    CHECK(reloaded.digest_sha256 == base.digest_sha256);
    CHECK(reloaded.acceleration_structure_digest_sha256 !=
          base.acceleration_structure_digest_sha256);

    SECTION("v1 AS declaration loses before any scene component check") {
        auto invalid = root;
        invalid["version"] = 1;
        CHECK_THROWS_WITH(
            parseRayGroupLayout(invalid.dump()),
            ContainsSubstring(
                "pelican.ray_as_declaration_requires_layout_v2@1"));
    }
    SECTION("malformed and duplicate selectors fail during layout parse") {
        auto malformed = root;
        malformed["acceleration_structure"]["rules"][0]["material"]
                 ["asset"] = "models/chairs.glb";
        CHECK_THROWS_WITH(
            parseRayGroupLayout(malformed.dump()),
            ContainsSubstring("pelican.ray_as_rule_malformed@1"));

        auto duplicate = root;
        duplicate["acceleration_structure"]["rules"].push_back(
            oakRule({}));
        CHECK_THROWS_WITH(
            parseRayGroupLayout(duplicate.dump()),
            ContainsSubstring("pelican.ray_as_duplicate_rule@1"));
    }
    SECTION("closed flag vocabulary and conflict are named") {
        auto unknown = root;
        unknown["acceleration_structure"]["defaults"]["geometry_flags"] =
            Json::array({"future_flag"});
        CHECK_THROWS_WITH(
            parseRayGroupLayout(unknown.dump()),
            ContainsSubstring("pelican.ray_as_unknown_flag@1"));

        auto conflict = root;
        conflict["acceleration_structure"]["defaults"]["instance_flags"] =
            Json::array({"force_opaque", "force_no_opaque"});
        CHECK_THROWS_WITH(
            parseRayGroupLayout(conflict.dump()),
            ContainsSubstring(
                "pelican.ray_as_conflicting_instance_flags@1"));
    }
}

TEST_CASE("WP374 production row resolver applies both two-level chains",
          "[WP374][ray-tracing][rt3-1][resolver][production][mutation]") {
    const auto layout = v2Layout();
    std::array inputs{
        rowInput("chair-b", "Pine", 0, 102),
        rowInput("chair-a", "Oak", 1, 101),
    };
    inputs[0].ray_instance = RayInstanceComponent{
        .mask = std::uint8_t{3},
        .instance_flags = std::uint32_t{rayInstanceTriangleFlipFacing},
        .custom_index = std::uint32_t{0x123456},
    };
    const auto rows = resolveRaySceneRows(inputs, layout, nullptr);
    REQUIRE(rows.semantic_mode == RayLayoutSemanticMode::LayoutV2);
    REQUIRE(rows.rows.size() == 2);

    const auto &oak = rowByAuthor(rows, 101);
    CHECK(oak.geometry_flags == rayGeometryOpaque);
    CHECK(oak.instance_mask == 127);
    CHECK(oak.instance_flags == rayInstanceTriangleFacingCullDisable);
    CHECK(oak.instance_custom_index == 0);

    const auto &pine = rowByAuthor(rows, 102);
    CHECK(pine.geometry_flags == 0);
    CHECK(pine.instance_mask == 3);
    CHECK(pine.instance_flags == rayInstanceTriangleFlipFacing);
    CHECK(pine.instance_custom_index == 0x123456);

    std::array<RayQueryGeometryInstanceSnapshot, 2> geometry{};
    geometry[0].geometry_allocation_id = 41;
    geometry[1].geometry_allocation_id = 41;
    const std::array<std::size_t, 2> static_indices{0, 1};
    const auto plan = lowerRayTracingTlasInstancePlan(
        geometry, static_indices, &rows, 2);
    REQUIRE(plan.size() == 2);
    const auto planned_pine = std::ranges::find(
        plan, pine.source_index,
        &RayTracingTlasInstancePlanEntry::source_index);
    REQUIRE(planned_pine != plan.end());
    CHECK(planned_pine->instance_mask == 3);
    CHECK(planned_pine->instance_flags == rayInstanceTriangleFlipFacing);
    CHECK(planned_pine->instance_custom_index == 0x123456);
    CHECK(planned_pine->geometry_flags == 0);

    SECTION("custom index is stable when a new row changes q") {
        auto expanded = std::vector<RaySceneRowInput>{inputs.begin(),
                                                     inputs.end()};
        expanded.push_back(rowInput("chair-0", "Pine", 2, 103));
        const auto shifted = resolveRaySceneRows(expanded, layout, nullptr);
        const auto &shifted_pine = rowByAuthor(shifted, 102);
        CHECK(shifted_pine.q != pine.q);
        CHECK(shifted_pine.instance_custom_index == 0x123456);
    }
    SECTION("overlapping document and asset rules are ambiguous per row") {
        auto declaration = accelerationDeclaration();
        declaration["rules"].push_back({
            {"material",
             {{"asset", "models/chairs.glb"},
              {"source_material_index", 1}}},
            {"geometry_flags", Json::array()},
        });
        const auto ambiguous = v2Layout(std::move(declaration));
        CHECK_THROWS_WITH(
            resolveRaySceneRows(inputs, ambiguous, nullptr),
            ContainsSubstring("pelican.ray_as_rule_ambiguous@1"));
    }
    SECTION("rules reject missing or partial row provenance") {
        auto unavailable = inputs;
        unavailable[0].material_document.clear();
        unavailable[0].material_asset.clear();
        CHECK_THROWS_WITH(
            resolveRaySceneRows(unavailable, layout, nullptr),
            ContainsSubstring(
                "pelican.ray_as_row_material_identity_unavailable@1"));
    }
}

TEST_CASE("WP374 semantic modes preserve legacy and v1 custom-index ABIs",
          "[WP374][ray-tracing][rt3-1][semantic-mode][legacy][mutation]") {
    auto v1_root = layoutRoot(1);
    const auto v1 = parseRayGroupLayout(v1_root.dump());
    std::array inputs{
        rowInput("z", "Oak", 0, 1),
        rowInput("a", "Pine", 1, 2),
    };
    const auto rows = resolveRaySceneRows(inputs, v1, nullptr);
    REQUIRE(rows.semantic_mode == RayLayoutSemanticMode::LayoutV1Q);
    for (const auto &row : rows.rows) {
        CHECK(row.instance_custom_index == row.q);
    }
    inputs[0].ray_instance = RayInstanceComponent{.mask = 1};
    CHECK_THROWS_WITH(
        resolveRaySceneRows(inputs, v1, nullptr),
        ContainsSubstring("pelican.ray_instance_requires_layout_v2@1"));

    std::array<RayQueryGeometryInstanceSnapshot, 2> geometry{};
    const std::array<std::size_t, 2> static_indices{1, 0};
    const auto legacy = lowerRayTracingTlasInstancePlan(
        geometry, static_indices, nullptr, 0);
    REQUIRE(legacy.size() == 2);
    CHECK(legacy[0].source_index == 1);
    CHECK(legacy[0].instance_custom_index == 0);
    CHECK(legacy[1].instance_custom_index == 1);
    CHECK(legacy[0].instance_mask == 0xffu);
    CHECK(legacy[0].instance_flags == 0);
    CHECK(legacy[0].geometry_flags == 0);
}

TEST_CASE("WP374 ray_instance codec is strict and version gated",
          "[WP374][ray-tracing][rt3-1][component-codec][version-gate][matrix]") {
    FastModuleContainer modules;
    auto &mode = GET_MODULE(RayLayoutRuntimeState);
    mode.configure(RayLayoutSemanticMode::LayoutV2);
    REQUIRE(componentCodecs().size() == 8);
    const auto &codec = requireComponentCodec("ray_instance");
    const auto decoded = codec.decodeAuthored({
        {"name", "ray_instance"},
        {"mask", 9},
        {"instance_flags", {"force_no_opaque"}},
        {"custom_index", 0x654321},
    });
    const auto canonical = codec.encodeCanonical(decoded);
    CHECK(canonical["mask"] == 9);
    CHECK(canonical["instance_flags"] ==
          Json::array({"force_no_opaque"}));
    CHECK(canonical["custom_index"] == 0x654321);

    CHECK_THROWS_WITH(
        codec.decodeAuthored({{"name", "ray_instance"}}),
        ContainsSubstring("pelican.ray_instance_empty@1"));
    CHECK_THROWS_WITH(
        codec.decodeAuthored(
            {{"name", "ray_instance"}, {"future", 1}}),
        ContainsSubstring("pelican.ray_instance_unknown_field@1"));
    CHECK_THROWS_WITH(
        codec.decodeAuthored(
            {{"name", "ray_instance"}, {"mask", "all"}}),
        ContainsSubstring("pelican.ray_instance_field_type@1"));
    CHECK_THROWS_WITH(
        codec.decodeAuthored(
            {{"name", "ray_instance"}, {"mask", 256}}),
        ContainsSubstring("pelican.ray_instance_field_range@1"));
    CHECK_THROWS_WITH(
        codec.decodeAuthored({{"name", "ray_instance"},
                              {"custom_index", 0x1000000}}),
        ContainsSubstring("pelican.ray_instance_field_range@1"));
    CHECK_THROWS_WITH(
        mode.configure(RayLayoutSemanticMode::LayoutV1Q),
        ContainsSubstring(
            "pelican.ray_layout_mode_change_requires_restart@1"));

    STATIC_CHECK(PELICAN_RUNTIME_SHADER_COMPILER == 0 ||
                 PELICAN_RUNTIME_SHADER_COMPILER == 1);
    STATIC_CHECK(PELICAN_WITH_OPENXR == 0 || PELICAN_WITH_OPENXR == 1);
    STATIC_CHECK(PELICAN_WITH_IMGUI == 0 || PELICAN_WITH_IMGUI == 1);
    STATIC_CHECK(PELICAN_WP374_SKIP_DEVSTUDIO == 0 ||
                 PELICAN_WP374_SKIP_DEVSTUDIO == 1);
}

TEST_CASE("WP374 version gate preserves an external no-layout owner",
          "[WP374][ray-tracing][rt3-1][namespace][external-owner][mutation]") {
    SECTION("no-layout keeps the pre-existing external component") {
        FastModuleContainer modules;
        GET_MODULE(RayLayoutRuntimeState)
            .configure(RayLayoutSemanticMode::LegacyNoLayout);
        (void)internal::getComponentRegisterer()
            .registerComponent<Wp374ExternalRayInstanceComponent>(
                "ray_instance");
        CHECK(findComponentCodec("ray_instance") == nullptr);
        ResolvedScene resolved;
        const auto &scene = resolveProductionScene(true, resolved);
        CHECK_NOTHROW(internal::validateResolvedSceneFeatureBindings(scene));
    }
    SECTION("v2 reverse registration cannot steal the external name") {
        FastModuleContainer modules;
        GET_MODULE(RayLayoutRuntimeState)
            .configure(RayLayoutSemanticMode::LayoutV2);
        (void)internal::getComponentRegisterer()
            .registerComponent<Wp374ExternalRayInstanceComponent>(
                "ray_instance");
        CHECK_THROWS(internal::getComponentRegisterer()
                         .registerComponent<RayInstanceComponent>(
                             "ray_instance"));
    }
}

TEST_CASE("WP374 non-ray-traced declarations fail before static filtering",
          "[WP374][ray-tracing][rt3-1][classification][production][mutation]") {
    RayTracingProjectConfig project{.layout = v2Layout(std::nullopt)};
    auto instance = RayQueryGeometryInstanceSnapshot{
        .asset_identity = 1,
        .authoring_object_id = 77,
        .ray_instance = RayInstanceComponent{.mask = 1},
        .material_asset_reference = "models/target.glb",
        .material_name = "material",
        .resolved_material_handle = 1,
        .resolved_material_generation = 1,
        .resolved_material_lease = std::make_shared<const int>(1),
        .geometry_allocation_id = 10,
        .index_count = 3,
        .vertex_count = 3,
    };
    SECTION("skinned") {
        instance.skinned = true;
        CHECK_THROWS_WITH(resolveRayTracingSceneRows(
                              std::span{&instance, std::size_t{1}}, project),
                          ContainsSubstring(
                              "pelican.ray_instance_not_ray_traced@1"));
    }
    SECTION("morph") {
        instance.morph_deformed = true;
        CHECK_THROWS_WITH(resolveRayTracingSceneRows(
                              std::span{&instance, std::size_t{1}}, project),
                          ContainsSubstring(
                              "pelican.ray_instance_not_ray_traced@1"));
    }
    SECTION("VAT") {
        instance.vat_deformed = true;
        CHECK_THROWS_WITH(resolveRayTracingSceneRows(
                              std::span{&instance, std::size_t{1}}, project),
                          ContainsSubstring(
                              "pelican.ray_instance_not_ray_traced@1"));
    }
    SECTION("BLAS-ineligible") {
        instance.index_count = 0;
        CHECK_THROWS_WITH(resolveRayTracingSceneRows(
                              std::span{&instance, std::size_t{1}}, project),
                          ContainsSubstring(
                              "pelican.ray_instance_not_ray_traced@1"));
    }
    SECTION("mixed static and excluded applies only to static") {
        auto excluded = instance;
        excluded.skinned = true;
        std::array mixed{instance, excluded};
        const auto rows = resolveRayTracingSceneRows(mixed, project);
        REQUIRE(rows.rows.size() == 1);
        CHECK(rows.rows.front().source_index == 0);
        CHECK(rows.rows.front().instance_mask == 1);
    }
}

TEST_CASE("WP374 compiler-off error wins before layout or scene loading",
          "[WP374][ray-tracing][rt3-1][compiler-off][matrix]") {
    Json root{{"ray_group_layout", "layout.json"}};
    bool loader_called = false;
    CHECK_THROWS_WITH(
        loadRayTracingProjectConfig(
            root,
            [&](std::string_view) {
                loader_called = true;
                return layoutRoot().dump();
            },
            false),
        ContainsSubstring(
            "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
    CHECK_FALSE(loader_called);
}

} // namespace Pelican
