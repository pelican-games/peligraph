#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/shader/raytracingvalidation.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderreflection.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/project/vulkanviewplanning.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace Pelican {
namespace {

using Catch::Matchers::ContainsSubstring;

std::uint32_t readWp368U32(std::span<const std::byte> bytes) {
    REQUIRE(bytes.size() >= 4);
    return std::to_integer<std::uint32_t>(bytes[0]) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 8u) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 16u) |
           (std::to_integer<std::uint32_t>(bytes[3]) << 24u);
}

RayGroupLayout wp368Layout(nlohmann::json engine) {
    nlohmann::json root{
        {"schema", "pelican.ray_group_layout"},
        {"version", 1},
        {"ray_types", {"primary"}},
        {"default_group", "opaque"},
        {"groups", {{{"name", "opaque"}}}},
        {"misses", {{{"name", "sky"}}}},
        {"record", {{{"name", "tint"}, {"type", "vec4"}}}},
    };
    if (!engine.is_null()) root["engine_record"] = std::move(engine);
    return parseRayGroupLayout(root.dump(), "wp368-layout.json");
}

std::shared_ptr<const void> wp368Lease(int value) {
    return std::static_pointer_cast<const void>(
        std::make_shared<const int>(value));
}

struct Wp368CompilerFixture {
    nlohmann::json config = nlohmann::json::parse(R"json({
      "features": [],
      "render_targets": [],
      "rendering_passes": [{
        "name": "main",
        "passes": [{
          "name": "scene",
          "type": "material",
          "output": {"color": ["swapchain"], "depth": null}
        }]
      }]
    })json");
    PathResolver path_resolver;
    GraphTransformRegistrySnapshot graph_transforms;
    RenderStrategyRegistrySnapshot render_strategies;
    SubgraphReplacementRegistrySnapshot subgraph_replacements;
    VulkanRenderCompilerBackendContext backend{
        vk::Format::eB8G8R8A8Srgb,
        vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    std::array<RenderCompilerProgramVariantRequest, 3> variants{
        RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::flat,
            .artifact = RenderCompilerProgramArtifact::data_only,
        },
        RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::xr,
            .artifact = RenderCompilerProgramArtifact::data_only,
        },
        RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::preview,
            .artifact = RenderCompilerProgramArtifact::data_only,
        }};

    RenderCompilerProgramInput input(
        std::shared_ptr<const RayTracingProjectConfig> project) const {
        return {
            .rendering_config = config,
            .source_name = "WP368 inactive resource contrast",
            .path_resolver = path_resolver,
            .runtime_shader_compiler_enabled = true,
            .ray_tracing_project = std::move(project),
            .graph_transforms = graph_transforms,
            .render_strategies = render_strategies,
            .subgraph_replacements = subgraph_replacements,
            .backend_context = backend,
            .variants = variants,
        };
    }
};

std::size_t wp368BindlessCapabilityCount(
    const CompiledRenderPipeline &pipeline) {
    std::size_t result = 0;
    for (const auto &graph : pipeline.target_planning.graphs) {
        result += static_cast<std::size_t>(std::ranges::count(
            graph.required_capabilities,
            std::string{vulkanRayMaterialBindlessCapability}));
    }
    return result;
}

} // namespace

TEST_CASE(
    "WP368 physical record is one canonical std430 struct for zero one and two engine fields",
    "[WP368][ray-tracing][rt2b][record][abi][production][mutation]") {
    const auto zero = wp368Layout(nullptr);
    REQUIRE(zero.engine_record.empty());
    REQUIRE(zero.physical_record.size() == 1);
    CHECK(zero.physical_record[0].name == "tint");
    CHECK(zero.physical_record[0].offset == 0);
    CHECK(zero.physical_record[0].size == 16);
    CHECK(zero.data_size == 16);
    const auto zero_json = canonicalRayRecordAbiJson(zero);
    CHECK(zero_json == nlohmann::json{
        {"schema", "pelican.ray_record_abi"},
        {"version", 1},
        {"data_size", 16},
        {"members", nlohmann::json::array({
            {{"name", "tint"}, {"type", "vec4"},
             {"offset", 0}, {"size", 16}, {"alignment", 16}},
        })},
    });

    const auto one = wp368Layout(
        nlohmann::json::array({"material_index"}));
    REQUIRE(one.physical_record.size() == 2);
    CHECK(one.physical_record[0] == RayRecordMemberLayout{
          "material_index", RayRecordType::u32, 0, 4, 4});
    CHECK(one.physical_record[1] == RayRecordMemberLayout{
          "tint", RayRecordType::vec4, 16, 16, 16});
    CHECK(one.data_size == 32);
    CHECK(canonicalRayRecordAbiJson(one) == nlohmann::json{
        {"schema", "pelican.ray_record_abi"},
        {"version", 1},
        {"data_size", 32},
        {"members", nlohmann::json::array({
            {{"name", "material_index"}, {"type", "u32"},
             {"offset", 0}, {"size", 4}, {"alignment", 4}},
            {{"name", "tint"}, {"type", "vec4"},
             {"offset", 16}, {"size", 16}, {"alignment", 16}},
        })},
        {"engine_record", {"material_index"}},
        {"stride", 32},
    });

    // Declaration order is deliberately reversed. Physical order is fixed.
    const auto two = wp368Layout(nlohmann::json::array(
        {"geometry_index", "material_index"}));
    REQUIRE(two.physical_record.size() == 3);
    CHECK(two.physical_record[0] == RayRecordMemberLayout{
          "material_index", RayRecordType::u32, 0, 4, 4});
    CHECK(two.physical_record[1] == RayRecordMemberLayout{
          "geometry_index", RayRecordType::u32, 4, 4, 4});
    CHECK(two.physical_record[2] == RayRecordMemberLayout{
          "tint", RayRecordType::vec4, 16, 16, 16});
    CHECK(two.data_size == 32);
    const auto two_json = canonicalRayRecordAbiJson(two);
    CHECK(two_json == nlohmann::json{
        {"schema", "pelican.ray_record_abi"},
        {"version", 1},
        {"data_size", 32},
        {"members", nlohmann::json::array({
            {{"name", "material_index"}, {"type", "u32"},
             {"offset", 0}, {"size", 4}, {"alignment", 4}},
            {{"name", "geometry_index"}, {"type", "u32"},
             {"offset", 4}, {"size", 4}, {"alignment", 4}},
            {{"name", "tint"}, {"type", "vec4"},
             {"offset", 16}, {"size", 16}, {"alignment", 16}},
        })},
        {"engine_record", {"material_index", "geometry_index"}},
        {"stride", 32},
    });

    const auto bindings = parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":1,"bindings":[{
        "material":{"document":"materials/mat.json"},
        "group":"opaque","record":{"tint":[0.25,0.5,0.75,1.0]}
      }]})json", two, "wp368-bindings.json");
    REQUIRE(bindings.bindings.size() == 1);
    const auto &authored = bindings.bindings.front().record_data;
    REQUIRE(authored.size() == 32);
    CHECK(readWp368U32(std::span{authored}.subspan(0, 4)) == 0);
    CHECK(readWp368U32(std::span{authored}.subspan(4, 4)) == 0);
    CHECK(readWp368U32(std::span{authored}.subspan(16, 4)) ==
          std::bit_cast<std::uint32_t>(0.25F));

    RaySceneRow row{
        .q = 0,
        .group_index = 0,
        .record_data = authored,
        .material_index = 7,
        .geometry_index = 11,
    };
    const std::array handles{
        std::byte{0xa0}, std::byte{0}, std::byte{0}, std::byte{0}};
    const auto pack = [&](const RaySceneRow &value) {
        return packRayTracingHitTable(
            two, {.shader_group_handle_size = 4,
                  .shader_group_handle_alignment = 4,
                  .max_shader_group_stride = 64,
                  .group_handles = handles,
                  .rows = std::span<const RaySceneRow>{&value, 1}});
    };
    const auto packed = pack(row);
    REQUIRE(packed.data_start == 4);
    CHECK(packed.record_byte_stride == 36);
    const auto physical = std::span{packed.bytes}.subspan(
        packed.data_start, two.data_size);
    CHECK(readWp368U32(physical.subspan(0, 4)) == 7);
    CHECK(readWp368U32(physical.subspan(4, 4)) == 11);
    CHECK(readWp368U32(physical.subspan(16, 4)) ==
          std::bit_cast<std::uint32_t>(0.25F));

    auto shifted = row;
    shifted.material_index = 8;
    const auto shifted_pack = pack(shifted);
    CHECK((shifted_pack.bytes != packed.bytes));
    CHECK(readWp368U32(std::span{shifted_pack.bytes}.subspan(
              shifted_pack.data_start, 4)) == 8);
    auto unfinalized = row;
    unfinalized.geometry_index = rayMissingTextureIndex;
    CHECK_THROWS_WITH(
        pack(unfinalized),
        ContainsSubstring("geometry_index was not finalized"));

    SECTION("unknown duplicate and authored reserved names are named") {
        auto root = nlohmann::json::parse(R"json({
          "schema":"pelican.ray_group_layout","version":1,
          "ray_types":["primary"],"default_group":"opaque",
          "groups":[{"name":"opaque"}],
          "misses":[{"name":"sky"}],"record":[]
        })json");
        root["engine_record"] = {"texture_index"};
        CHECK_THROWS_WITH(parseRayGroupLayout(root.dump()),
                          ContainsSubstring(
                              "pelican.ray_engine_record_unknown@1"));
        root["engine_record"] = {"material_index", "material_index"};
        CHECK_THROWS_WITH(parseRayGroupLayout(root.dump()),
                          ContainsSubstring(
                              "pelican.ray_engine_record_duplicate@1"));
        root["engine_record"] = {"material_index"};
        root["record"] = {{{"name", "material_index"},
                            {"type", "u32"}}};
        CHECK_THROWS_WITH(parseRayGroupLayout(root.dump()),
                          ContainsSubstring(
                              "pelican.ray_record_name_reserved@1"));
    }
}

TEST_CASE(
    "WP368 material and geometry tables canonically finalize one immutable row snapshot",
    "[WP368][ray-tracing][rt2b][tables][snapshot][digest][production][mutation]") {
    const auto asset_lease = wp368Lease(1);
    const auto document_lease = wp368Lease(2);
    const RaySceneRowSnapshot immutable_rows{
        .identity_digest_sha256 = "pre-table-row-digest",
        .rows = {
            RaySceneRow{
                .q = 0,
                .material_reference = "document:materials/mat.json#Leaf",
                .resolved_material_handle = 20,
                .resolved_material_generation = 200,
                .resolved_material_lease = document_lease,
                .geometry_allocation_id = 9,
            },
            RaySceneRow{
                .q = 1,
                .material_reference = "asset:models/tree.glb#7",
                .resolved_material_handle = 10,
                .resolved_material_generation = 100,
                .resolved_material_lease = asset_lease,
                .geometry_allocation_id = 4,
            },
        },
    };
    const std::array materials{
        RayMaterialTableSource{
            .material_handle = 20,
            .material_generation = 200,
            .material_lease = document_lease,
            .canonical_key = "document:materials/mat.json#Leaf",
            .base_color_factor = {0.25F, 0.5F, 0.75F, 1.0F},
            .alpha_cutoff = 0.625F,
            .alpha_mode = 1,
        },
        RayMaterialTableSource{
            .material_handle = 10,
            .material_generation = 100,
            .material_lease = asset_lease,
            .canonical_key = "asset:models/tree.glb#7",
            .base_color_factor = {1.0F, 0.0F, 0.0F, 1.0F},
            .alpha_cutoff = 0.5F,
            .alpha_mode = 0,
        },
    };
    const RayTracingGeometrySnapshot geometry{
        .digest_sha256 = "immutable-geometry-input",
        .rows = {
            RayTracingGeometrySnapshotRow{
                .geometry_allocation_id = 4,
                .index_count = 3,
                .index_offset = 6,
                .vertex_offset = 3,
                .vertex_count = 3,
                .gpu = RayGeometryTableRow{
                    .vertex_address = 0x20c0,
                    .index_address = 0x1018,
                    .vertex_stride = 64,
                    .index_type = 0,
                    .attribute_presence = rayGeometryAttributeUv0,
                },
            },
            RayTracingGeometrySnapshotRow{
                .geometry_allocation_id = 9,
                .index_count = 3,
                .index_offset = 0,
                .vertex_offset = 0,
                .vertex_count = 3,
                .gpu = RayGeometryTableRow{
                    .vertex_address = 0x2000,
                    .index_address = 0x1000,
                    .vertex_stride = 64,
                    .index_type = 0,
                    .attribute_presence =
                        rayGeometryAttributeNormal |
                        rayGeometryAttributeColor0,
                },
            },
        },
    };

    const auto first = buildRayTracingTableSnapshot(
        immutable_rows, materials, geometry);
    const auto repeated = buildRayTracingTableSnapshot(
        immutable_rows, materials, geometry);
    CHECK(immutable_rows.identity_digest_sha256 ==
          "pre-table-row-digest");
    CHECK(immutable_rows.rows[0].material_index ==
          rayMissingTextureIndex);
    CHECK((first.tables.material_bytes == repeated.tables.material_bytes));
    CHECK((first.tables.geometry_bytes == repeated.tables.geometry_bytes));
    CHECK(first.tables.material_digest_sha256 ==
          repeated.tables.material_digest_sha256);
    CHECK(first.tables.geometry_digest_sha256 ==
          repeated.tables.geometry_digest_sha256);
    CHECK(first.scene_rows.identity_digest_sha256 ==
          repeated.scene_rows.identity_digest_sha256);
    CHECK(first.tables.material_keys == std::vector<std::string>{
          "asset:models/tree.glb#7",
          "document:materials/mat.json#Leaf"});
    CHECK(first.tables.geometry_allocation_ids ==
          std::vector<std::uint64_t>{4, 9});
    REQUIRE(first.tables.material_rows.size() == 2);
    CHECK(first.tables.material_rows[0].base_color_texture_index ==
          rayMissingTextureIndex);
    CHECK(first.tables.material_rows[1].alpha_cutoff == 0.625F);
    CHECK(first.tables.material_rows[1].alpha_mode == 1);
    CHECK(first.scene_rows.rows[0].material_index == 1);
    CHECK(first.scene_rows.rows[0].geometry_index == 1);
    CHECK(first.scene_rows.rows[1].material_index == 0);
    CHECK(first.scene_rows.rows[1].geometry_index == 0);
    CHECK(first.scene_rows.material_table_digest_sha256 ==
          first.tables.material_digest_sha256);
    CHECK(first.scene_rows.geometry_table_digest_sha256 ==
          first.tables.geometry_digest_sha256);

    CHECK(offsetof(RayMaterialTableRow, base_color_factor) == 0);
    CHECK(offsetof(RayMaterialTableRow, alpha_cutoff) == 16);
    CHECK(offsetof(RayMaterialTableRow, alpha_mode) == 20);
    CHECK(offsetof(RayMaterialTableRow,
                   base_color_texture_index) == 24);
    CHECK(sizeof(RayMaterialTableRow) == 32);
    CHECK(offsetof(RayGeometryTableRow, vertex_address) == 0);
    CHECK(offsetof(RayGeometryTableRow, index_address) == 8);
    CHECK(offsetof(RayGeometryTableRow, vertex_stride) == 16);
    CHECK(offsetof(RayGeometryTableRow, index_type) == 20);
    CHECK(offsetof(RayGeometryTableRow, attribute_presence) == 24);
    CHECK(sizeof(RayGeometryTableRow) == 32);

    auto changed_materials = materials;
    changed_materials[0].base_color_factor[0] = 0.375F;
    const auto material_mutant = buildRayTracingTableSnapshot(
        immutable_rows, changed_materials, geometry);
    CHECK((material_mutant.tables.material_bytes !=
           first.tables.material_bytes));
    CHECK(material_mutant.tables.material_digest_sha256 !=
          first.tables.material_digest_sha256);
    CHECK((material_mutant.tables.geometry_bytes ==
           first.tables.geometry_bytes));
    CHECK(material_mutant.scene_rows.identity_digest_sha256 !=
          first.scene_rows.identity_digest_sha256);

    auto changed_geometry = geometry;
    changed_geometry.rows[0].gpu.vertex_address += 64;
    const auto geometry_mutant = buildRayTracingTableSnapshot(
        immutable_rows, materials, changed_geometry);
    CHECK((geometry_mutant.tables.geometry_bytes !=
           first.tables.geometry_bytes));
    CHECK(geometry_mutant.tables.geometry_digest_sha256 !=
          first.tables.geometry_digest_sha256);
    CHECK((geometry_mutant.tables.material_bytes ==
           first.tables.material_bytes));
    CHECK(geometry_mutant.scene_rows.identity_digest_sha256 !=
          first.scene_rows.identity_digest_sha256);

    auto presence_geometry = geometry;
    presence_geometry.rows[0].gpu.attribute_presence = 0;
    const auto missing_attributes = buildRayTracingTableSnapshot(
        immutable_rows, materials, presence_geometry);
    CHECK((missing_attributes.tables.geometry_bytes !=
           first.tables.geometry_bytes));
    CHECK(missing_attributes.tables.geometry_digest_sha256 !=
          first.tables.geometry_digest_sha256);

    auto stale_rows = immutable_rows;
    stale_rows.rows[0].resolved_material_generation = 201;
    CHECK_THROWS_WITH(
        buildRayTracingTableSnapshot(stale_rows, materials, geometry),
        ContainsSubstring(
            "pelican.ray_material_snapshot_invalid@1"));
    stale_rows = immutable_rows;
    stale_rows.rows[0].resolved_material_lease = asset_lease;
    CHECK_THROWS_WITH(
        buildRayTracingTableSnapshot(stale_rows, materials, geometry),
        ContainsSubstring(
            "pelican.ray_material_snapshot_invalid@1"));

    auto missing_texture_resources = materials;
    missing_texture_resources[0].base_color_texture_identity =
        "document:materials/mat.json#texture:albedo";
    CHECK_THROWS_WITH(
        buildRayTracingTableSnapshot(
            immutable_rows, missing_texture_resources, geometry),
        ContainsSubstring(
            "requires an sRGB view and default sampler"));

    auto reversed_geometry = geometry;
    std::swap(reversed_geometry.rows[0], reversed_geometry.rows[1]);
    CHECK_THROWS_WITH(
        buildRayTracingTableSnapshot(
            immutable_rows, materials, reversed_geometry),
        ContainsSubstring(
            "geometry rows must be unique non-zero allocation IDs in ascending order"));
}

TEST_CASE(
    "WP368 geometry snapshot uses sorted subrange addresses and transports presence without rejecting",
    "[WP368][ray-tracing][rt2b][geometry][attributes][bda][production][mutation]") {
    RayQueryGeometryInstanceSnapshot high{
        .geometry_allocation_id = 9,
        .index_count = 3,
        .index_offset = 0,
        .vertex_offset = 0,
        .vertex_count = 3,
        .attribute_presence = 0,
    };
    RayQueryGeometryInstanceSnapshot low{
        .geometry_allocation_id = 4,
        .index_count = 3,
        .index_offset = 6,
        .vertex_offset = 3,
        .vertex_count = 3,
        // Values can be all-zero while importer presence remains true.
        .attribute_presence = rayGeometryAttributeUv0,
    };
    const std::array input{high, low};
    const auto snapshot = makeRayTracingGeometrySnapshot(
        input, 0x1000, 0x2000);
    REQUIRE(snapshot.rows.size() == 2);
    CHECK(snapshot.rows[0].geometry_allocation_id == 4);
    CHECK(snapshot.rows[0].gpu.vertex_address == 0x20c0);
    CHECK(snapshot.rows[0].gpu.index_address == 0x1018);
    CHECK(snapshot.rows[0].gpu.vertex_stride == 64);
    CHECK(snapshot.rows[0].gpu.index_type ==
          static_cast<std::uint32_t>(vk::IndexType::eUint32));
    CHECK(snapshot.rows[0].gpu.attribute_presence ==
          rayGeometryAttributeUv0);
    CHECK(snapshot.rows[1].geometry_allocation_id == 9);
    CHECK(snapshot.rows[1].gpu.attribute_presence == 0);

    auto present = high;
    present.attribute_presence = rayGeometryAttributeUv0;
    const std::array missing_input{high};
    const std::array present_input{present};
    RayTracingGeometrySnapshot missing;
    CHECK_NOTHROW(missing = makeRayTracingGeometrySnapshot(
                      missing_input, 0x1000, 0x2000));
    const auto all_zero_but_present = makeRayTracingGeometrySnapshot(
        present_input, 0x1000, 0x2000);
    CHECK(missing.rows[0].gpu.attribute_presence == 0);
    CHECK(all_zero_but_present.rows[0].gpu.attribute_presence ==
          rayGeometryAttributeUv0);
    CHECK(missing.digest_sha256 !=
          all_zero_but_present.digest_sha256);

    auto inconsistent = high;
    inconsistent.index_offset = 3;
    const std::array duplicate{high, inconsistent};
    CHECK_THROWS_WITH(
        makeRayTracingGeometrySnapshot(duplicate, 0x1000, 0x2000),
        ContainsSubstring(
            "geometry allocation identity maps to inconsistent ranges or attributes"));
}

TEST_CASE(
    "WP368 table keys refine material identity without changing RT1b selectors",
    "[WP368][ray-tracing][rt2b][material][selector][canonical-key][production][mutation]") {
    const auto layout = parseRayGroupLayout(R"json({
      "schema":"pelican.ray_group_layout","version":1,
      "ray_types":["primary"],"default_group":"opaque",
      "groups":[{"name":"opaque"},{"name":"foliage"}],
      "misses":[{"name":"sky"}],
      "engine_record":["material_index","geometry_index"],
      "record":[]
    })json");
    const auto bindings = parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":1,"bindings":[{
        "material":{"document":"materials/multi.json"},
        "group":"foliage","record":{}
      }]
    })json", layout);
    const auto lease_a = wp368Lease(1);
    const auto lease_b = wp368Lease(2);
    const std::array inputs{
        RaySceneRowInput{
            .scene_identity = "scene-a",
            .geometry_identity = "geometry-a",
            .material_document = "materials/multi.json",
            .material_name = "Oak",
            .resolved_material_handle = 1,
            .resolved_material_generation = 10,
            .resolved_material_lease = lease_a,
            .geometry_allocation_id = 1,
            .source_index = 0,
        },
        RaySceneRowInput{
            .scene_identity = "scene-b",
            .geometry_identity = "geometry-b",
            .material_document = "materials/multi.json",
            .material_name = "Pine",
            .resolved_material_handle = 2,
            .resolved_material_generation = 20,
            .resolved_material_lease = lease_b,
            .geometry_allocation_id = 2,
            .source_index = 1,
        },
    };
    const auto rows = resolveRaySceneRows(inputs, layout, &bindings);
    REQUIRE(rows.rows.size() == 2);
    CHECK(rows.rows[0].group_index == layout.groupIndex("foliage"));
    CHECK(rows.rows[1].group_index == layout.groupIndex("foliage"));
    CHECK(rows.rows[0].material_reference ==
          "document:materials/multi.json#Oak");
    CHECK(rows.rows[1].material_reference ==
          "document:materials/multi.json#Pine");

    auto renamed = inputs;
    renamed[0].material_name = "Zed";
    const auto renamed_rows = resolveRaySceneRows(
        renamed, layout, &bindings);
    CHECK(std::ranges::all_of(
        renamed_rows.rows,
        [&](const auto &row) {
            return row.group_index == layout.groupIndex("foliage");
        }));
    CHECK(std::ranges::any_of(
        renamed_rows.rows, [](const auto &row) {
            return row.material_reference ==
                   "document:materials/multi.json#Zed";
        }));

    RaySceneRowInput embedded{
        .scene_identity = "scene-c",
        .geometry_identity = "geometry-c",
        .material_asset = "models/tree.glb",
        .source_material_index = 7,
        .resolved_material_handle = 3,
        .resolved_material_generation = 30,
        .resolved_material_lease = lease_a,
        .geometry_allocation_id = 3,
    };
    const auto embedded_rows = resolveRaySceneRows(
        std::span<const RaySceneRowInput>{&embedded, 1},
        layout, nullptr);
    REQUIRE(embedded_rows.rows.size() == 1);
    CHECK(embedded_rows.rows[0].material_reference ==
          "asset:models/tree.glb#7");
}

TEST_CASE(
    "WP368 absent table profile creates no RT2b capability shader or target resources",
    "[WP368][ray-tracing][rt2b][inactive][variant][purge][production][mutation]") {
    Wp368CompilerFixture fixture;
    const auto compile = [&](std::shared_ptr<const RayTracingProjectConfig>
                                 project) {
        auto output = runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(),
            fixture.input(std::move(project)));
        REQUIRE(output.variants.size() == fixture.variants.size());
        for (std::size_t index = 0; index < output.variants.size(); ++index) {
            CAPTURE(index);
            REQUIRE(output.variants[index].compiled_pipeline != nullptr);
            CHECK(output.variants[index].graph_variant ==
                  fixture.variants[index].graph_variant);
        }
        return output;
    };

    const auto absent = compile(nullptr);
    std::array<nlohmann::json, 3> absent_targets;
    for (std::size_t index = 0; index < absent.variants.size(); ++index) {
        const auto &variant = absent.variants[index];
        CAPTURE(index);
        CHECK(wp368BindlessCapabilityCount(
                  *variant.compiled_pipeline) == 0);
        CHECK(variant.compute_task_definitions.empty());
        CHECK(variant.buffer_definitions.empty());
        REQUIRE(variant.normalized_config.contains("render_targets"));
        CHECK(variant.physical_package == nullptr);
        absent_targets[index] =
            variant.normalized_config.at("render_targets");
    }

    auto legacy_project = std::make_shared<RayTracingProjectConfig>();
    legacy_project->layout = wp368Layout(nullptr);
    const auto legacy = compile(legacy_project);
    for (std::size_t index = 0; index < legacy.variants.size(); ++index) {
        const auto &variant = legacy.variants[index];
        CAPTURE(index);
        CHECK(wp368BindlessCapabilityCount(
                  *variant.compiled_pipeline) == 0);
        CHECK(variant.compute_task_definitions.empty());
        CHECK(variant.buffer_definitions.empty());
        CHECK(variant.physical_package == nullptr);
        CHECK(variant.normalized_config.at("render_targets") ==
              absent_targets[index]);
    }

    auto active_project = std::make_shared<RayTracingProjectConfig>();
    active_project->layout = wp368Layout(
        nlohmann::json::array(
            {"material_index", "geometry_index"}));
    const auto active = compile(active_project);
    // Direct mutation contrast: enabling the engine prefix is the only input
    // change, and it creates exactly one real compiled-plan capability demand.
    for (std::size_t index = 0; index < active.variants.size(); ++index) {
        const auto &variant = active.variants[index];
        CAPTURE(index);
        CHECK(wp368BindlessCapabilityCount(
                  *variant.compiled_pipeline) == 1);
        CHECK(variant.compute_task_definitions.empty());
        CHECK(variant.buffer_definitions.empty());
        CHECK(variant.physical_package == nullptr);
        CHECK(variant.normalized_config.at("render_targets") ==
              absent_targets[index]);
    }
}

TEST_CASE(
    "WP368 set three descriptor ABI and generated accessors are exact",
    "[WP368][ray-tracing][rt2b][descriptor][generated-include][cache-key][mutation]") {
    const auto bindings = rayMaterialDescriptorSetLayoutBindings();
    const auto flags = rayMaterialDescriptorBindingFlags();
    REQUIRE(bindings.size() == 3);
    REQUIRE(flags.size() == bindings.size());
    CHECK(bindings[0].binding == 0);
    CHECK(bindings[0].descriptorType ==
          vk::DescriptorType::eStorageBuffer);
    CHECK(bindings[0].descriptorCount == 1);
    CHECK(bindings[1].binding == 1);
    CHECK(bindings[1].descriptorType ==
          vk::DescriptorType::eCombinedImageSampler);
    CHECK(bindings[1].descriptorCount == 1024);
    CHECK(bindings[2].binding == 2);
    CHECK(bindings[2].descriptorType ==
          vk::DescriptorType::eStorageBuffer);
    CHECK(bindings[2].descriptorCount == 1);
    CHECK(flags[0] == vk::DescriptorBindingFlags{});
    CHECK(flags[1] ==
          vk::DescriptorBindingFlagBits::ePartiallyBound);
    CHECK(flags[2] == vk::DescriptorBindingFlags{});

    const PipelineFactory::DescriptorSetLayoutKey production_key{
        bindings, flags};
    auto missing_flag_key = production_key;
    missing_flag_key.binding_flags[1] = {};
    CHECK_FALSE(production_key == missing_flag_key);
    CHECK(PipelineFactory::DescriptorSetLayoutKeyHash{}(
              production_key) !=
          PipelineFactory::DescriptorSetLayoutKeyHash{}(
              missing_flag_key));

    const auto layout = wp368Layout(nlohmann::json::array(
        {"material_index", "geometry_index"}));
    const auto source = makeRayGroupLayoutVirtualInclude(layout);
    for (const auto token : {
             "#extension GL_EXT_nonuniform_qualifier : require",
             "layout(set = 3, binding = 0, std430)",
             "layout(set = 3, binding = 1) uniform sampler2D pelican_ray_material_textures[1024]",
             "layout(set = 3, binding = 2, std430)",
             "nonuniformEXT(row.base_color_texture_index)",
             "pelicanRayFetchUV",
             "pelicanRayFetchNormal",
             "pelicanRayFetchColor0",
             "vertex_index * (row.vertex_stride / 4u) + 10u",
             "vertex_index * (row.vertex_stride / 4u) + 3u",
             "vertex_index * (row.vertex_stride / 4u) + 12u",
         }) {
        CAPTURE(token);
        CHECK(source.find(token) != std::string::npos);
    }

    const auto legacy = wp368Layout(nullptr);
    const auto legacy_source = makeRayGroupLayoutVirtualInclude(legacy);
    CHECK(legacy_source.find("set = 3") == std::string::npos);
    CHECK(legacy_source.find("nonuniformEXT") == std::string::npos);
    CHECK(legacy_source.find("pelicanRayFetchUV") ==
          std::string::npos);

    const TargetEndpoint endpoint{
        .id = "device:wp368",
        .kind = TargetEndpointKind::vulkan_device,
    };
    const std::array required{
        std::string{vulkanRayMaterialBindlessCapability}};
    CHECK_THROWS_WITH(
        requireVulkanEndpointCapabilities(endpoint, required),
        ContainsSubstring(
            "pelican.plan.ray_material_bindless_required_unavailable@1"));
    auto capable = endpoint;
    capable.capabilities.assign(required.begin(), required.end());
    CHECK_NOTHROW(requireVulkanEndpointCapabilities(capable, required));
}

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE(
    "WP368 generated table include compiles and SPIR-V mirrors the physical record and set three",
    "[WP368][ray-tracing][rt2b][spirv][mirror][generated-accessor][production][mutation]") {
    const auto layout = wp368Layout(nlohmann::json::array(
        {"geometry_index", "material_index"}));
    std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
)glsl";
    source += makeRayGroupLayoutVirtualInclude(layout);
    source += R"glsl(
layout(location=0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer Record {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
    vec4 tint;
} record_data;
void main() {
    vec2 uv = pelicanRayFetchUV(record_data.geometry_index,
                               gl_PrimitiveID, hit_attribute);
    vec3 normal = pelicanRayFetchNormal(record_data.geometry_index,
                                        gl_PrimitiveID, hit_attribute);
    vec4 color = pelicanRayFetchColor0(record_data.geometry_index,
                                       gl_PrimitiveID, hit_attribute);
    vec4 material = pelicanRaySampleBaseColor(
        record_data.material_index, uv);
    payload = material + color + vec4(normal, 0.0) +
              record_data.tint;
}
)glsl";

    ShaderCompiler compiler;
    const auto compiled = compiler.compileSource(
        source, vk::ShaderStageFlagBits::eClosestHitKHR,
        "wp368-mirror.rchit");
    INFO(compiled.log);
    REQUIRE(compiled.ok);
    const auto module = inspectRayTracingSpirv(
        compiled.spirv, "wp368-mirror.rchit");
    const auto &entry = selectRayTracingEntryPoint(
        module, ShaderStage::closesthit, "main",
        "wp368-mirror.rchit");
    REQUIRE(entry.shader_record);
    REQUIRE(entry.shader_record->members.size() == 3);
    CHECK(entry.shader_record->members[0].offset == 0);
    CHECK(entry.shader_record->members[0].type ==
          RayTracingCanonicalType{
              .kind = RayTracingCanonicalTypeKind::unsigned_integer,
              .bit_width = 32});
    CHECK(entry.shader_record->members[1].offset == 4);
    CHECK(entry.shader_record->members[1].type ==
          entry.shader_record->members[0].type);
    CHECK(entry.shader_record->members[2].offset == 16);
    CHECK(entry.shader_record->members[2].type ==
          RayTracingCanonicalType{
              .kind = RayTracingCanonicalTypeKind::vector,
              .element_count = 4,
              .elements = {RayTracingCanonicalType{
                  .kind = RayTracingCanonicalTypeKind::floating,
                  .bit_width = 32}}});

    const auto reflection = reflect(compiled.spirv);
    const auto binding = [&](std::uint32_t number) -> const ReflectedBinding & {
        const auto found = std::ranges::find_if(
            reflection.bindings, [&](const auto &candidate) {
                return candidate.set == 3 &&
                       candidate.binding == number;
            });
        REQUIRE(found != reflection.bindings.end());
        return *found;
    };
    CHECK(binding(0).type == vk::DescriptorType::eStorageBuffer);
    CHECK(binding(0).count == 1);
    CHECK(binding(1).type ==
          vk::DescriptorType::eCombinedImageSampler);
    CHECK(binding(1).count == 1024);
    CHECK(binding(2).type == vk::DescriptorType::eStorageBuffer);
    CHECK(binding(2).count == 1);
    CHECK(std::ranges::count_if(
              reflection.bindings, [](const auto &candidate) {
                  return candidate.set == 3;
              }) == 3);
}
#endif

} // namespace Pelican
