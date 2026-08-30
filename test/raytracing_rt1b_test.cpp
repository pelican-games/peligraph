#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/shader/raytracingvalidation.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

TEST_CASE("WP366 staged runtime publication waits for joint submit commit",
          "[ray-tracing][rt1b][transaction][production][mutation]") {
    FrameGraphRuntimeContainer runtime;
    std::size_t lifetime_commits = 0;
    std::size_t coordinated_commits = 0;
    const auto stage = [&] {
        auto prepared = runtime.prepareGeneration({});
        const auto generation = prepared.generation();
        auto lifetime = std::make_shared<int>(366);
        runtime.stagePreparedGeneration(
            std::move(prepared),
            std::static_pointer_cast<const void>(lifetime),
            [&] { ++lifetime_commits; },
            [&](const RendererRuntimeGeneration &) {
                ++coordinated_commits;
            });
        return generation;
    };

    const auto rolled_back = stage();
    CHECK(runtime.snapshot() == nullptr);
    REQUIRE(runtime.renderingSnapshot() != nullptr);
    CHECK(runtime.renderingSnapshot()->generation == rolled_back);
    CHECK(runtime.rollbackStagedGeneration(rolled_back));
    CHECK(runtime.snapshot() == nullptr);
    CHECK(runtime.stagedGeneration() == 0);
    CHECK(lifetime_commits == 0);
    CHECK(coordinated_commits == 0);

    const auto committed = stage();
    runtime.prepareStagedGenerationCommit(committed);
    CHECK(runtime.snapshot() == nullptr);
    CHECK(coordinated_commits == 1);
    REQUIRE(runtime.publishStagedGeneration(committed));
    REQUIRE(runtime.snapshot() != nullptr);
    CHECK(runtime.snapshot()->generation == committed);
    CHECK(runtime.stagedGeneration() == 0);
    CHECK(lifetime_commits == 1);
}
namespace {

using Catch::Matchers::ContainsSubstring;

constexpr std::string_view layoutJson = R"json({
  "schema": "pelican.ray_group_layout",
  "version": 1,
  "ray_types": ["primary", "shadow"],
  "default_group": "opaque",
  "groups": [{"name":"opaque"},{"name":"foliage"}],
  "misses": [{"name":"sky"},{"name":"shadow_open"}],
  "record": [{"name":"tint","type":"vec3"},{"name":"flags","type":"f32"}]
})json";

std::uint32_t readU32(std::span<const std::byte> bytes) {
    REQUIRE(bytes.size() >= 4);
    return std::to_integer<std::uint32_t>(bytes[0]) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 8u) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 16u) |
           (std::to_integer<std::uint32_t>(bytes[3]) << 24u);
}

RayBindings bindingsFor(const RayGroupLayout &layout) {
    return parseRayBindings(R"json({
      "schema":"pelican.ray_bindings","version":1,"bindings":[
        {"material":{"document":"materials/leaf.mat.json"},
         "group":"foliage","record":{"tint":[0.2,0.8,0.4],"flags":7.0}},
        {"material":{"asset":"models/tree.glb","source_material_index":2},
         "group":"opaque","record":{"tint":[0.3,0.7,0.5],"flags":9.0}}
      ]
    })json", layout, "bindings.json");
}

TEST_CASE("WP366 layout schema and generated source are raw-digest bound",
          "[ray-tracing][rt1b][layout][mutation]") {
    const auto layout = parseRayGroupLayout(layoutJson, "layout.json");
    CHECK(layout.ray_types == std::vector<std::string>{"primary", "shadow"});
    CHECK(layout.groups == std::vector<std::string>{"opaque", "foliage"});
    CHECK(layout.misses == std::vector<std::string>{"sky", "shadow_open"});
    CHECK(layout.default_group == "opaque");

    const auto include = makeRayGroupLayoutVirtualInclude(layout);
    CHECK_THAT(include, ContainsSubstring("PELICAN_RAY_TYPE_primary 0"));
    CHECK_THAT(include, ContainsSubstring("PELICAN_RAY_TYPE_shadow 1"));
    CHECK_THAT(include, ContainsSubstring("PELICAN_MISS_sky 0"));
    CHECK_THAT(include, ContainsSubstring(layout.digest_sha256));
    CHECK(include.find("GROUP_") == std::string::npos);
    CHECK(include.find("ROW_") == std::string::npos);

    const auto whitespace = parseRayGroupLayout(
        std::string{layoutJson} + "\n", "layout.json");
    CHECK(whitespace.digest_sha256 != layout.digest_sha256);
    CHECK(makeRayGroupLayoutVirtualInclude(whitespace) != include);

    auto reordered_json = nlohmann::json::parse(layoutJson);
    std::swap(reordered_json["groups"][0], reordered_json["groups"][1]);
    reordered_json["default_group"] = "opaque";
    const auto reordered_raw = reordered_json.dump();
    const auto reordered = parseRayGroupLayout(reordered_raw, "layout.json");
    CHECK(reordered.digest_sha256 != layout.digest_sha256);
    CHECK(makeRayGroupLayoutVirtualInclude(reordered) != include);

    SECTION("unknown fields cannot silently enter the ABI") {
        auto invalid = nlohmann::json::parse(layoutJson);
        invalid["groups"][0]["shader"] = "hidden";
        CHECK_THROWS_WITH(parseRayGroupLayout(invalid.dump()),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_group_layout_invalid@1"));
    }
    SECTION("hyphens cannot create invalid generated symbols") {
        auto invalid = nlohmann::json::parse(layoutJson);
        invalid["ray_types"][0] = "primary-ray";
        CHECK_THROWS_WITH(parseRayGroupLayout(invalid.dump()),
                          Catch::Matchers::ContainsSubstring(
                              "^[A-Za-z0-9_]{1,64}$"));
    }
    SECTION("all three name domains reject duplicates and emptiness") {
        for (const std::string field : {"ray_types", "groups", "misses"}) {
            auto duplicate = nlohmann::json::parse(layoutJson);
            duplicate[field].push_back(duplicate[field][0]);
            CHECK_THROWS_WITH(parseRayGroupLayout(duplicate.dump()),
                              Catch::Matchers::ContainsSubstring("duplicate"));
            auto empty = nlohmann::json::parse(layoutJson);
            empty[field] = nlohmann::json::array();
            CHECK_THROWS_WITH(parseRayGroupLayout(empty.dump()),
                              Catch::Matchers::ContainsSubstring("non-empty"));
        }
    }
    SECTION("default group must resolve") {
        auto invalid = nlohmann::json::parse(layoutJson);
        invalid["default_group"] = "missing";
        CHECK_THROWS_WITH(parseRayGroupLayout(invalid.dump()),
                          Catch::Matchers::ContainsSubstring("is not in groups"));
    }
    SECTION("record v1 rejects capability-dependent types") {
        auto invalid = nlohmann::json::parse(layoutJson);
        invalid["record"][0]["type"] = "u64";
        CHECK_THROWS_WITH(parseRayGroupLayout(invalid.dump()),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_record_type_unsupported@1"));
    }
}

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("WP366 generated include salt drives the real shader cache",
          "[ray-tracing][rt1b][cache][include][production][mutation]") {
    const auto cache = std::filesystem::temp_directory_path() /
        ("pelican-wp366-cache-" + std::to_string(
             std::chrono::steady_clock::now()
                 .time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{cache};
    ShaderCompiler compiler;
    compiler.setCacheDirectory(cache);
    constexpr std::string_view source = R"glsl(
      #version 450
      #include "__pelican_ray_group_layout.glsl"
      void main() {
        gl_Position = vec4(float(PELICAN_RAY_TYPE_primary),
                           float(PELICAN_MISS_sky), 0.0, 1.0);
      }
    )glsl";
    const auto compile = [&](const RayGroupLayout &layout) {
        ShaderCompileOptions options;
        options.virtual_includes.emplace_back(
            std::string{rayGroupLayoutVirtualIncludeName},
            makeRayGroupLayoutVirtualInclude(layout));
        return compiler.compileSource(
            source, vk::ShaderStageFlagBits::eVertex,
            "wp366-cache.vert", options);
    };
    const auto base_layout = parseRayGroupLayout(layoutJson);
    const auto base = compile(base_layout);
    INFO(base.log);
    REQUIRE(base.ok);
    REQUIRE_FALSE(base.cache_hit);

    auto reordered_json = nlohmann::json::parse(layoutJson);
    std::reverse(reordered_json["groups"].begin(),
                 reordered_json["groups"].end());
    const auto reordered = compile(parseRayGroupLayout(
        reordered_json.dump()));
    INFO(reordered.log);
    REQUIRE(reordered.ok);
    CHECK_FALSE(reordered.cache_hit);
    CHECK(reordered.cache_key != base.cache_key);

    const auto whitespace = compile(parseRayGroupLayout(
        std::string{layoutJson} + "\n"));
    INFO(whitespace.log);
    REQUIRE(whitespace.ok);
    CHECK_FALSE(whitespace.cache_hit);
    CHECK(whitespace.cache_key != base.cache_key);
    CHECK(whitespace.cache_key != reordered.cache_key);

    const auto repeated = compile(base_layout);
    REQUIRE(repeated.ok);
    CHECK(repeated.cache_hit);
    CHECK(repeated.cache_key == base.cache_key);

    ShaderCompileOptions disconnected;
    const auto without_hookup = compiler.compileSource(
        source, vk::ShaderStageFlagBits::eVertex,
        "wp366-cache-disconnected.vert", disconnected);
    CHECK_FALSE(without_hookup.ok);
}
#endif

TEST_CASE("WP366 root sidecar is single-load and drives the complete task grammar",
          "[ray-tracing][rt1b][wire][production][mutation]") {
    const auto root = nlohmann::json{
        {"ray_group_layout", "layout.json"},
        {"ray_bindings", "bindings.json"}};
    std::size_t layout_loads = 0;
    std::size_t binding_loads = 0;
    const auto project = loadRayTracingProjectConfig(
        root,
        [&](std::string_view reference) {
            if (reference == "layout.json") {
                ++layout_loads;
                return std::string{layoutJson};
            }
            ++binding_loads;
            return std::string{R"json({
              "schema":"pelican.ray_bindings","version":1,"bindings":[]
            })json"};
        },
        true);
    REQUIRE(project);
    CHECK(layout_loads == 1);
    CHECK(binding_loads == 1);

    auto config = nlohmann::json::parse(R"json({
      "compute_tasks":[{
        "name":"trace",
        "ray_tracing":{
          "raygen":"shaders/trace",
          "traced_ray_types":["primary"],
          "misses":{"sky":"shaders/sky","shadow_open":"shaders/open"},
          "hit_shaders":{"primary":{
            "opaque":{"closesthit":"shaders/opaque"},
            "foliage":{"closesthit":"shaders/foliage"}
          }}
        },
        "writes":["mask"],
        "resource_ports":{"mask":{"resource":"mask","access":"storage"}},
        "dispatch":{"rays_from":{"port":"mask"}}
      }]
    })json");
    const auto tasks =
        parseComputeTaskDefinitionsFromConfigJson(config, project.get());
    REQUIRE(tasks.size() == 1);
    REQUIRE(tasks[0].ray_tracing);
    CHECK(tasks[0].ray_tracing->uses_group_layout);
    CHECK(tasks[0].ray_tracing->traced_ray_type_indices ==
          std::vector<std::uint32_t>{0});
    REQUIRE(tasks[0].ray_tracing->hit_shader_slots.size() == 4);
    CHECK(tasks[0].ray_tracing->hit_shader_slots[0].has_value());
    CHECK_FALSE(tasks[0].ray_tracing->hit_shader_slots[1].has_value());
    CHECK(tasks[0].ray_tracing->hit_shader_slots[2].has_value());
    CHECK_FALSE(tasks[0].ray_tracing->hit_shader_slots[3].has_value());

    SECTION("removing one traced group is a named task/group/ray/available failure") {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]["hit_shaders"]
               ["primary"].erase("foliage");
        CHECK_THROWS_WITH(
            parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.hit_shader_missing@1: compute task 'trace' group='foliage' ray_type='primary' available=[opaque]"));
    }
    SECTION("miss names are complete and layout-owned") {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]["misses"].erase(
            "shadow_open");
        CHECK_THROWS_WITH(
            parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.miss_shader_missing@1"));
    }
    SECTION("legacy task syntax is valid only without layout") {
        auto invalid = config;
        auto &pipeline = invalid["compute_tasks"][0]["ray_tracing"];
        pipeline.erase("traced_ray_types");
        pipeline.erase("misses");
        pipeline.erase("hit_shaders");
        pipeline["miss"] = "shaders/legacy";
        pipeline["closesthit"] = "shaders/legacy";
        CHECK_THROWS_WITH(
            parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.legacy_syntax_with_layout@1"));
        CHECK_NOTHROW(parseComputeTaskDefinitionsFromConfigJson(invalid));
    }
    SECTION("explicit SPIR-V is source-only named reject") {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]["raygen"] =
            "shaders/trace.rgen.spv";
        CHECK_THROWS_WITH(
            parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.layout_source_only@1"));
    }
    SECTION("compiler OFF rejects layout before reading its documents") {
        std::size_t loads = 0;
        CHECK_THROWS_WITH(
            loadRayTracingProjectConfig(
                root,
                [&](std::string_view) {
                    ++loads;
                    return std::string{};
                },
                false),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
        CHECK(loads == 0);
    }
}

TEST_CASE("WP366 canonical std430 ABI mirrors binding bytes",
          "[ray-tracing][rt1b][abi][mutation]") {
    const auto layout = parseRayGroupLayout(layoutJson);
    REQUIRE(layout.record.size() == 2);
    CHECK(layout.record[0].offset == 0);
    CHECK(layout.record[0].size == 12);
    CHECK(layout.record[0].alignment == 16);
    CHECK(layout.record[1].offset == 12);
    CHECK(layout.record[1].size == 4);
    CHECK(layout.data_size == 16);
    CHECK(canonicalRayRecordAbiJson(layout) == nlohmann::json{
        {"schema", "pelican.ray_record_abi"}, {"version", 1},
        {"data_size", 16},
        {"members", nlohmann::json::array({
            {{"name", "tint"}, {"type", "vec3"}, {"offset", 0},
             {"size", 12}, {"alignment", 16}},
            {{"name", "flags"}, {"type", "f32"}, {"offset", 12},
             {"size", 4}, {"alignment", 4}}
        })}
    });

    const auto bindings = bindingsFor(layout);
    REQUIRE(bindings.bindings.size() == 2);
    const auto &bytes = bindings.bindings[0].record_data;
    CHECK(readU32(std::span{bytes}.subspan(0, 4)) ==
          std::bit_cast<std::uint32_t>(0.2f));
    CHECK(readU32(std::span{bytes}.subspan(12, 4)) ==
          std::bit_cast<std::uint32_t>(7.0f));

    SECTION("record member set is exact") {
        auto invalid = nlohmann::json::parse(R"json({
          "schema":"pelican.ray_bindings","version":1,"bindings":[
            {"material":{"document":"m"},"group":"opaque",
             "record":{"tint":[0,0,0]}}
          ]})json");
        CHECK_THROWS_WITH(parseRayBindings(invalid.dump(), layout),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_binding_record_invalid@1"));
        invalid["bindings"][0]["record"]["flags"] = 1.0;
        invalid["bindings"][0]["record"]["extra"] = 1.0;
        CHECK_THROWS_WITH(parseRayBindings(invalid.dump(), layout),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_binding_record_invalid@1"));
    }
    SECTION("record member types are exact") {
        auto invalid = nlohmann::json::parse(R"json({
          "schema":"pelican.ray_bindings","version":1,"bindings":[
            {"material":{"document":"m"},"group":"opaque",
             "record":{"tint":[0,0],"flags":"1"}}
          ]})json");
        CHECK_THROWS_WITH(parseRayBindings(invalid.dump(), layout),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_binding_record_invalid@1"));
    }
}

TEST_CASE("WP366 material resolution produces canonical immutable rows",
          "[ray-tracing][rt1b][bindings][rows][mutation]") {
    const auto layout = parseRayGroupLayout(layoutJson);
    const auto bindings = bindingsFor(layout);
    const std::array input{
        RaySceneRowInput{.scene_identity = "TreeB/0", .geometry_identity = "mesh/0",
                         .material_asset = "models/tree.glb", .source_material_index = 2,
                         .source_index = 41},
        RaySceneRowInput{.scene_identity = "TreeA/0", .geometry_identity = "mesh/0",
                         .material_document = "materials/leaf.mat.json", .source_index = 17},
    };
    const auto forward = resolveRaySceneRows(input, layout, &bindings);
    auto reverse_input = input;
    std::reverse(reverse_input.begin(), reverse_input.end());
    const auto reverse = resolveRaySceneRows(reverse_input, layout, &bindings);
    REQUIRE(forward.rows.size() == 2);
    CHECK(forward.rows[0].scene_identity == "TreeA/0");
    CHECK(forward.rows[0].q == 0);
    CHECK(forward.rows[0].group_index == layout.groupIndex("foliage"));
    CHECK(forward.rows[1].q == 1);
    CHECK(forward.identity_digest_sha256 == reverse.identity_digest_sha256);
    CHECK((forward.rows[0].record_data == reverse.rows[0].record_data));
    CHECK((forward.rows[1].record_data == reverse.rows[1].record_data));

    SECTION("only unbound rows use the default group and zero record") {
        const std::array unbound{RaySceneRowInput{
            .scene_identity = "Rock/0", .geometry_identity = "mesh/0",
            .material_asset = "models/rock.glb", .source_material_index = 0}};
        const auto rows = resolveRaySceneRows(unbound, layout, nullptr);
        CHECK(rows.rows[0].group_index == layout.groupIndex("opaque"));
        CHECK(std::all_of(rows.rows[0].record_data.begin(), rows.rows[0].record_data.end(),
                          [](std::byte byte) { return byte == std::byte{0}; }));
    }
    SECTION("a selector resolving twice is rejected") {
        auto duplicate = bindings;
        duplicate.bindings.push_back(duplicate.bindings.front());
        CHECK_THROWS_WITH(resolveRaySceneRows(input, layout, &duplicate),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_binding_selector_duplicate@1"));
    }
    SECTION("an unresolved authored selector is rejected") {
        std::array only_leaf{input[1]};
        CHECK_THROWS_WITH(resolveRaySceneRows(only_leaf, layout, &bindings),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_binding_selector_unresolved@1"));
    }
    SECTION("one geometric row cannot receive divergent bindings") {
        auto ambiguous = input;
        ambiguous[1].scene_identity = ambiguous[0].scene_identity;
        ambiguous[1].geometry_identity = ambiguous[0].geometry_identity;
        CHECK_THROWS_WITH(resolveRaySceneRows(ambiguous, layout, &bindings),
                          Catch::Matchers::ContainsSubstring(
                              "pelican.ray_row_binding_ambiguous@1"));
    }
}

TEST_CASE("WP366 production AS lowering is publish-order independent",
          "[ray-tracing][rt1b][rows][tlas][production][mutation]") {
    auto project = RayTracingProjectConfig{
        .layout = parseRayGroupLayout(layoutJson),
    };
    project.bindings = bindingsFor(project.layout);
    RayQueryGeometryInstanceSnapshot leaf{
        .asset_identity = 9,
        .instance_index = 71,
        .instance_generation = 3,
        .scene_epoch = 4,
        .material_asset_reference = "models/tree.glb",
        .material_document_reference = "materials/leaf.mat.json",
        .source_material_index = 8,
        .geometry_allocation_id = 100,
        .index_count = 3,
        .index_offset = 0,
        .vertex_offset = 0,
        .vertex_count = 3,
        .mesh_index = 0,
        .primitive_index = 0,
        .node_index = 0,
        .world_transform = glm::translate(
            glm::mat4{1.0f}, glm::vec3{-2.0f, 0.0f, 0.0f}),
    };
    RayQueryGeometryInstanceSnapshot trunk{
        .asset_identity = 9,
        .instance_index = 12,
        .instance_generation = 8,
        .scene_epoch = 2,
        .material_asset_reference = "models/tree.glb",
        .source_material_index = 2,
        .geometry_allocation_id = 101,
        .index_count = 3,
        .index_offset = 3,
        .vertex_offset = 3,
        .vertex_count = 3,
        .mesh_index = 0,
        .primitive_index = 1,
        .node_index = 0,
        .world_transform = glm::translate(
            glm::mat4{1.0f}, glm::vec3{2.0f, 0.0f, 0.0f}),
    };
    const std::array forward_inputs{trunk, leaf};
    const std::array reverse_inputs{leaf, trunk};
    const auto forward_rows = resolveRayTracingSceneRows(
        forward_inputs, project);
    const auto reverse_rows = resolveRayTracingSceneRows(
        reverse_inputs, project);
    CHECK(forward_rows.identity_digest_sha256 ==
          reverse_rows.identity_digest_sha256);
    REQUIRE(forward_rows.rows.size() == 2);
    for (std::size_t q = 0; q < forward_rows.rows.size(); ++q) {
        CHECK(forward_rows.rows[q].q == q);
        CHECK(forward_rows.rows[q].scene_identity ==
              reverse_rows.rows[q].scene_identity);
        CHECK(forward_rows.rows[q].geometry_identity ==
              reverse_rows.rows[q].geometry_identity);
        CHECK(forward_rows.rows[q].material_reference ==
              reverse_rows.rows[q].material_reference);
        CHECK(forward_rows.rows[q].group_index ==
              reverse_rows.rows[q].group_index);
        CHECK((forward_rows.rows[q].record_data ==
               reverse_rows.rows[q].record_data));
    }

    const auto forward_classification =
        classifyRayQueryStaticGeometry(forward_inputs);
    const auto reverse_classification =
        classifyRayQueryStaticGeometry(reverse_inputs);
    const auto forward_tlas = lowerRayTracingTlasInstancePlan(
        forward_inputs, forward_classification.static_instance_indices,
        &forward_rows, 2);
    const auto reverse_tlas = lowerRayTracingTlasInstancePlan(
        reverse_inputs, reverse_classification.static_instance_indices,
        &reverse_rows, 2);
    REQUIRE(forward_tlas.size() == 2);
    REQUIRE(reverse_tlas.size() == 2);
    for (std::size_t q = 0; q < forward_tlas.size(); ++q) {
        const auto &left = forward_tlas[q];
        const auto &right = reverse_tlas[q];
        CHECK(left.q == q);
        CHECK(left.instance_custom_index == q);
        CHECK(left.instance_sbt_record_offset == q * 2);
        CHECK(right.q == left.q);
        CHECK(right.instance_custom_index == left.instance_custom_index);
        CHECK(right.instance_sbt_record_offset ==
              left.instance_sbt_record_offset);
        CHECK(forward_inputs[left.source_index].world_transform ==
              reverse_inputs[right.source_index].world_transform);
    }

    SECTION("geometrically coincident production rows reject divergent material bindings") {
        auto ambiguous = forward_inputs;
        ambiguous[1].geometry_allocation_id =
            ambiguous[0].geometry_allocation_id;
        ambiguous[1].index_offset = ambiguous[0].index_offset;
        ambiguous[1].vertex_offset = ambiguous[0].vertex_offset;
        ambiguous[1].mesh_index = ambiguous[0].mesh_index;
        ambiguous[1].primitive_index =
            ambiguous[0].primitive_index;
        ambiguous[1].node_index = ambiguous[0].node_index;
        ambiguous[1].world_transform = ambiguous[0].world_transform;
        project.bindings = parseRayBindings(R"json({
          "schema":"pelican.ray_bindings","version":1,"bindings":[
            {"material":{"document":"materials/leaf.mat.json"},
             "group":"opaque","record":{"tint":[1,0,0],"flags":1}},
            {"material":{"asset":"models/tree.glb","source_material_index":2},
             "group":"foliage","record":{"tint":[0,1,0],"flags":2}}
          ]})json", project.layout);
        CHECK_THROWS_WITH(
            resolveRayTracingSceneRows(ambiguous, project),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_row_binding_ambiguous@1"));
    }
}

TEST_CASE("WP366 q by r addressing packs every row from H of its logical group",
          "[ray-tracing][rt1b][sbt][sentinel][mutation]") {
    const auto layout = parseRayGroupLayout(layoutJson);
    const auto bindings = bindingsFor(layout);
    const std::array inputs{
        RaySceneRowInput{.scene_identity = "TreeB/0", .geometry_identity = "mesh/0",
                         .material_asset = "models/tree.glb", .source_material_index = 2,
                         .source_index = 1},
        RaySceneRowInput{.scene_identity = "TreeA/0", .geometry_identity = "mesh/0",
                         .material_document = "materials/leaf.mat.json", .source_index = 0},
    };
    const auto rows = resolveRaySceneRows(inputs, layout, &bindings);
    constexpr std::uint32_t handle_size = 4;
    // H[opaque,primary], H[opaque,shadow], H[foliage,primary], H[foliage,shadow]
    const std::array<std::byte, 16> handles{
        std::byte{0x10}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x11}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x20}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0x21}, std::byte{0}, std::byte{0}, std::byte{0},
    };
    const auto packed = packRayTracingHitTable(
        layout, {.shader_group_handle_size = handle_size,
                 .shader_group_handle_alignment = 4,
                 .max_shader_group_stride = 64,
                 .group_handles = handles,
                 .rows = rows.rows});
    CHECK(packed.data_start == 4);
    CHECK(packed.record_index_stride == 2);
    CHECK(packed.record_byte_stride == 20);
    CHECK(packed.record_count == 4);
    // Canonical TreeA row selects foliage; TreeB selects opaque.
    CHECK(std::to_integer<unsigned>(packed.bytes[0]) == 0x20);
    CHECK(std::to_integer<unsigned>(packed.bytes[20]) == 0x21);
    CHECK(std::to_integer<unsigned>(packed.bytes[40]) == 0x10);
    CHECK(std::to_integer<unsigned>(packed.bytes[60]) == 0x11);
    CHECK(std::equal(rows.rows[0].record_data.begin(), rows.rows[0].record_data.end(),
                     packed.bytes.begin() + packed.data_start));
    CHECK(std::equal(rows.rows[0].record_data.begin(), rows.rows[0].record_data.end(),
                     packed.bytes.begin() + packed.record_byte_stride + packed.data_start));
    CHECK(rayTracingInstanceRecordOffset(1, 2) == 2);

    SECTION("removing q times R from TLAS base selects the wrong sentinel") {
        std::array<RayQueryGeometryInstanceSnapshot, 2> instances{};
        instances[0].index_count = instances[1].index_count = 3;
        instances[0].vertex_count = instances[1].vertex_count = 3;
        const std::array<std::size_t, 2> static_indices{0, 1};
        const auto production_tlas = lowerRayTracingTlasInstancePlan(
            instances, static_indices, &rows, 2);
        const auto correct_record =
            production_tlas.at(1).instance_sbt_record_offset;
        const auto wrong_record = 1u;
        CHECK(correct_record != wrong_record);
        CHECK(std::to_integer<unsigned>(packed.bytes[correct_record * packed.record_byte_stride]) ==
              0x10);
        CHECK(std::to_integer<unsigned>(packed.bytes[wrong_record * packed.record_byte_stride]) ==
              0x21);
    }
    SECTION("untraced no-op slots retain their valid H handle but zero data") {
        constexpr std::array<std::uint32_t, 1> traced{0};
        const auto masked = packRayTracingHitTable(
            layout, {.shader_group_handle_size = handle_size,
                     .shader_group_handle_alignment = 4,
                     .max_shader_group_stride = 64,
                     .group_handles = handles,
                     .rows = rows.rows,
                     .traced_ray_type_indices = traced});
        CHECK(std::to_integer<unsigned>(
                  masked.bytes[masked.record_byte_stride]) == 0x21);
        CHECK(std::all_of(
            masked.bytes.begin() + masked.record_byte_stride +
                masked.data_start,
            masked.bytes.begin() + masked.record_byte_stride * 2,
            [](std::byte value) { return value == std::byte{0}; }));
    }
    SECTION("record stride beyond the device fact is named") {
        CHECK_THROWS_WITH(
            packRayTracingHitTable(
                layout, {.shader_group_handle_size = handle_size,
                         .shader_group_handle_alignment = 4,
                         .max_shader_group_stride = 19,
                         .group_handles = handles,
                         .rows = rows.rows}),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.shader_group_stride_limit@1"));
    }
    SECTION("record byte extent overflow is named before allocation") {
        CHECK_THROWS_WITH(
            packRayTracingHitTable(
                layout,
                {.shader_group_handle_size =
                     std::numeric_limits<std::uint32_t>::max(),
                 .shader_group_handle_alignment = 1,
                 .max_shader_group_stride =
                     std::numeric_limits<std::uint32_t>::max()}),
            Catch::Matchers::ContainsSubstring(
                "pelican.sbt.layout_overflow@1"));
    }
    SECTION("all padding starts as zero") {
        const auto empty_layout = parseRayGroupLayout(R"json({
          "schema":"pelican.ray_group_layout","version":1,
          "ray_types":["primary"],"default_group":"opaque",
          "groups":[{"name":"opaque"}],"misses":[{"name":"sky"}],"record":[]
        })json");
        const std::array one_row{RaySceneRow{.q = 0, .group_index = 0}};
        const std::array<std::byte, 3> short_handle{
            std::byte{1}, std::byte{2}, std::byte{3}};
        const auto padded = packRayTracingHitTable(
            empty_layout, {.shader_group_handle_size = 3,
                           .shader_group_handle_alignment = 4,
                           .max_shader_group_stride = 32,
                           .group_handles = short_handle,
                           .rows = one_row});
        CHECK((padded.bytes == std::vector<std::byte>{
            std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0}}));
    }
}

TEST_CASE("WP366 SPIR-V trace operands are validated in layout domains",
          "[ray-tracing][rt1b][addressing][mutation]") {
    const auto layout = parseRayGroupLayout(layoutJson);
    const RayTracingRecordAddressing addressing{
        .record_index_stride = 2,
        .traced_ray_type_indices = {0}};
    CHECK_NOTHROW(validateRayTracingRecordAddressing(0, 2, 1, layout, addressing));
    SECTION("one-geometry scenes still reject zero and one stride") {
        for (const auto wrong : {0u, 1u}) {
            CHECK_THROWS_WITH(
                validateRayTracingRecordAddressing(0, wrong, 1, layout, addressing),
                Catch::Matchers::ContainsSubstring(
                    "pelican.ray_tracing.record_index_stride_invalid@1"));
        }
    }
    SECTION("untraced ray-type slots are unreachable") {
        CHECK_THROWS_WITH(
            validateRayTracingRecordAddressing(1, 2, 1, layout, addressing),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.ray_type_offset_invalid@1"));
    }
    SECTION("miss is outside the hit grid but layout-bounded") {
        CHECK_THROWS_WITH(
            validateRayTracingRecordAddressing(0, 2, 2, layout, addressing),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.miss_index_invalid@1"));
    }
    SECTION("q times R observes the Vulkan 24-bit field") {
        CHECK(rayTracingInstanceRecordOffset((1u << 23u) - 1u, 2) ==
              (1u << 24u) - 2u);
        CHECK_THROWS_WITH(
            rayTracingInstanceRecordOffset(1u << 23u, 2),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_row_count_limit@1"));
    }
}

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("WP366 final SPIR-V shader-record ABI is a load-time invariant",
          "[ray-tracing][rt1b][spirv][abi][production][mutation]") {
    ShaderCompiler compiler;
    const auto compile = [&](std::string_view source,
                             vk::ShaderStageFlagBits stage,
                             std::string_view name) {
        const auto result = compiler.compileSource(source, stage, name);
        INFO(result.log);
        REQUIRE(result.ok);
        return inspectRayTracingSpirv(result.spirv, name);
    };
    auto raygen = compile(R"glsl(
      #version 460
      #extension GL_EXT_ray_tracing : require
      layout(set=0,binding=6) uniform accelerationStructureEXT frame_tlas;
      layout(location=0) rayPayloadEXT vec4 payload;
      void main() {
        traceRayEXT(frame_tlas, gl_RayFlagsOpaqueEXT, 0xffu, 0u, 2u, 0u,
                    vec3(0.0), 0.001, vec3(0.0,0.0,1.0), 100.0, 0);
      }
    )glsl", vk::ShaderStageFlagBits::eRaygenKHR, "abi.rgen");
    auto miss0 = compile(R"glsl(
      #version 460
      #extension GL_EXT_ray_tracing : require
      layout(location=0) rayPayloadInEXT vec4 payload;
      void main() { payload = vec4(0.0); }
    )glsl", vk::ShaderStageFlagBits::eMissKHR, "abi0.rmiss");
    auto miss1 = compile(R"glsl(
      #version 460
      #extension GL_EXT_ray_tracing : require
      layout(location=0) rayPayloadInEXT vec4 payload;
      void main() { payload = vec4(1.0); }
    )glsl", vk::ShaderStageFlagBits::eMissKHR, "abi1.rmiss");
    auto hit = compile(R"glsl(
      #version 460
      #extension GL_EXT_ray_tracing : require
      layout(location=0) rayPayloadInEXT vec4 payload;
      hitAttributeEXT vec2 attributes;
      layout(shaderRecordEXT, std430) buffer Record {
        vec3 tint;
        float flags;
      } record_data;
      void main() { payload = vec4(record_data.tint, record_data.flags); }
    )glsl", vk::ShaderStageFlagBits::eClosestHitKHR, "abi.rchit");
    auto noop = compile(R"glsl(
      #version 460
      #extension GL_EXT_ray_tracing : require
      void main() {}
    )glsl", vk::ShaderStageFlagBits::eClosestHitKHR, "noop.rchit");

    const auto occurrence = [](
        const RayTracingModuleReflection &module, ShaderStage stage,
        std::uint32_t group, std::string_view uri) {
        return RayTracingShaderOccurrence{
            .stage = stage,
            .group_index = group,
            .diagnostic_uri = uri,
            .entry = &selectRayTracingEntryPoint(module, stage, "main", uri)};
    };
    const auto raygen_occurrence =
        occurrence(raygen, ShaderStage::raygen, 0, "abi.rgen");
    const std::array misses{
        occurrence(miss0, ShaderStage::miss, 1, "abi0.rmiss"),
        occurrence(miss1, ShaderStage::miss, 2, "abi1.rmiss")};
    std::array hits{
        occurrence(hit, ShaderStage::closesthit, 3, "abi.rchit"),
        occurrence(noop, ShaderStage::closesthit, 4, "noop.rchit"),
        occurrence(hit, ShaderStage::closesthit, 5, "abi.rchit"),
        occurrence(noop, ShaderStage::closesthit, 6, "noop.rchit")};
    const auto layout = parseRayGroupLayout(layoutJson);
    const std::array<std::uint32_t, 1> traced{0};
    const RayTracingRuntimeFacts facts{
        .max_ray_recursion_depth = 1,
        .max_shader_group_stride = 4096,
        .max_ray_hit_attribute_size = 32,
        .max_ray_dispatch_invocation_count = 1,
        .max_dispatch_dimensions = {1, 1, 1},
        .device_name = "fixture"};
    const auto request = [&] {
        return RayTracingPipelineInterfaceRequest{
            .raygen = raygen_occurrence,
            .misses = misses,
            .hit_groups = hits,
            .group_layout = &layout,
            .traced_ray_type_indices = traced};
    };
    CHECK(validateRayTracingPipelineInterfaces(request(), facts)
              .starts_with("rt1b-fnv1a64:"));

    SECTION("changing only the shader declaration is named reject") {
        auto wrong = compile(R"glsl(
          #version 460
          #extension GL_EXT_ray_tracing : require
          layout(location=0) rayPayloadInEXT vec4 payload;
          hitAttributeEXT vec2 attributes;
          layout(shaderRecordEXT, std430) buffer Record {
            vec4 tint;
            float flags;
          } record_data;
          void main() { payload = record_data.tint; }
        )glsl", vk::ShaderStageFlagBits::eClosestHitKHR, "wrong.rchit");
        hits[0] = occurrence(wrong, ShaderStage::closesthit, 3,
                             "wrong.rchit");
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(request(), facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.shader_record_abi_mismatch@1"));
    }
}
#endif

} // namespace
} // namespace Pelican
