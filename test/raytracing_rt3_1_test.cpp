#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/generators/catch_generators.hpp>
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

namespace {

using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::json;
using RayTracingRt31Fixture::accelerationDeclaration;
using RayTracingRt31Fixture::layoutRoot;
using RayTracingRt31Fixture::oakRule;
using RayTracingRt31Fixture::rowInput;
using RayTracingRt31Fixture::v3Layout;
using RayTracingRt31Fixture::rowByAuthor;
using RayTracingRt31Fixture::resolveProductionScene;

ComponentCodecValue decodeRayCodecOrThrow(
    const ComponentCodec &codec, const nlohmann::json &authored) {
    auto decoded = codec.decodeAuthored(authored);
    if (!decoded) {
        throw ResolutionFailureException{
            StoredResolutionFailure{std::move(decoded).error()}};
    }
    return std::move(decoded).value();
}

} // namespace

TEST_CASE("WP374 layout v2 owns AS parsing and separates SBT/AS digests",
          "[WP374][ray-tracing][rt3-1][layout][digest][mutation]") {
    auto root = layoutRoot();
    root["acceleration_structure"] = accelerationDeclaration();
    const auto base = parseRayGroupLayout(root.dump(), "base.json");
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

    SECTION("a v1 AS declaration is refused as a version before its AS is read") {
        auto invalid = root;
        invalid["version"] = 1;
        CHECK_THROWS_WITH(
            parseRayGroupLayout(invalid.dump()),
            ContainsSubstring(
                "pelican.ray_group_layout.version_unsupported@1"));
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
    const auto layout = v3Layout();
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
    REQUIRE(rows.rows.size() == 2);

    const auto &oak = rowByAuthor(rows, 101);
    CHECK(oak.geometry_flags == rayGeometryOpaque);
    CHECK(oak.instance_mask == 127);
    CHECK(oak.instance_flags == rayInstanceTriangleFacingCullDisable);
    CHECK(oak.instance_custom_index == 0);
    CHECK((oak.setting_sources.mask == "layout_default"));
    CHECK((oak.setting_sources.geometry_flags == "geometry_rule"));
    CHECK(oak.setting_sources.geometry_rule_index.has_value());

    const auto &pine = rowByAuthor(rows, 102);
    CHECK(pine.geometry_flags == 0);
    CHECK(pine.instance_mask == 3);
    CHECK(pine.instance_flags == rayInstanceTriangleFlipFacing);
    CHECK(pine.instance_custom_index == 0x123456);
    CHECK((pine.setting_sources.mask == "component"));
    CHECK((pine.setting_sources.instance_flags == "component"));
    CHECK((pine.setting_sources.custom_index == "component"));

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
    SECTION("document identity takes precedence over a matching asset rule") {
        auto declaration = accelerationDeclaration();
        declaration["rules"].push_back({
            {"material",
             {{"asset", "models/chairs.glb"},
              {"source_material_index", 1}}},
            {"geometry_flags", Json::array()},
        });
        const auto canonical = v3Layout(std::move(declaration));
        const auto canonical_rows =
            resolveRaySceneRows(inputs, canonical, nullptr);
        CHECK(rowByAuthor(canonical_rows, 101).geometry_flags ==
              rayGeometryOpaque);
    }
    SECTION("rules reject rows with neither document nor asset provenance") {
        auto unavailable = inputs;
        unavailable[0].material_document.clear();
        unavailable[0].material_asset.clear();
        CHECK_THROWS_WITH(
            resolveRaySceneRows(unavailable, layout, nullptr),
            ContainsSubstring(
                "pelican.ray_as_row_material_identity_unavailable@1"));
    }
    SECTION("an unnamed document remains Document ahead of an asset") {
        auto document_identified = inputs;
        document_identified[0].material_name.clear();
        const auto document_rows =
            resolveRaySceneRows(document_identified, layout, nullptr);
        REQUIRE(document_rows.rows.size() == inputs.size());
        CHECK(rowByAuthor(document_rows, 102).geometry_flags == 0);
        CHECK(rowByAuthor(document_rows, 102).material_reference ==
              "document:materials/multi.json");
    }
    SECTION("complete asset provenance tolerates an unpaired embedded material name") {
        auto asset_identified = inputs;
        asset_identified[0].material_document.clear();
        const auto asset_rows =
            resolveRaySceneRows(asset_identified, layout, nullptr);
        REQUIRE(asset_rows.rows.size() == inputs.size());
        CHECK(rowByAuthor(asset_rows, 102).geometry_flags == 0);
    }
}

TEST_CASE("WP383 canonical material identity drives AS gate and lookup",
          "[WP383][ray-tracing][resolver][production][param][mutation]") {
    // Every one of the five input axes goes through the production resolver.
    // The binding identifies the canonical row kind, while the AS rule makes
    // the optional document material name observable by the gate.
    const bool document_present = GENERATE(false, true);
    const bool material_name_present = GENERATE(false, true);
    const bool asset_present = GENERATE(false, true);
    const bool index_present = GENERATE(false, true);
    const bool index_valid = GENERATE(false, true);
    CAPTURE(document_present, material_name_present, asset_present,
            index_present, index_valid);

    // Keep the selector's optional document name independent from the row's
    // name.  These are deliberately not generator-derived: a selector name
    // must reject both an absent and a different row name, while an unnamed
    // selector must still match a named row.  Otherwise removing the name
    // equality requirement is only caught by the WP374 resolver unit tests.
    {
        auto named_declaration = accelerationDeclaration();
        const auto named_layout = v3Layout(std::move(named_declaration));
        auto unnamed_row = rowInput("wp383-name-absent", "Oak", 1, 3831);
        unnamed_row.material_name.clear();
        const auto unnamed_rows =
            resolveRaySceneRows(std::array{unnamed_row}, named_layout, nullptr);
        REQUIRE(unnamed_rows.rows.size() == 1);
        CHECK(unnamed_rows.rows.front().geometry_flags == 0);

        auto mismatched_row = rowInput("wp383-name-mismatch", "Pine", 1, 3832);
        const auto mismatched_rows = resolveRaySceneRows(
            std::array{mismatched_row}, named_layout, nullptr);
        REQUIRE(mismatched_rows.rows.size() == 1);
        CHECK(mismatched_rows.rows.front().geometry_flags == 0);

        auto unnamed_declaration = accelerationDeclaration();
        unnamed_declaration["rules"][0]["material"] =
            Json{{"document", "materials/multi.json"}};
        const auto unnamed_layout = v3Layout(std::move(unnamed_declaration));
        auto named_row = rowInput("wp383-selector-unnamed", "Oak", 1, 3833);
        const auto named_rows =
            resolveRaySceneRows(std::array{named_row}, unnamed_layout, nullptr);
        REQUIRE(named_rows.rows.size() == 1);
        CHECK(named_rows.rows.front().geometry_flags == rayGeometryOpaque);
    }

    auto declaration = accelerationDeclaration();
    if (!material_name_present) {
        declaration["rules"][0]["material"] =
            Json{{"document", "materials/multi.json"}};
    }
    declaration["rules"].push_back({
        {"material",
         {{"asset", "models/chairs.glb"}, {"source_material_index", 1}}},
        {"geometry_flags", Json::array({"no_duplicate_any_hit_invocation"})},
    });
    auto root = layoutRoot();
    root["groups"] = Json::array({{{"name", "opaque"}, {"type", "triangles"}},
                                  {{"name", "document"}, {"type", "triangles"}},
                                  {{"name", "asset"}, {"type", "triangles"}}});
    root["acceleration_structure"] = std::move(declaration);
    const auto layout = parseRayGroupLayout(root.dump(), "wp383-layout.json");

    auto input = rowInput("wp383", "Oak", 1, 383);
    input.material_document =
        document_present ? "materials/multi.json" : "";
    input.material_name = material_name_present ? "Oak" : "";
    input.material_asset = asset_present ? "models/chairs.glb" : "";
    input.source_material_index =
        index_present
            ? std::optional<std::uint32_t>{
                  index_valid ? 1u : std::numeric_limits<std::uint32_t>::max()}
            : std::optional<std::uint32_t>{};

    const bool document_identity = document_present;
    const bool invalid_asset_index =
        !document_identity && asset_present && index_present && !index_valid;
    const bool asset_identity =
        !document_identity && asset_present && index_present && index_valid;
    if (invalid_asset_index) {
        CHECK_THROWS_WITH(
            resolveRaySceneRows(std::array{input}, layout, nullptr),
            ContainsSubstring("pelican.ray_material_index_out_of_range@1"));
    } else if (!document_identity && !asset_identity) {
        CHECK_THROWS_WITH(
            resolveRaySceneRows(std::array{input}, layout, nullptr),
            ContainsSubstring(
                "pelican.ray_as_row_material_identity_unavailable@1"));
    } else {
        Json material;
        if (document_identity) {
            material = {{"document", "materials/multi.json"}};
        } else {
            material = {{"asset", "models/chairs.glb"},
                        {"source_material_index", 1}};
        }
        const auto bindings = parseRayBindings(
            Json{{"schema", "pelican.ray_bindings"},
                 {"version", 2},
                 {"bindings", Json::array({{{"material", std::move(material)},
                                              {"group", document_identity
                                                            ? "document"
                                                            : "asset"},
                                              {"record", Json::object()}}})}}
                .dump(),
            layout, "wp383-bindings.json");
        const auto rows =
            resolveRaySceneRows(std::array{input}, layout, &bindings);
        REQUIRE(rows.rows.size() == 1);
        CHECK(rows.rows.front().geometry_flags ==
              (document_identity ? rayGeometryOpaque
                                 : rayGeometryNoDuplicateAnyHitInvocation));
        CHECK(rows.rows.front().group_index ==
              layout.groupIndex(document_identity ? "document" : "asset"));
        CHECK(rows.rows.front().material_reference ==
              (document_identity ? "document:materials/multi.json"
                                 : "asset:models/chairs.glb#1"));
    }
}

TEST_CASE("MG-2a ray group layout v1 and v2 and ray bindings v1 are refused by name",
          "[MG-2a][ray-tracing][rt3-1][version-unsupported][mutation]") {
    const auto layout_code = "pelican.ray_group_layout.version_unsupported@1";
    const auto bindings_code = "pelican.ray_bindings.version_unsupported@1";
    // The shape the old versions were written in: v1/v2 groups have no type.
    const auto old_layout = [](int version) {
        return layoutRoot(static_cast<std::uint32_t>(version)).dump();
    };
    SECTION("layout parse") {
        for (const int version : {1, 2}) {
            INFO("layout version " << version);
            CHECK_THROWS_WITH(parseRayGroupLayout(old_layout(version), "old.json"),
                              ContainsSubstring(layout_code) &&
                                  ContainsSubstring("'old.json'"));
            // The typed form does not make an old version readable.
            auto typed = layoutRoot();
            typed["version"] = version;
            CHECK_THROWS_WITH(parseRayGroupLayout(typed.dump()),
                              ContainsSubstring(layout_code));
        }
        CHECK_NOTHROW(parseRayGroupLayout(layoutRoot().dump()));
    }
    const auto layout = parseRayGroupLayout(layoutRoot().dump(), "layout.json");
    const auto bindings_of = [](int version, bool procedural) {
        Json entry = procedural
            ? Json{{"procedural", "sphere_asset"}, {"group", "opaque"}}
            : Json{{"material", {{"document", "materials/multi.json"}}},
                   {"group", "opaque"},
                   {"record", Json::object()}};
        return Json{{"schema", "pelican.ray_bindings"},
                    {"version", version},
                    {"bindings", Json::array({entry})}}
            .dump();
    };
    SECTION("bindings parse") {
        for (const bool procedural : {false, true}) {
            INFO("procedural selector " << procedural);
            CHECK_THROWS_WITH(
                parseRayBindings(bindings_of(1, procedural), layout, "old-bindings.json"),
                ContainsSubstring(bindings_code) &&
                    ContainsSubstring("'old-bindings.json'"));
        }
        CHECK_NOTHROW(parseRayBindings(bindings_of(2, false), layout));
    }
    SECTION("the project loader refuses either document by name") {
        const Json root{{"ray_group_layout", "layout.json"},
                        {"ray_bindings", "bindings.json"}};
        const auto load = [&](std::string layout_bytes,
                              std::string bindings_bytes) {
            return loadRayTracingProjectConfig(
                root,
                [&](std::string_view reference) {
                    return reference == "layout.json" ? layout_bytes
                                                      : bindings_bytes;
                },
                true);
        };
        CHECK_THROWS_WITH(load(old_layout(1), bindings_of(2, false)),
                          ContainsSubstring(layout_code));
        CHECK_THROWS_WITH(load(old_layout(2), bindings_of(2, false)),
                          ContainsSubstring(layout_code));
        CHECK_THROWS_WITH(load(layoutRoot().dump(), bindings_of(1, false)),
                          ContainsSubstring(bindings_code));
        const auto loaded = load(layoutRoot().dump(), bindings_of(2, false));
        REQUIRE(loaded);
        CHECK(rayLayoutSemanticMode(loaded.get()) ==
              RayLayoutSemanticMode::LayoutV3);
    }
    SECTION("the one version keeps none of v1's meaning") {
        // v1 digested its raw bytes; the layout digest is of the parsed
        // document, so spelling does not move it.
        const auto spaced = parseRayGroupLayout(layoutRoot().dump(2) + "\n");
        CHECK(spaced.digest_sha256 == layout.digest_sha256);
        // v1 wrote the row's q into the instance custom index; a row now
        // takes the layout's default like any other.
        std::array inputs{
            rowInput("z", "Oak", 0, 1),
            rowInput("a", "Pine", 1, 2),
        };
        const auto rows = resolveRaySceneRows(inputs, layout, nullptr);
        REQUIRE(rows.rows.size() == 2);
        for (const auto &row : rows.rows) {
            CHECK(row.instance_custom_index ==
                  rayAccelerationStructureCanonicalDefaults.custom_index);
            CHECK((row.setting_sources.custom_index == "engine_default"));
        }
        CHECK(rows.rows[1].q == 1);
    }
}

TEST_CASE("WP374 a project without a layout keeps the legacy custom-index ABI",
          "[WP374][ray-tracing][rt3-1][semantic-mode][legacy][mutation]") {
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

TEST_CASE("WP379 pelican_ray_instance codec is strict and version gated",
          "[WP374][ray-tracing][rt3-1][component-codec][version-gate][matrix]") {
    FastModuleContainer modules;
    auto &mode = GET_MODULE(RayLayoutRuntimeState);
    mode.configure(RayLayoutSemanticMode::LayoutV3);
    REQUIRE(componentCodecs().size() == 9);
    const auto &codec = requireComponentCodec("pelican_ray_instance");
    const auto decoded = decodeRayCodecOrThrow(codec, {
        {"name", "pelican_ray_instance"},
        {"mask", 9},
        {"instance_flags", {"force_no_opaque"}},
        {"custom_index", 0x654321},
    });
    const auto canonical = codec.encodeCanonical(decoded);
    CHECK(canonical["mask"] == 9);
    CHECK(canonical["instance_flags"] ==
          Json::array({"force_no_opaque"}));
    CHECK(canonical["custom_index"] == 0x654321);

    struct InvalidRow {
        std::string_view branch;
        Json authored;
        std::string_view code;
    };
    const std::array<InvalidRow, 11> invalid_corpus{{
        {"root type", Json::array(), "pelican.ray_instance_field_type@1"},
        {"name", Json{{"mask", 1}},
         "pelican.ray_instance_field_type@1"},
        {"unknown field",
         Json{{"name", "pelican_ray_instance"}, {"future", 1}},
         "pelican.ray_instance_unknown_field@1"},
        {"empty", Json{{"name", "pelican_ray_instance"}},
         "pelican.ray_instance_empty@1"},
        {"unsigned type",
         Json{{"name", "pelican_ray_instance"}, {"mask", "all"}},
         "pelican.ray_instance_field_type@1"},
        {"unsigned range",
         Json{{"name", "pelican_ray_instance"}, {"mask", 256}},
         "pelican.ray_instance_field_range@1"},
        {"flags container",
         Json{{"name", "pelican_ray_instance"},
              {"instance_flags", "force_opaque"}},
         "pelican.ray_instance_field_type@1"},
        {"flags entry",
         Json{{"name", "pelican_ray_instance"},
              {"instance_flags", Json::array({1})}},
         "pelican.ray_instance_field_type@1"},
        {"flags duplicate",
         Json{{"name", "pelican_ray_instance"},
              {"instance_flags",
               Json::array({"force_opaque", "force_opaque"})}},
         "pelican.ray_instance_field_type@1"},
        {"flags unknown",
         Json{{"name", "pelican_ray_instance"},
              {"instance_flags", Json::array({"future"})}},
         "pelican.ray_as_unknown_flag@1"},
        {"flags conflict",
         Json{{"name", "pelican_ray_instance"},
              {"instance_flags",
               Json::array({"force_opaque", "force_no_opaque"})}},
         "pelican.ray_as_conflicting_instance_flags@1"},
    }};
    for (const auto &row : invalid_corpus) {
        DYNAMIC_SECTION(row.branch) {
            REQUIRE_THROWS_WITH(
                decodeRayCodecOrThrow(codec, row.authored),
                ContainsSubstring(std::string{row.code}));
        }
    }
    CHECK_THROWS_WITH(
        decodeRayCodecOrThrow(codec, {{"name", "pelican_ray_instance"},
                                      {"custom_index", 0x1000000}}),
        ContainsSubstring("pelican.ray_instance_field_range@1"));
    CHECK_THROWS_WITH(
        mode.configure(RayLayoutSemanticMode::LegacyNoLayout),
        ContainsSubstring(
            "pelican.ray_layout_mode_change_requires_restart@1"));

    STATIC_CHECK(PELICAN_RUNTIME_SHADER_COMPILER == 0 ||
                 PELICAN_RUNTIME_SHADER_COMPILER == 1);
    STATIC_CHECK(PELICAN_WITH_OPENXR == 0 || PELICAN_WITH_OPENXR == 1);
    STATIC_CHECK(PELICAN_WITH_IMGUI == 0 || PELICAN_WITH_IMGUI == 1);
    STATIC_CHECK(PELICAN_WP374_SKIP_DEVSTUDIO == 0 ||
                 PELICAN_WP374_SKIP_DEVSTUDIO == 1);
}

TEST_CASE("WP382d occurrence rows preserve presence and resolve within their authoring object",
          "[WP382d][ray-tracing][rt3-1][resolver][component-codec]") {
    const auto *codec = testing::findComponentCodecIncludingDisabled("pelican_ray_instance");
    REQUIRE(codec != nullptr);
    Json authored = {
        {"name", "pelican_ray_instance"}, {"mask", 17},
        {"instance_flags", {"triangle_flip_facing"}}, {"custom_index", 93},
        {"overrides", {{"$list", {{"v", 1}, {"next_id", 4}}},
            {"rows", Json::array({
                {{"$row", {{"id", 1}, {"v", 1}}}, {"occurrence_id", "first"},
                 {"mask", 0}, {"instance_flags", Json::array()}, {"custom_index", 0}},
                {{"$row", {{"id", 2}, {"v", 1}}}, {"occurrence_id", "second"}},
                {{"$row", {{"id", 3}, {"v", 1}}}, {"occurrence_id", "removed"}}
            })}}}
    };
    const auto decoded = decodeRayCodecOrThrow(*codec, authored);
    CHECK(Json(codec->encodeCanonical(decoded)) == authored);
    const auto declaration = std::any_cast<RayInstanceComponent>(decoded);
    REQUIRE(declaration.overrides.has_value());
    REQUIRE(declaration.overrides->rows.size() == 3);
    CHECK(declaration.overrides->rows[0].value.mask == 0);
    CHECK_FALSE(declaration.overrides->rows[1].value.mask.has_value());
    auto duplicate = authored;
    duplicate["overrides"]["rows"][1]["occurrence_id"] = "first";
    const auto rejected = codec->decodeAuthored(duplicate);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "DuplicateOccurrenceOverride");
    CHECK(rejected.error().fields.at("first_row_id") == 1);
    CHECK(rejected.error().fields.at("second_row_id") == 2);

    RayTracingProjectConfig project{.layout = v3Layout(std::nullopt)};
    RayQueryGeometryInstanceSnapshot first{
        .asset_identity = 1, .authoring_object_id = 7,
        .ray_instance = declaration,
        .material_asset_reference = "model.glb", .material_name = "same",
        .resolved_material_handle = 1, .resolved_material_generation = 1,
        .resolved_material_lease = std::make_shared<const int>(1),
        .source_material_index = 0,
        .geometry_allocation_id = 1, .index_count = 3, .vertex_count = 3,
        .occurrence_id = "first",
    };
    auto second = first;
    second.primitive_index = 1;
    second.index_offset = 3;
    second.occurrence_id = "second";
    auto other = first;
    other.authoring_object_id = 8;
    other.ray_instance = RayInstanceComponent{.mask = 29};
    other.world_transform[3][0] = 4.0f;
    std::array instances{second, other, first};
    const auto resolved = resolveRayTracingSceneRows(instances, project);
    REQUIRE(resolved.rows.size() == 3);
    const auto by_source = [&](std::size_t source) -> const RaySceneRow & {
        const auto found = std::ranges::find(resolved.rows, source, &RaySceneRow::source_index);
        REQUIRE(found != resolved.rows.end());
        return *found;
    };
    CHECK(by_source(2).instance_mask == 0);
    CHECK(by_source(2).instance_flags == 0);
    CHECK(by_source(2).instance_custom_index == 0);
    CHECK(by_source(0).instance_mask == 17);
    CHECK(by_source(0).instance_flags == rayInstanceTriangleFlipFacing);
    CHECK(by_source(0).instance_custom_index == 93);
    CHECK(by_source(1).instance_mask == 29);
    CHECK((by_source(2).setting_sources.mask == "occurrence_override"));
    CHECK((by_source(2).setting_sources.instance_flags == "occurrence_override"));
    CHECK((by_source(2).setting_sources.custom_index == "occurrence_override"));
    CHECK(by_source(2).setting_sources.override_row_id == 1);
    CHECK((by_source(0).setting_sources.mask == "component"));
    CHECK((by_source(1).setting_sources.mask == "component"));
    CHECK_FALSE(by_source(1).setting_sources.override_row_id.has_value());
    REQUIRE(resolved.occurrence_overrides.size() == 3);
    CHECK(resolved.occurrence_overrides[0].q == by_source(2).q);
    CHECK(resolved.occurrence_overrides[2].error == "OccurrenceAbsent");
    CHECK_FALSE(resolved.occurrence_overrides[2].q.has_value());
    for (auto &instance : instances) {
        if (instance.authoring_object_id == 7)
            instance.ray_instance->overrides->rows.erase(instance.ray_instance->overrides->rows.begin());
    }
    const auto deleted = resolveRayTracingSceneRows(instances, project);
    const auto restored = std::ranges::find(deleted.rows, std::size_t{2}, &RaySceneRow::source_index);
    REQUIRE(restored != deleted.rows.end());
    CHECK(restored->instance_mask == 17);
    CHECK(restored->instance_flags == rayInstanceTriangleFlipFacing);
    CHECK(restored->instance_custom_index == 93);
    CHECK((restored->setting_sources.mask == "component"));
    CHECK_FALSE(restored->setting_sources.override_row_id.has_value());

    SECTION("authored override diagnostics survive loss of every geometry") {
        const nlohmann::ordered_json witness = {
            {"type", "pelican_ray_instance"}, {"occurrence", 1},
            {"origin", {{"kind", "authored"}, {"authoring_component_index", 1}}}};
        const RaySceneDeclarationCatalog declarations{{7, {declaration, witness}}};
        auto geometry_only = first;
        geometry_only.ray_instance.reset();
        const auto present = resolveRayTracingSceneRows(std::span{&geometry_only, 1}, project, declarations);
        REQUIRE(present.rows.size() == 1);
        CHECK(present.rows[0].instance_mask == 0);
        CHECK(present.rows[0].instance_flags == 0);
        CHECK(present.rows[0].instance_custom_index == 0);
        CHECK(present.occurrence_overrides[0].error.empty());
        const auto unresolved = resolveRayTracingSceneRows({}, project, declarations);
        CHECK(unresolved.rows.empty());
        REQUIRE(unresolved.occurrence_overrides.size() == 3);
        for (const auto &row : unresolved.occurrence_overrides) {
            CHECK(row.error == "OccurrenceAbsent");
            CHECK_FALSE(row.q.has_value());
            CHECK(row.ray_instance_component_witness == witness);
        }
        const auto diagnostics = rayScenePlanDiagnostics(unresolved, 7);
        REQUIRE(diagnostics.rejected_rows.size() == 3);
        CHECK(diagnostics.rejected_rows[0].address.row_id == 1);
        CHECK(diagnostics.rejected_rows[0].address.component_witness == witness);
        // A different object with the old occurrence does not rescue the row.
        const auto unrelated = resolveRayTracingSceneRows(std::span{&other, 1}, project, declarations);
        REQUIRE(unrelated.occurrence_overrides.size() == 3);
        CHECK(unrelated.occurrence_overrides[0].error == "OccurrenceAbsent");
    }
    SECTION("row-only declarations retain ineligible targets as diagnostics") {
        auto excluded = first;
        excluded.ray_instance->mask.reset();
        excluded.ray_instance->instance_flags.reset();
        excluded.ray_instance->custom_index.reset();
        excluded.index_count = 0;
        const auto unresolved = resolveRayTracingSceneRows(std::span{&excluded, 1}, project);
        CHECK(unresolved.rows.empty());
        REQUIRE(unresolved.occurrence_overrides.size() == 3);
        CHECK(unresolved.occurrence_overrides[0].error == "OccurrenceNotRayTraced");
    }
    SECTION("ambiguous occurrence does not arbitrarily select a primitive") {
        auto ambiguous = first;
        ambiguous.occurrence_ambiguous = true;
        const auto unresolved = resolveRayTracingSceneRows(std::span{&ambiguous, 1}, project);
        REQUIRE(unresolved.rows.size() == 1);
        CHECK(unresolved.rows[0].instance_mask == 17);
        CHECK(unresolved.occurrence_overrides[0].error == "OccurrenceAmbiguous");
    }
}

TEST_CASE("RT5 declarations support deformation and reject ineligible geometry",
          "[WP374][ray-tracing][rt3-1][classification][production][mutation]") {
    RayTracingProjectConfig project{.layout = v3Layout(std::nullopt)};
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
        const auto rows = resolveRayTracingSceneRows(
            std::span{&instance, std::size_t{1}}, project);
        REQUIRE(rows.rows.size() == 1);
        CHECK(rows.rows.front().instance_mask == 1);
        CHECK_THROWS_WITH(makeRayTracingGeometrySnapshot(
            std::span{&instance, std::size_t{1}}, 0x1000, 0x2000),
            ContainsSubstring("deformed geometry must be recorded"));
        instance.deformed_vertex_address = 0x4000;
        const auto geometry = makeRayTracingGeometrySnapshot(
            std::span{&instance, std::size_t{1}}, 0x1000, 0x2000);
        REQUIRE(geometry.rows.size() == 1);
        CHECK(geometry.rows.front().gpu.vertex_address == 0x4000);
    }
    SECTION("morph") {
        instance.morph_deformed = true;
        const auto rows = resolveRayTracingSceneRows(
            std::span{&instance, std::size_t{1}}, project);
        REQUIRE(rows.rows.size() == 1);
        CHECK(rows.rows.front().instance_mask == 1);
        CHECK_THROWS_WITH(makeRayTracingGeometrySnapshot(
            std::span{&instance, std::size_t{1}}, 0x1000, 0x2000),
            ContainsSubstring("deformed geometry must be recorded"));
        instance.deformed_vertex_address = 0x4000;
        const auto geometry = makeRayTracingGeometrySnapshot(
            std::span{&instance, std::size_t{1}}, 0x1000, 0x2000);
        REQUIRE(geometry.rows.size() == 1);
        CHECK(geometry.rows.front().gpu.vertex_address == 0x4000);
    }
    SECTION("VAT") {
        instance.vat_deformed = true;
        const auto rows = resolveRayTracingSceneRows(
            std::span{&instance, std::size_t{1}}, project);
        REQUIRE(rows.rows.size() == 1);
        CHECK(rows.rows.front().instance_mask == 1);
        CHECK_THROWS_WITH(makeRayTracingGeometrySnapshot(
            std::span{&instance, std::size_t{1}}, 0x1000, 0x2000),
            ContainsSubstring("deformed geometry must be recorded"));
        instance.deformed_vertex_address = 0x4000;
        const auto geometry = makeRayTracingGeometrySnapshot(
            std::span{&instance, std::size_t{1}}, 0x1000, 0x2000);
        REQUIRE(geometry.rows.size() == 1);
        CHECK(geometry.rows.front().gpu.vertex_address == 0x4000);
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
        excluded.index_count = 0;
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
