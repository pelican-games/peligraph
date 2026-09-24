#include "../src/core/shader/pipelinefactory.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/shader/raytracingvalidation.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderreflection.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/vkcore/shaderresolution.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderpipelinegpuarena.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/project/vulkanviewplanning.hpp"
#include "../src/project/gltfmateriallowering.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <picosha2.h>
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
        {"version", 3},
        {"ray_types", {"primary"}},
        {"default_group", "opaque"},
        {"groups", {{{"name", "opaque"}, {"type", "triangles"}}}},
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
    // MG-2b: the authored config is a graph root.
    nlohmann::json config = Pelican::TestSupport::renderGraphRoot(nlohmann::json::parse(R"json({
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
    })json"));
    PathResolver path_resolver;
    GraphTransformRegistrySnapshot graph_transforms;
    RenderStrategyRegistrySnapshot render_strategies;
    SubgraphReplacementRegistrySnapshot subgraph_replacements;
    VulkanRenderCompilerBackendContext backend{
        vk::Format::eB8G8R8A8Srgb,
        vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    std::vector<RenderCompilerProgramVariantRequest> variants = [] {
        std::vector<RenderCompilerProgramVariantRequest> result{
            RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::flat,
            .artifact = RenderCompilerProgramArtifact::data_only,
        }};
#if PELICAN_WITH_OPENXR
        result.push_back(RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::xr,
            .artifact = RenderCompilerProgramArtifact::data_only,
        });
#endif
        result.push_back(RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::preview,
            .artifact = RenderCompilerProgramArtifact::data_only,
        });
        return result;
    }();

    RenderCompilerProgramInput input(
        std::shared_ptr<const RayTracingProjectConfig> project) const {
        return {
            .rendering_config = config,
            .source_name = "WP368 inactive resource contrast",
            .path_resolver = path_resolver,
            .runtime_shader_compiler_enabled =
                PELICAN_RUNTIME_SHADER_COMPILER != 0,
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

std::shared_ptr<const RayTracingProjectConfig>
loadWp368Sidecar(bool runtime_shader_compiler_enabled,
                 std::size_t *loads = nullptr) {
    const auto root = Pelican::TestSupport::renderGraphRoot(nlohmann::json{
        {"ray_group_layout", "layout.json"},
        {"ray_bindings", "bindings.json"},
    });
    return loadRayTracingProjectConfig(
        root,
        [&](std::string_view reference) {
            if (loads != nullptr) ++*loads;
            if (reference == "layout.json") {
                return std::string{R"json({
                  "schema":"pelican.ray_group_layout","version":3,
                  "ray_types":["primary"],"default_group":"opaque",
                  "groups":[{"name":"opaque","type":"triangles"}],
                  "misses":[{"name":"sky"}],
                  "engine_record":["material_index","geometry_index"],
                  "record":[{"name":"tint","type":"vec4"}]
                })json"};
            }
            if (reference == "bindings.json") {
                return std::string{R"json({
                  "schema":"pelican.ray_bindings","version":2,
                  "bindings":[{
                    "material":{"asset":"models/tree.glb","source_material_index":0},
                    "group":"opaque","record":{"tint":[1,0,0,1]}
                  }]
                })json"};
            }
            throw std::runtime_error(
                "unexpected WP368 sidecar reference");
        },
        runtime_shader_compiler_enabled);
}

RayGroupLayout wp369ProfileLayout() {
    return parseRayGroupLayout(R"json({
      "schema":"pelican.ray_group_layout","version":3,
      "portable_profile":"material_alpha_v1",
      "ray_types":["primary"],"default_group":"opaque",
      "groups":[{"name":"opaque","type":"triangles"}],
      "misses":[{"name":"sky"}],
      "engine_record":["material_index","geometry_index"],
      "record":[]
    })json", "project://ray_layout.json");
}

nlohmann::json wp369TypedTask() {
    return nlohmann::json::parse(R"json({
      "compute_tasks":[{
        "name":"alpha_trace",
        "ray_tracing":{
          "raygen":"project://shaders/alpha_trace",
          "traced_ray_types":["primary"],
          "misses":{"sky":"project://shaders/sky"},
          "hit_shaders":{"primary":{"opaque":{
            "closesthit":"project://shaders/alpha_hit"
          }}}
        },
        "writes":["alpha_result"],
        "resource_ports":{"alpha_result":{
          "resource":"alpha_result","access":"storage"
        }},
        "dispatch":{"rays_from":{"port":"alpha_result"}}
      }]
    })json");
}

RayMaterialTableSource wp369MaterialSource(
    const NormalizedAlphaIr &ir, int handle,
    std::uint64_t generation,
    std::shared_ptr<const void> lease) {
    RayMaterialTableSource result{
        .material_handle = handle,
        .material_generation = generation,
        .material_lease = std::move(lease),
        .canonical_key = std::string{ir.materialIdentity()},
        .base_color_factor = {1.0F, 1.0F, 1.0F,
                              ir.baseFactorA()},
        .alpha_cutoff = ir.cutoff(),
        .alpha_mode = static_cast<std::uint32_t>(ir.alphaMode()),
        .base_color_texture_identity =
            std::string{ir.textureIdentity()},
        .has_normalized_alpha_ir = true,
        .alpha_source_kind = std::string{ir.sourceKind()},
        .alpha_material_identity =
            std::string{ir.materialIdentity()},
        .alpha_texture_identity =
            std::string{ir.textureIdentity()},
        .normalized_alpha_mode =
            static_cast<std::uint32_t>(ir.alphaMode()),
        .normalized_alpha_cutoff = ir.cutoff(),
        .normalized_base_factor_a = ir.baseFactorA(),
        .normalized_uv_offset_scale = ir.uvOffsetScale(),
        .normalized_uv_rotation = ir.uvRotation(),
        .normalized_uv_transform_state =
            static_cast<std::uint32_t>(
                ir.uvTransformState()),
        .normalized_alpha_digest_sha256 =
            std::string{ir.digestSha256()},
    };
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
      "schema":"pelican.ray_bindings","version":2,"bindings":[{
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
          "schema":"pelican.ray_group_layout","version":3,
          "ray_types":["primary"],"default_group":"opaque",
          "groups":[{"name":"opaque","type":"triangles"}],
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
      "schema":"pelican.ray_group_layout","version":3,
      "ray_types":["primary"],"default_group":"opaque",
      "groups":[{"name":"opaque","type":"triangles"},{"name":"foliage","type":"triangles"}],
      "misses":[{"name":"sky"}],
      "engine_record":["material_index","geometry_index"],
      "record":[]
    })json");
    const auto bindings = parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":2,"bindings":[{
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
    "WP368 inactive production profile and build matrix enforce RT2b owner gates",
    "[WP368][ray-tracing][rt2b][inactive][variant][purge][production][matrix][mutation]") {
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER,
            PELICAN_WITH_OPENXR,
            PELICAN_WITH_IMGUI,
            PELICAN_WP368_SKIP_DEVSTUDIO);
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
    std::vector<nlohmann::json> absent_targets(
        absent.variants.size());
    for (std::size_t index = 0; index < absent.variants.size(); ++index) {
        const auto &variant = absent.variants[index];
        CAPTURE(index);
        CHECK(wp368BindlessCapabilityCount(
                  *variant.compiled_pipeline) == 0);
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
        CHECK(variant.buffer_definitions.empty());
        CHECK(variant.physical_package == nullptr);
        CHECK(variant.normalized_config.at("render_targets") ==
              absent_targets[index]);
    }

    // Build-matrix contrast through the production root-sidecar gate.  The
    // disabled build must reject the named contract before invoking either
    // real sidecar loader.
    std::size_t loads = 0;
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto project = loadWp368Sidecar(
        PELICAN_RUNTIME_SHADER_COMPILER != 0, &loads);
    REQUIRE(project != nullptr);
    CHECK(loads == 2);
    CHECK(project->layout.usesRayTables());
    CHECK(project->layout.source_reference == "layout.json");
    REQUIRE(project->bindings.has_value());
    CHECK(project->bindings->source_reference ==
          "bindings.json");
#else
    CHECK_THROWS_WITH(
        loadWp368Sidecar(
            PELICAN_RUNTIME_SHADER_COMPILER != 0, &loads),
        ContainsSubstring(
            "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
    CHECK(loads == 0);
#endif

    // The same executable observes the production GPU-owner policy selected
    // by the OpenXR build flag, rather than a mirrored test-only condition.
#if PELICAN_WITH_OPENXR
    const auto flat_owner = renderPipelineVariantGpuOwnerScope(
        true, RenderPipelineGraphVariant::flat);
    const auto xr_owner = renderPipelineVariantGpuOwnerScope(
        true, RenderPipelineGraphVariant::xr);
    CHECK(flat_owner == "render_pipeline/variants");
    CHECK(xr_owner == flat_owner);
#else
    CHECK(renderPipelineVariantGpuOwnerScope(
              false, RenderPipelineGraphVariant::flat) ==
          "render_pipeline/flat");
    CHECK_THROWS_WITH(
        renderPipelineVariantGpuOwnerScope(
            true, RenderPipelineGraphVariant::xr),
        ContainsSubstring(
            "XR graph variant is unavailable in this build"));
#endif
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

TEST_CASE(
    "WP369 layout profile generates every traced anyhit slot and profile absence creates no artifact",
    "[WP369][ray-tracing][rt2b-2][profile][grammar][projection][inactive][production][mutation]") {
    const auto layout = wp369ProfileLayout();
    REQUIRE(layout.usesMaterialAlphaProfile());
    CHECK(layout.portable_profile ==
          RayPortableProfile::material_alpha_v1);

    RayTracingProjectConfig project{.layout = layout};
    const auto tasks = parseComputeTaskDefinitionsFromConfigJson(
        wp369TypedTask(), &project);
    REQUIRE(tasks.size() == 1);
    REQUIRE(tasks.front().ray_tracing);
    const auto &ray = *tasks.front().ray_tracing;
    REQUIRE(ray.any_hit_shader_slots.size() == 1);
    REQUIRE(ray.any_hit_shader_slots.front());
    CHECK(ray.any_hit_shader_slots.front()->ref ==
          "generated://material_alpha_v1");
    CHECK(ray.any_hit_shader_slots.front()->kind ==
          ShaderReferenceKind::engine_generated);
    CHECK(std::ranges::none_of(
        tasks.front().declared_shader_refs,
        [](const auto &reference) {
            return reference.stage == DeclaredShaderStage::anyhit;
        }));

    RayTracingProjectConfig multi_project{
        .layout = parseRayGroupLayout(R"json({
          "schema":"pelican.ray_group_layout","version":3,
          "portable_profile":"material_alpha_v1",
          "ray_types":["primary","secondary"],
          "default_group":"opaque",
          "groups":[{"name":"opaque","type":"triangles"},{"name":"foliage","type":"triangles"}],
          "misses":[{"name":"sky"}],
          "engine_record":["material_index","geometry_index"],
          "record":[]
        })json", "project://multi_ray_layout.json")};
    auto multi_task = wp369TypedTask();
    auto &multi_ray = multi_task["compute_tasks"][0]["ray_tracing"];
    multi_ray["traced_ray_types"] = {"primary", "secondary"};
    const auto hit_template =
        multi_ray["hit_shaders"]["primary"]["opaque"];
    multi_ray["hit_shaders"]["primary"]["foliage"] = hit_template;
    multi_ray["hit_shaders"]["secondary"]["opaque"] = hit_template;
    multi_ray["hit_shaders"]["secondary"]["foliage"] = hit_template;
    const auto multi_tasks = parseComputeTaskDefinitionsFromConfigJson(
        multi_task, &multi_project);
    REQUIRE(multi_tasks.size() == 1);
    REQUIRE(multi_tasks.front().ray_tracing);
    const auto &multi_slots =
        multi_tasks.front().ray_tracing->any_hit_shader_slots;
    REQUIRE(multi_slots.size() == 4);
    for (std::size_t slot = 0; slot < multi_slots.size(); ++slot) {
        CAPTURE(slot);
        REQUIRE(multi_slots[slot]);
        CHECK(multi_slots[slot]->ref ==
              "generated://material_alpha_v1");
        CHECK(multi_slots[slot]->kind ==
              ShaderReferenceKind::engine_generated);
    }
    // Material alpha belongs only to triangle rows. A procedural group has
    // no material index and may instead supply its own optional any-hit.
    auto mixed_project = multi_project;
    mixed_project.layout.group_types[1] = RayGroupType::procedural;
    auto mixed_task = multi_task;
    for (const auto *ray_name : {"primary", "secondary"}) {
        auto &group = mixed_task["compute_tasks"][0]["ray_tracing"]
                                ["hit_shaders"][ray_name]["foliage"];
        group["intersection"] = "project://shaders/sphere";
    }
    const auto mixed_tasks = parseComputeTaskDefinitionsFromConfigJson(
        mixed_task, &mixed_project);
    const auto &mixed_slots = mixed_tasks.front().ray_tracing->any_hit_shader_slots;
    REQUIRE(mixed_slots.size() == 4);
    CHECK(mixed_slots[0].has_value());
    CHECK(mixed_slots[1].has_value());
    CHECK_FALSE(mixed_slots[2].has_value());
    CHECK_FALSE(mixed_slots[3].has_value());
    mixed_task["compute_tasks"][0]["ray_tracing"]
              ["hit_shaders"]["primary"]["foliage"]["anyhit"] =
        "project://shaders/procedural_filter";
    const auto filtered_tasks = parseComputeTaskDefinitionsFromConfigJson(
        mixed_task, &mixed_project);
    const auto &filter = filtered_tasks.front().ray_tracing->any_hit_shader_slots[2];
    REQUIRE(filter.has_value());
    CHECK(filter->ref == "project://shaders/procedural_filter");

    auto authored_multi = multi_task;
    authored_multi["compute_tasks"][0]["ray_tracing"]
                  ["hit_shaders"]["secondary"]["foliage"]["anyhit"] =
        "project://shaders/authored_filter";
    CHECK_THROWS_WITH(
        parseComputeTaskDefinitionsFromConfigJson(
            authored_multi, &multi_project),
        ContainsSubstring("pelican.ray_profile_authored_anyhit@1") &&
            ContainsSubstring("authored_filter"));

    const auto project_resolution = [](const ComputeTaskDefinition &task) {
        CompiledRenderingPass compiled;
        compiled.compute_tasks.push_back(CompiledComputeTask{
            .definition = task,
            .task_id = ComputeTaskId{0},
        });
        nlohmann::json plan{
            {"schema", "pelican.frame_plan"},
            {"version", 1},
            {"nodes", nlohmann::json::array({
                {{"name", "alpha_trace"}, {"kind", "compute"}},
            })},
        };
        PathResolver resolver;
        resolver.setup(
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                "projects" / "example",
            false);
        appendShaderResolution(plan, compiled, resolver);
        return plan.at("nodes").at(0).at("shader_resolution");
    };
    const auto active_resolution = project_resolution(tasks.front());
    REQUIRE(active_resolution.contains("anyhit_slots"));
    REQUIRE(active_resolution.at("anyhit_slots").size() == 1);
    const auto &generated = active_resolution.at("anyhit_slots").at(0);
    CHECK(generated.at("origin") == "generated");
    CHECK_FALSE(generated.contains("declared_ref"));
    CHECK(generated.at("source_open_ref").is_null());
    CHECK(generated.at("source_open_reason") == "generated");
    CHECK(generated.at("effective_ref") ==
          "generated://material_alpha_v1");

    auto authored = wp369TypedTask();
    authored["compute_tasks"][0]["ray_tracing"]["hit_shaders"]
            ["primary"]["opaque"]["anyhit"] =
        "project://shaders/authored_filter";
    CHECK_THROWS_WITH(
        parseComputeTaskDefinitionsFromConfigJson(authored, &project),
        ContainsSubstring("pelican.ray_profile_authored_anyhit@1") &&
            ContainsSubstring("authored_filter"));

    auto absent_layout_json = nlohmann::json::parse(R"json({
      "schema":"pelican.ray_group_layout","version":3,
      "ray_types":["primary"],"default_group":"opaque",
      "groups":[{"name":"opaque","type":"triangles"}],"misses":[{"name":"sky"}],
      "engine_record":["material_index","geometry_index"],"record":[]
    })json");
    RayTracingProjectConfig absent{
        .layout = parseRayGroupLayout(absent_layout_json.dump())};
    const auto absent_tasks = parseComputeTaskDefinitionsFromConfigJson(
        wp369TypedTask(), &absent);
    REQUIRE(absent_tasks.size() == 1);
    REQUIRE(absent_tasks.front().ray_tracing);
    REQUIRE(absent_tasks.front().ray_tracing->any_hit_shader_slots.size() ==
            1);
    CHECK_FALSE(absent_tasks.front()
                    .ray_tracing->any_hit_shader_slots.front());
    const auto absent_resolution =
        project_resolution(absent_tasks.front());
    CHECK_FALSE(absent_resolution.contains("anyhit_slots"));
    const auto absent_include =
        makeRayGroupLayoutVirtualInclude(absent.layout);
    CHECK(absent_include.size() == 4857);
    CHECK(picosha2::hash256_hex_string(
              absent_include.begin(), absent_include.end()) ==
          // MG-2a: the include carries the layout digest, which is now the
          // parsed-document digest (v1 hashed the raw bytes).
          "71481b74ae0920b492b7ecadb9ea798eb837af9f5615aea14d03a7d7ed0414d2");
    CHECK_THROWS(makeMaterialAlphaAnyHitSource(absent.layout));

    auto partial = absent_layout_json;
    partial["portable_profile"] = "material_alpha_v1";
    partial["engine_record"] = {"material_index"};
    CHECK_THROWS_WITH(
        parseRayGroupLayout(partial.dump()),
        ContainsSubstring(
            "pelican.ray_profile_engine_record_required@1"));
    partial["engine_record"] = {
        "material_index", "geometry_index"};
    partial["portable_profile"] = "future_profile";
    CHECK_THROWS_WITH(
        parseRayGroupLayout(partial.dump()),
        ContainsSubstring("portable_profile"));
}

TEST_CASE(
    "WP369 profile planning enters the mandatory capability lane in every build variant",
    "[WP369][ray-tracing][rt2b-2][capability][matrix][production][mutation]") {
    CAPTURE(PELICAN_RUNTIME_SHADER_COMPILER,
            PELICAN_WITH_OPENXR,
            PELICAN_WITH_IMGUI,
            PELICAN_WP369_SKIP_DEVSTUDIO);
    Wp368CompilerFixture fixture;
    const auto compile = [&](std::shared_ptr<const RayTracingProjectConfig>
                                 project) {
        return runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(),
            fixture.input(std::move(project)));
    };

    const auto absent = compile(nullptr);
#if PELICAN_RUNTIME_SHADER_COMPILER
    auto profile = std::make_shared<RayTracingProjectConfig>();
    profile->layout = wp369ProfileLayout();
    const auto active = compile(profile);
    REQUIRE(active.variants.size() == absent.variants.size());
    for (std::size_t index = 0; index < active.variants.size(); ++index) {
        CAPTURE(index, active.variants[index].graph_variant);
        REQUIRE(active.variants[index].compiled_pipeline != nullptr);
        REQUIRE(absent.variants[index].compiled_pipeline != nullptr);
        CHECK(wp368BindlessCapabilityCount(
                  *active.variants[index].compiled_pipeline) == 1);
        // Direct opt-out contrast: deleting portable_profile together with
        // its RT layout removes the mandatory capability occurrence.
        CHECK(wp368BindlessCapabilityCount(
                  *absent.variants[index].compiled_pipeline) == 0);
    }

    const TargetEndpoint endpoint{
        .id = "device:wp369-profile",
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
    CHECK_NOTHROW(
        requireVulkanEndpointCapabilities(capable, required));
#else
    REQUIRE_FALSE(absent.variants.empty());
    ShaderLibrary registrations{
        ShaderLibraryModuleMode::reflection_only};
    const auto registration_checkpoint =
        registrations.checkpointRegistrations();
    std::size_t document_loads = 0;
    const auto root = Pelican::TestSupport::renderGraphRoot(
        nlohmann::json{{"ray_group_layout", "layout.json"}});
    CHECK_THROWS_WITH(
        loadRayTracingProjectConfig(
            root,
            [&](std::string_view) {
                ++document_loads;
                return std::string{};
            },
            PELICAN_RUNTIME_SHADER_COMPILER != 0),
        ContainsSubstring(
            "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
    // No layout document means no typed task and therefore no generated
    // shader reference can be registered in the production lowering chain.
    CHECK(document_loads == 0);
    CHECK(registrations.registrationCount() == 0);
    CHECK(registrations.registrationsSince(
              registration_checkpoint).empty());
    CHECK(wp368BindlessCapabilityCount(
              *absent.variants.front().compiled_pipeline) == 0);
#endif
}

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE(
    "WP369 generated anyhit omits payload uses explicit lod zero and mirrors the profile record",
    "[WP369][ray-tracing][rt2b-2][generated-anyhit][spirv][payload][lod0][production][mutation]") {
    const auto layout = wp369ProfileLayout();
    const auto source = makeMaterialAlphaAnyHitSource(layout);
    ShaderCompiler compiler;
    const auto compiled = compiler.compileSource(
        source, vk::ShaderStageFlagBits::eAnyHitKHR,
        "generated-material-alpha.rahit",
        ShaderCompileOptions{
            .virtual_includes = {{
                std::string{rayGroupLayoutVirtualIncludeName},
                makeRayGroupLayoutVirtualInclude(layout)}}});
    INFO(compiled.log);
    REQUIRE(compiled.ok);
    const auto module = inspectRayTracingSpirv(
        compiled.spirv, "generated://material_alpha_v1.rahit");
    const auto &entry = selectRayTracingEntryPoint(
        module, ShaderStage::anyhit, "main",
        "generated://material_alpha_v1.rahit");
    CHECK(std::ranges::none_of(
        entry.interfaces, [](const auto &interface) {
            return interface.storage ==
                   RayTracingInterfaceStorage::incoming_ray_payload;
        }));
    REQUIRE(entry.shader_record);
    REQUIRE(entry.shader_record->members.size() == 2);
    CHECK(entry.shader_record->members[0].offset == 0);
    CHECK(entry.shader_record->members[1].offset == 4);
}
#endif

TEST_CASE(
    "WP369 normalized alpha rows resolve attributes overrides and authored identities",
    "[WP369][ray-tracing][rt2b-2][alpha-ir][tables][attributes][override][digest][production][mutation]") {
    const auto layout = wp369ProfileLayout();
    const auto no_texture_ir = normalizeGltfBaseColorAlpha({
        .authored_reference = "models/tree.glb",
        .material_index = 0,
        .alpha_mode = MaterialAlphaMode::mask,
        .cutoff = 0.5F,
        .base_factor_a = 0.75F,
    });
    CHECK(std::string{no_texture_ir.sourceKind()} == "gltf_base_color");
    const auto lease = wp368Lease(369);
    const auto source = wp369MaterialSource(
        no_texture_ir, 7, 11, lease);
    const RaySceneRowSnapshot rows{
        .identity_digest_sha256 = "stable-authored-scene",
        .rows = {RaySceneRow{
            .q = 0,
            .scene_identity = "tree@identity",
            .material_reference = "asset:models/tree.glb#0",
            .resolved_material_handle = 7,
            .resolved_material_generation = 11,
            .resolved_material_lease = lease,
            .geometry_allocation_id = 4,
        }},
    };
    const RayTracingGeometrySnapshot no_attributes{
        .rows = {RayTracingGeometrySnapshotRow{
            .geometry_allocation_id = 4,
            .index_count = 3,
            .vertex_count = 3,
            .gpu = RayGeometryTableRow{
                .vertex_stride = 64,
                .index_type = static_cast<std::uint32_t>(
                    vk::IndexType::eUint32),
            },
        }},
    };
    const std::array sources{source};
    const auto base = buildRayTracingTableSnapshot(
        rows, sources, no_attributes, &layout);
    REQUIRE(base.tables.material_rows.empty());
    REQUIRE(base.tables.profile_material_rows.size() == 1);
    const auto &base_row = base.tables.profile_material_rows.front();
    CHECK(base_row.alpha_mode == 1);
    CHECK(base_row.cutoff == 0.5F);
    CHECK(base_row.base_factor_a == 0.75F);
    CHECK(base_row.texture_index == rayMissingTextureIndex);
    CHECK(base_row.uses_vertex_color == 0);
    CHECK(base.tables.material_bytes.size() ==
          sizeof(RayProfileMaterialTableRow));
    CHECK(base.scene_rows.rows.front().material_index == 0);

    // Resolved non-MASK rows must not enter any normalized-alpha or alpha
    // texture path. Poison every normalized field and leave the ordinary
    // texture resources empty: reverting the MASK guard rejects these rows
    // before their ordinary alpha mode can be emitted.
    for (const auto mode : {0u, 2u}) {
        auto ordinary_source = RayMaterialTableSource{
            .material_handle = static_cast<int>(20 + mode),
            .material_generation = 30 + mode,
            .material_lease = wp368Lease(static_cast<int>(40 + mode)),
            .canonical_key =
                "document:materials/project.material.json#ordinary-" +
                std::to_string(mode),
            .base_color_factor = {0.2F, 0.3F, 0.4F, 0.6F},
            .alpha_cutoff = 2.0F,
            .alpha_mode = mode,
            .base_color_texture_identity = "ordinary-texture-without-view",
            .alpha_material_identity = "poison-material-identity",
            .alpha_texture_identity = "poison-alpha-texture",
            .normalized_alpha_mode = 1u,
            .normalized_alpha_cutoff = -9.0F,
            .normalized_base_factor_a = -8.0F,
            .normalized_uv_offset_scale = {-7.0F, -6.0F, -5.0F, -4.0F},
            .normalized_uv_rotation = -3.0F,
            .normalized_uv_transform_state = 3u,
        };
        const RaySceneRowSnapshot ordinary_rows{
            .rows = {RaySceneRow{
                .q = 0,
                .scene_identity = "ordinary-scene",
                .material_reference = ordinary_source.canonical_key,
                .resolved_material_handle =
                    ordinary_source.material_handle,
                .resolved_material_generation =
                    ordinary_source.material_generation,
                .resolved_material_lease =
                    ordinary_source.material_lease,
                .geometry_allocation_id = 4,
            }},
        };
        const std::array ordinary_sources{ordinary_source};
        const auto ordinary = buildRayTracingTableSnapshot(
            ordinary_rows, ordinary_sources, no_attributes, &layout);
        REQUIRE(ordinary.tables.profile_material_rows.size() == 1);
        const auto &ordinary_row =
            ordinary.tables.profile_material_rows.front();
        CHECK(ordinary_row.alpha_mode == mode);
        CHECK(ordinary_row.cutoff == 2.0F);
        CHECK(ordinary_row.base_factor_a == 0.6F);
        CHECK(ordinary_row.texture_index == rayMissingTextureIndex);
        CHECK(ordinary_row.uv_offset_scale ==
              std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});
        CHECK(ordinary_row.uv_rotation == 0.0F);
        CHECK(ordinary_row.uses_vertex_color == 0u);
    }

    auto overridden_rows = rows;
    overridden_rows.rows.front().alpha_override_mask =
        rayAlphaOverrideBaseFactor |
        rayAlphaOverrideUvTransform;
    overridden_rows.rows.front()
        .alpha_override_base_color_factor[3] = 0.25F;
    overridden_rows.rows.front().alpha_override_uv_offset_scale =
        {0.25F, 0.5F, 2.0F, 3.0F};
    overridden_rows.rows.front().alpha_override_uv_rotation = 0.75F;
    const auto overridden = buildRayTracingTableSnapshot(
        overridden_rows, sources, no_attributes, &layout);
    REQUIRE(overridden.tables.profile_material_rows.size() == 1);
    CHECK(overridden.tables.profile_material_rows.front().base_factor_a ==
          0.25F);
    CHECK(overridden.tables.profile_material_rows.front().uv_offset_scale ==
          std::array<float, 4>{0.25F, 0.5F, 2.0F, 3.0F});
    CHECK(overridden.tables.profile_material_rows.front().uv_rotation ==
          0.75F);
    CHECK((overridden.tables.material_bytes != base.tables.material_bytes));
    CHECK(overridden.tables.material_digest_sha256 !=
          base.tables.material_digest_sha256);

    const auto texture_ir = normalizeGltfBaseColorAlpha({
        .authored_reference = "models/tree.glb",
        .material_index = 0,
        .texture_index = 2,
        .alpha_mode = MaterialAlphaMode::mask,
        .cutoff = 0.5F,
        .base_factor_a = 1.0F,
    });
    auto textured_source = wp369MaterialSource(
        texture_ir, 7, 11, lease);
    textured_source.base_color_srgb_view = vk::ImageView{
        reinterpret_cast<VkImageView>(std::uintptr_t{1})};
    textured_source.default_sampler = vk::Sampler{
        reinterpret_cast<VkSampler>(std::uintptr_t{2})};
    const std::array textured_sources{textured_source};
    CHECK_THROWS_WITH(
        buildRayTracingTableSnapshot(
            rows, textured_sources, no_attributes, &layout),
        ContainsSubstring("pelican.ray_geometry_attribute_missing@1") &&
            ContainsSubstring("TEXCOORD_0") &&
            ContainsSubstring("asset:models/tree.glb#0"));
    auto uv_geometry = no_attributes;
    uv_geometry.rows.front().gpu.attribute_presence =
        rayGeometryAttributeUv0;
    CHECK_NOTHROW(buildRayTracingTableSnapshot(
        rows, textured_sources, uv_geometry, &layout));

    for (const auto state : {
             NormalizedAlphaUvTransformState::non_identity,
             NormalizedAlphaUvTransformState::invalid}) {
        const auto transformed_ir = normalizeGltfBaseColorAlpha({
            .authored_reference = "models/tree.glb",
            .material_index = 0,
            .alpha_mode = MaterialAlphaMode::mask,
            .uv_offset_scale = {0.5F, 0.0F, 1.0F, 1.0F},
            .uv_transform_state = state,
        });
        const auto transformed_source = wp369MaterialSource(
            transformed_ir, 7, 11, lease);
        const std::array transformed_sources{transformed_source};
        CHECK_THROWS_WITH(
            buildRayTracingTableSnapshot(
                rows, transformed_sources, no_attributes, &layout),
            ContainsSubstring(
                "pelican.ray_profile_uv_transform_unsupported@1"));
    }
    const auto authored_identity_ir = normalizeGltfBaseColorAlpha({
        .authored_reference = "models/tree.glb",
        .material_index = 0,
        .alpha_mode = MaterialAlphaMode::mask,
        .uv_transform_state =
            NormalizedAlphaUvTransformState::identity,
    });
    const auto authored_identity_source = wp369MaterialSource(
        authored_identity_ir, 7, 11, lease);
    const std::array authored_identity_sources{
        authored_identity_source};
    CHECK_NOTHROW(buildRayTracingTableSnapshot(
        rows, authored_identity_sources, no_attributes, &layout));

    const auto reloaded_ir = normalizeGltfBaseColorAlpha({
        .authored_reference = "models/tree.glb",
        .material_index = 0,
        .alpha_mode = MaterialAlphaMode::mask,
        .cutoff = 0.5F,
        .base_factor_a = 0.75F,
    });
    CHECK(reloaded_ir == no_texture_ir);
    const auto other_lease = wp368Lease(370);
    const auto reloaded_source = wp369MaterialSource(
        reloaded_ir, 91, 102, other_lease);
    auto reloaded_rows = rows;
    reloaded_rows.rows.front().resolved_material_handle = 91;
    reloaded_rows.rows.front().resolved_material_generation = 102;
    reloaded_rows.rows.front().resolved_material_lease = other_lease;
    const std::array reloaded_sources{reloaded_source};
    const auto reloaded = buildRayTracingTableSnapshot(
        reloaded_rows, reloaded_sources, no_attributes, &layout);
    CHECK((reloaded.tables.material_bytes == base.tables.material_bytes));
    CHECK(reloaded.tables.material_digest_sha256 ==
          base.tables.material_digest_sha256);

    CHECK_THROWS_WITH(
        normalizeGltfBaseColorAlpha({
            .authored_reference = "C:/physical/tree.glb"}),
        ContainsSubstring(
            "pelican.normalized_alpha_identity_invalid@1"));
    auto missing_ir = source;
    missing_ir.has_normalized_alpha_ir = false;
    const std::array missing_ir_sources{missing_ir};
    CHECK_THROWS_WITH(
        buildRayTracingTableSnapshot(
            rows, missing_ir_sources, no_attributes, &layout),
        ContainsSubstring("pelican.ray_profile_alpha_ir_missing@1"));
}

TEST_CASE(
    "WP369 validator permits only generated payload omission and constant non opaque ray flags",
    "[WP369][ray-tracing][rt2b-2][validator][payload][ray-flags][opaque][production][mutation]") {
    const auto f32 = RayTracingCanonicalType{
        .kind = RayTracingCanonicalTypeKind::floating,
        .bit_width = 32,
    };
    const auto u32 = RayTracingCanonicalType{
        .kind = RayTracingCanonicalTypeKind::unsigned_integer,
        .bit_width = 32,
    };
    const auto vec2 = RayTracingCanonicalType{
        .kind = RayTracingCanonicalTypeKind::vector,
        .element_count = 2,
        .elements = {f32},
    };
    const auto interface = [](RayTracingInterfaceStorage storage,
                              std::uint32_t location,
                              RayTracingCanonicalType type) {
        return RayTracingInterfaceVariable{
            .storage = storage,
            .location = location,
            .type = std::move(type),
            .fingerprint = "test",
            .byte_size = 4,
        };
    };
    const RayTracingShaderRecordReflection record{
        .members = {
            {.type = u32, .offset = 0},
            {.type = u32, .offset = 4},
        },
    };
    RayTracingEntryPointReflection raygen{
        .key = {spirvExecutionModelForShaderStage(
                    ShaderStage::raygen),
                "main"},
        .interfaces = {interface(
            RayTracingInterfaceStorage::ray_payload, 0, f32)},
    };
    raygen.trace_calls.push_back(RayTracingTraceCall{
        .caller_entry = raygen.key,
        .instruction_number = 12,
        .payload_type = f32,
        .miss_index = 0,
        .sbt_record_offset = 0,
        .sbt_record_stride = 1,
        .ray_flags = 0,
    });
    RayTracingEntryPointReflection miss{
        .key = {spirvExecutionModelForShaderStage(
                    ShaderStage::miss),
                "main"},
        .interfaces = {interface(
            RayTracingInterfaceStorage::incoming_ray_payload, 0, f32)},
    };
    RayTracingEntryPointReflection closest{
        .key = {spirvExecutionModelForShaderStage(
                    ShaderStage::closesthit),
                "main"},
        .interfaces = {
            interface(
                RayTracingInterfaceStorage::incoming_ray_payload, 0,
                f32),
            interface(
                RayTracingInterfaceStorage::hit_attribute, 0, vec2),
        },
        .shader_record = record,
    };
    RayTracingEntryPointReflection generated_anyhit{
        .key = {spirvExecutionModelForShaderStage(
                    ShaderStage::anyhit),
                "main"},
        .interfaces = {interface(
            RayTracingInterfaceStorage::hit_attribute, 0, vec2)},
        .shader_record = record,
    };
    const auto raygen_occurrence = RayTracingShaderOccurrence{
        .stage = ShaderStage::raygen,
        .diagnostic_uri = "project://shaders/profile_trace.rgen",
        .entry = &raygen,
    };
    const std::array misses{RayTracingShaderOccurrence{
        .stage = ShaderStage::miss,
        .group_index = 1,
        .diagnostic_uri = "project://shaders/sky.rmiss",
        .entry = &miss,
    }};
    const auto closest_occurrence = RayTracingShaderOccurrence{
        .stage = ShaderStage::closesthit,
        .group_index = 2,
        .diagnostic_uri = "project://shaders/profile.rchit",
        .entry = &closest,
    };
    auto anyhit_occurrence = RayTracingShaderOccurrence{
        .stage = ShaderStage::anyhit,
        .group_index = 2,
        .diagnostic_uri = "generated://material_alpha_v1.rahit",
        .entry = &generated_anyhit,
        .engine_generated = true,
    };
    std::array pairs{RayTracingHitGroupOccurrence{
        .closest_hit = closest_occurrence,
        .any_hit = anyhit_occurrence,
    }};
    const auto layout = wp369ProfileLayout();
    const std::array<std::uint32_t, 1> traced{0};
    const RayTracingRuntimeFacts facts{
        .max_ray_recursion_depth = 8,
        .max_shader_group_stride = 4096,
        .max_ray_hit_attribute_size = 32,
        .max_ray_dispatch_invocation_count = 1u << 20u,
        .max_dispatch_dimensions = {4096, 4096, 4096},
        .device_name = "WP369 validator",
    };
    const auto validate = [&] {
        return validateRayTracingPipelineInterfaces(
            {.raygen = raygen_occurrence,
             .misses = misses,
             .group_layout = &layout,
             .traced_ray_type_indices = traced,
             .hit_group_occurrences = pairs},
            facts);
    };
    const auto active_flags_zero_digest = validate();
    CHECK(active_flags_zero_digest.starts_with("rt1b-fnv1a64:"));
    raygen.trace_calls.front().ray_flags = 2;
    CHECK(validate() != active_flags_zero_digest);
    raygen.trace_calls.front().ray_flags = 0;

    auto inactive_layout = layout;
    inactive_layout.portable_profile.reset();
    auto authored_anyhit = generated_anyhit;
    authored_anyhit.interfaces.push_back(interface(
        RayTracingInterfaceStorage::incoming_ray_payload, 0, f32));
    auto authored_anyhit_occurrence = anyhit_occurrence;
    authored_anyhit_occurrence.entry = &authored_anyhit;
    authored_anyhit_occurrence.engine_generated = false;
    const std::array inactive_pairs{RayTracingHitGroupOccurrence{
        .closest_hit = closest_occurrence,
        .any_hit = authored_anyhit_occurrence,
    }};
    const auto validate_inactive = [&] {
        return validateRayTracingPipelineInterfaces(
            {.raygen = raygen_occurrence,
             .misses = misses,
             .group_layout = &inactive_layout,
             .traced_ray_type_indices = traced,
             .hit_group_occurrences = inactive_pairs},
            facts);
    };
    const auto inactive_flags_zero_digest = validate_inactive();
    raygen.trace_calls.front().ray_flags = 2;
    // Direct profile-absent contrast: WP369 adds no ray-flag cache salt.
    CHECK(validate_inactive() == inactive_flags_zero_digest);
    raygen.trace_calls.front().ray_flags = 0;

    pairs.front().any_hit->engine_generated = false;
    CHECK_THROWS_WITH(
        validate(),
        ContainsSubstring("pelican.ray_tracing.payload_mismatch@1"));
    pairs.front().any_hit->engine_generated = true;

    raygen.trace_calls.front().ray_flags = 1;
    CHECK_THROWS_WITH(
        validate(),
        ContainsSubstring("pelican.ray_profile_opaque_trace@1") &&
            ContainsSubstring("project://shaders/profile_trace.rgen"));
    raygen.trace_calls.front().ray_flags = std::nullopt;
    CHECK_THROWS_WITH(
        validate(),
        ContainsSubstring("pelican.ray_profile_opaque_trace@1") &&
            ContainsSubstring("non-constant"));
    raygen.trace_calls.front().ray_flags = 0;

}

TEST_CASE(
    "WP369 checker oracle samples one mip with a generated boundary margin",
    "[WP369][ray-tracing][rt2b-2][oracle][checker][margin][single-mip][mutation]") {
    struct CheckerSample {
        std::array<double, 2> uv{};
        double alpha = 0.0;
        double boundary_margin = 0.0;
        std::uint32_t mip = 0;
    };
    const auto generate = [](std::uint32_t cells) {
        std::vector<CheckerSample> result;
        for (std::uint32_t y = 0; y < cells; ++y) {
            for (std::uint32_t x = 0; x < cells; ++x) {
                result.push_back({
                    .uv = {
                        (static_cast<double>(x) + 0.5) / cells,
                        (static_cast<double>(y) + 0.5) / cells,
                    },
                    .alpha = ((x + y) & 1u) == 0 ? 0.25 : 0.75,
                    .boundary_margin = 0.5 / cells,
                    .mip = 0,
                });
            }
        }
        return result;
    };
    const auto samples = generate(8);
    REQUIRE(samples.size() == 64);
    for (const auto &sample : samples) {
        CHECK(sample.boundary_margin >= 1.0 / 16.0);
        CHECK(sample.mip == 0);
        const auto x = static_cast<std::uint32_t>(sample.uv[0] * 8.0);
        const auto y = static_cast<std::uint32_t>(sample.uv[1] * 8.0);
        const auto cpu_alpha =
            ((x + y) & 1u) == 0 ? 0.25 : 0.75;
        CHECK(cpu_alpha == sample.alpha);
        CHECK((sample.alpha < 0.5) ==
              (((x + y) & 1u) == 0));
    }
    auto boundary_mutant = samples.front();
    boundary_mutant.uv[0] = 0.0;
    boundary_mutant.boundary_margin = 0.0;
    CHECK_FALSE(boundary_mutant.boundary_margin >= 1.0 / 16.0);
}

} // namespace Pelican
