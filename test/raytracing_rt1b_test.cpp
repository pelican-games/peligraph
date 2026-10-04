#include "../src/core/shader/raygrouplayout.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderpipelinegpuarena.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/shader/raytracingrefusal.hpp"
#include "../src/core/shader/raytracingvalidation.hpp"
#include "../src/core/shader/raytracingoracle.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/vkcore/renderer_config.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <functional>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    std::size_t publication_notifications = 0;
    std::size_t rollback_notifications = 0;
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
            },
            [&, generation](const RendererRuntimeGeneration &published) {
                CHECK(published.generation == generation);
                ++publication_notifications;
            },
            [&] { ++rollback_notifications; });
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
    CHECK(publication_notifications == 0);
    CHECK(rollback_notifications == 1);

    const auto committed = stage();
    runtime.prepareStagedGenerationCommit(committed);
    CHECK(runtime.snapshot() == nullptr);
    CHECK(coordinated_commits == 1);
    REQUIRE(runtime.publishStagedGeneration(committed));
    REQUIRE(runtime.snapshot() != nullptr);
    CHECK(runtime.snapshot()->generation == committed);
    CHECK(runtime.stagedGeneration() == 0);
    CHECK(lifetime_commits == 1);
    CHECK(publication_notifications == 1);
    CHECK(rollback_notifications == 1);
}
namespace {

using Catch::Matchers::ContainsSubstring;

constexpr std::string_view layoutJson = R"json({
  "schema": "pelican.ray_group_layout",
  "version": 3,
  "ray_types": ["primary", "shadow"],
  "default_group": "opaque",
  "groups": [{"name":"opaque","type":"triangles"},{"name":"foliage","type":"triangles"}],
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
      "schema":"pelican.ray_bindings","version":2,"bindings":[
        {"material":{"document":"materials/leaf.mat.json"},
         "group":"foliage","record":{"tint":[0.2,0.8,0.4],"flags":7.0}},
        {"material":{"asset":"models/tree.glb","source_material_index":2},
         "group":"opaque","record":{"tint":[0.3,0.7,0.5],"flags":9.0}}
      ]
    })json", layout, "bindings.json");
}

nlohmann::json rt1bSidecarRoot() {
    // MG-2b: the sidecar references sit in the graph root's section.
    return Pelican::TestSupport::renderGraphRoot(nlohmann::json{
        {"ray_group_layout", "layout.json"},
        {"ray_bindings", "bindings.json"},
    });
}

std::shared_ptr<const RayTracingProjectConfig> loadRt1bSidecar(
    bool runtime_shader_compiler_enabled,
    std::size_t *load_count = nullptr) {
    return loadRayTracingProjectConfig(
        rt1bSidecarRoot(),
        [load_count](std::string_view reference) {
            if (load_count != nullptr) {
                ++*load_count;
            }
            if (reference == "layout.json") {
                return std::string{layoutJson};
            }
            if (reference == "bindings.json") {
                return std::string{R"json({
                  "schema":"pelican.ray_bindings","version":2,
                  "bindings":[]
                })json"};
            }
            throw std::runtime_error(
                "unexpected RT1b sidecar reference");
        },
        runtime_shader_compiler_enabled);
}

struct Wp367TemporaryDirectory {
    std::filesystem::path path;

    Wp367TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("pelican_wp367_" + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch().count()));
        std::filesystem::create_directories(path / "shaders");
    }

    ~Wp367TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void writeWp367Text(const std::filesystem::path &path,
                    std::string_view text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error("cannot write WP367 fixture");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

nlohmann::json rt1bCompilerConfig(bool require_rt_capability) {
    nlohmann::json config{
        {"features", nlohmann::json::array()},
        {"render_targets", nlohmann::json::array()},
        {"rendering_passes",
         nlohmann::json::array({
             {
                 {"name", "main"},
                 {"passes",
                  nlohmann::json::array({
                      {
                          {"name", "scene"},
                          {"type", "material"},
                          {"output",
                           {{"color", nlohmann::json::array({"swapchain"})},
                            {"depth", nullptr}}},
                      },
                  })},
             },
         })},
    };
    if (require_rt_capability) {
        config["target_planning"] = {
            {"graphs",
             {{"main",
               {{"required_capabilities",
                 {"pelican.vulkan.ray_query@1",
                  "pelican.vulkan.ray_tracing_pipeline@1"}}}}}},
        };
    }
    return Pelican::TestSupport::renderGraphRoot(std::move(config));
}

struct Rt1bCompilerFixture {
    nlohmann::json config;
    PathResolver path_resolver;
    GraphTransformRegistrySnapshot graph_transforms;
    RenderStrategyRegistrySnapshot render_strategies;
    SubgraphReplacementRegistrySnapshot subgraph_replacements;
    VulkanRenderCompilerBackendContext backend{
        vk::Format::eB8G8R8A8Srgb,
        vk::Extent2D{64, 64}, {}, {}, {},
        VulkanRenderCompilerDevicePlanningMode::compiler_only};
    std::vector<RenderCompilerProgramVariantRequest> variants;

    RenderCompilerProgramInput input(
        std::shared_ptr<const RayTracingProjectConfig> project,
        bool runtime_shader_compiler_enabled) const {
        return {
            .rendering_config = config,
            .source_name = "WP366a RT1b contrast",
            .path_resolver = path_resolver,
            .runtime_shader_compiler_enabled =
                runtime_shader_compiler_enabled,
            .ray_tracing_project = std::move(project),
            .graph_transforms = graph_transforms,
            .render_strategies = render_strategies,
            .subgraph_replacements = subgraph_replacements,
            .backend_context = backend,
            .variants = variants,
        };
    }
};

TEST_CASE("WP367 typed triangle hit groups expose only optional anyhit",
          "[WP367][ray-tracing][rt2a][grammar][production][mutation]") {
    const auto project = loadRt1bSidecar(true);
    auto config = nlohmann::json::parse(R"json({
      "compute_tasks":[{
        "name":"trace",
        "ray_tracing":{
          "raygen":"shaders/trace",
          "traced_ray_types":["primary"],
          "misses":{"sky":"shaders/sky","shadow_open":"shaders/open"},
          "hit_shaders":{"primary":{
            "opaque":{"closesthit":"shaders/opaque","anyhit":"shaders/filter"},
            "foliage":{"closesthit":"shaders/foliage"}
          }}
        },
        "writes":["mask"],
        "resource_ports":{"mask":{"resource":"mask","access":"storage"}},
        "dispatch":{"rays_from":{"port":"mask"}}
      }]
    })json");

    const auto parsed =
        parseComputeTaskDefinitionsFromConfigJson(config, project.get());
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed.front().ray_tracing);
    const auto &ray = *parsed.front().ray_tracing;
    REQUIRE(ray.any_hit_shader_slots.size() == 4);
    REQUIRE(ray.any_hit_shader_slots[0]);
    CHECK(ray.any_hit_shader_slots[0]->stage == ShaderStage::anyhit);
    CHECK(ray.any_hit_shader_slots[0]->ref == "shaders/filter");
    CHECK_FALSE(ray.any_hit_shader_slots[1]);
    CHECK_FALSE(ray.any_hit_shader_slots[2]);
    CHECK_FALSE(ray.any_hit_shader_slots[3]);
    CHECK(shaderSourceCandidateReference(
              ray.any_hit_shader_slots[0]->ref,
              ShaderSourceStage::anyhit, false) ==
          "shaders/filter.rahit");

    // MG-2a: the layout (version 3) owns the group type. A task that still
    // spells one -- the v1/v2 task grammar -- is refused by name, whatever
    // the value.
    SECTION("a task-declared group type is refused") {
        for (const auto *type : {"triangles", "procedural"}) {
            INFO("type " << type);
            auto invalid = config;
            invalid["compute_tasks"][0]["ray_tracing"]["hit_shaders"]
                   ["primary"]["opaque"]["type"] = type;
            CHECK_THROWS_WITH(
                parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
                ContainsSubstring(
                    "pelican.ray_group_type_declared_in_task@1"));
        }
    }
    SECTION("a triangles group cannot declare intersection") {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]["hit_shaders"]
               ["primary"]["opaque"]["intersection"] = "shaders/nope";
        CHECK_THROWS_WITH(
            parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
            ContainsSubstring(
                "pelican.ray_tracing.unsupported_shader_stage@1"));
    }
    SECTION("unknown group fields use the one closed-grammar error") {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]["hit_shaders"]
               ["primary"]["opaque"]["shader"] = "shaders/nope";
        CHECK_THROWS_WITH(
            parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
            ContainsSubstring(
                "pelican.ray_tracing.ray_hit_group_invalid@1"));
    }
    SECTION("an empty optional anyhit is invalid") {
        auto invalid = config;
        invalid["compute_tasks"][0]["ray_tracing"]["hit_shaders"]
               ["primary"]["opaque"]["anyhit"] = "";
        CHECK_THROWS_WITH(
            parseComputeTaskDefinitionsFromConfigJson(invalid, project.get()),
            ContainsSubstring(
                "pelican.ray_tracing.ray_hit_group_invalid@1"));
    }
    SECTION("WP367a legacy task keeps rahit as a closesthit stem suffix") {
        const auto fixture_path =
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
            "fixtures" / "wp367a" /
            "legacy_rahit_stem_task.json";
        std::ifstream input{fixture_path, std::ios::binary};
        REQUIRE(input.is_open());
        const auto legacy = nlohmann::json::parse(input);
        const auto tasks =
            parseComputeTaskDefinitionsFromConfigJson(legacy);
        REQUIRE(tasks.size() == 1);
        REQUIRE(tasks.front().ray_tracing);
        REQUIRE(tasks.front().ray_tracing->closest_hits.size() == 1);
        const auto &closest =
            tasks.front().ray_tracing->closest_hits.front();
        CHECK(closest.ref == "shaders/foo.rahit");
        CHECK(shaderSourceCandidateReference(
                  closest.ref, ShaderSourceStage::closesthit, false) ==
              "shaders/foo.rahit.rchit");
        CHECK_THROWS_WITH(
            makeShaderReference(
                "shaders/foo.rahit", ShaderStage::anyhit),
            ContainsSubstring("explicit file extension"));
    }
}

TEST_CASE("WP367 integer predicate generator drives both CPU oracle lanes",
          "[WP367][ray-tracing][rt2a][oracle][mutation]") {
    auto front = makeConstructiveRayTracingOracleFixture(
        RayTracingSentinelKind::hit, 0xA0u, 8.0f);
    auto back = makeConstructiveRayTracingOracleFixture(
        RayTracingSentinelKind::hit, 0xB0u, 16.0f);
    front.triangles.front().primitive_id = 3u;
    front.triangles.front().instance_custom_index = 41u;
    front.triangles.front().record_u32 = 7u;
    back.triangles.front().primitive_id = 5u;
    back.triangles.front().instance_custom_index = 42u;
    back.triangles.front().record_u32 = 9u;
    std::vector triangles{
        front.triangles.front(), back.triangles.front()};

    const auto always =
        RayTracingAnyHitIntegerPredicate::alwaysAccept();
    const auto actual =
        RayTracingAnyHitIntegerPredicate::ignoreMatching({
            .primitive_id = 3u,
            .instance_custom_index = 41u,
            .record_u32 = 7u,
        });
    const auto always_result =
        traceRayTracingOracle(front.ray, triangles, always);
    const auto actual_result =
        traceRayTracingOracle(front.ray, triangles, actual);
    REQUIRE(always_result.hit);
    REQUIRE(actual_result.hit);
    CHECK(always_result.hit->record_id == 0xA0u);
    CHECK(actual_result.hit->record_id == 0xB0u);
    CHECK(actual.accepts({5u, 42u, 9u}));
    CHECK_FALSE(actual.accepts({3u, 41u, 7u}));

    const auto matching =
        RayTracingAnyHitIntegerPredicate::acceptMatching({3u, 41u, 7u});
    CHECK(matching.accepts({3u, 41u, 7u}));
    SECTION("each integer input independently controls the CPU evaluator") {
        CHECK_FALSE(matching.accepts({4u, 41u, 7u}));
        CHECK_FALSE(matching.accepts({3u, 42u, 7u}));
        CHECK_FALSE(matching.accepts({3u, 41u, 8u}));
    }
    SECTION("the immutable IR carries all three input leaves") {
        std::array<bool, 3> present{};
        for (const auto &node : matching.ir()) {
            if (node.opcode !=
                RayTracingAnyHitIntegerPredicateOpcode::input) {
                continue;
            }
            present[static_cast<std::size_t>(node.input)] = true;
        }
        CHECK(present == std::array{true, true, true});
    }
}

TEST_CASE("WP367 compiler contrast rejects layout before shader documents",
          "[WP367][ray-tracing][rt2a][matrix][compiler][production][mutation]") {
    std::size_t shader_document_loads = 0;
    CHECK_THROWS_WITH(
        loadRayTracingProjectConfig(
            rt1bSidecarRoot(),
            [&](std::string_view) {
                ++shader_document_loads;
                return std::string{};
            },
            false),
        ContainsSubstring(
            "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
    CHECK(shader_document_loads == 0);
}

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("WP367 canonical anyhit resolves compiles and selects final SPIR-V stage",
          "[WP367][ray-tracing][rt2a][resolver][shaderc][spirv][production][mutation]") {
    Wp367TemporaryDirectory temporary;
    const auto predicate =
        RayTracingAnyHitIntegerPredicate::ignoreMatching({
            .primitive_id = 2u,
            .instance_custom_index = 19u,
            .record_u32 = 367u,
        });
    const auto source =
        std::string{"#version 460\n"
                    "#extension GL_EXT_ray_tracing : require\n"
                    "layout(location=0) rayPayloadInEXT uvec4 payload;\n"
                    "hitAttributeEXT vec2 attributes;\n"
                    "layout(shaderRecordEXT, std430) buffer Record { uint marker; } record_data;\n"
                    "void main() { if (!("} +
        predicate.glslAcceptExpression("record_data.marker") +
        ")) { ignoreIntersectionEXT; } }\n";
    const auto path = temporary.path / "shaders" / "filter.rahit";
    writeWp367Text(path, source);

    PathResolver resolver;
    resolver.setup(temporary.path, false);
    const auto opened = resolver.resolveShaderSourceOpenReference(
        "shaders/filter", ShaderSourceStage::anyhit);
    REQUIRE(opened.openable());
    CHECK(opened.logical_ref ==
          std::optional<std::string>{"project://shaders/filter.rahit"});
    CHECK(opened.physical_path == std::optional<std::filesystem::path>{path});
    REQUIRE(inferShaderStageFromPath(path));
    CHECK(*inferShaderStageFromPath(path) ==
          vk::ShaderStageFlagBits::eAnyHitKHR);

    ShaderCompiler compiler;
    const auto compiled = compiler.compileFile(path);
    INFO(compiled.log);
    REQUIRE(compiled.ok);
    ShaderLibrary library{ShaderLibraryModuleMode::reflection_only};
    const auto id = library.loadFromFile(path);
    const auto &bundle = library.get(id);
    const auto &entry = selectRayTracingEntryPoint(
        bundle.reflection.ray_tracing_module, ShaderStage::anyhit,
        "main", bundle.diagnostic_uri);
    CHECK(entry.key.execution_model ==
          spirvExecutionModelForShaderStage(ShaderStage::anyhit));
    CHECK_THROWS_WITH(
        library.loadFromSpirv(compiled.spirv, ShaderStage::closesthit,
                              "wrong-final-stage.rchit"),
        ContainsSubstring(
            "pelican.ray_tracing.interface_type_unsupported@1"));
}
#endif

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
    CHECK_THAT(include, ContainsSubstring(
                            "const uint pelican_ray_type_count = 2u;"));
    CHECK_THAT(include, ContainsSubstring("PELICAN_MISS_sky 0"));
    CHECK_THAT(include, ContainsSubstring(layout.digest_sha256));
    CHECK(include.find("GROUP_") == std::string::npos);
    CHECK(include.find("ROW_") == std::string::npos);

    // MG-2a: the digest is of the parsed document (v1 alone hashed raw
    // bytes), so whitespace leaves it and the generated source unchanged.
    const auto whitespace = parseRayGroupLayout(
        std::string{layoutJson} + "\n", "layout.json");
    CHECK(whitespace.digest_sha256 == layout.digest_sha256);
    CHECK(makeRayGroupLayoutVirtualInclude(whitespace) == include);

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
    // MG-2a: spelling is not part of the layout digest (v1 alone hashed its
    // raw bytes), so the same include is a cache hit.
    CHECK(whitespace.cache_hit);
    CHECK(whitespace.cache_key == base.cache_key);
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
    const auto root = Pelican::TestSupport::renderGraphRoot(nlohmann::json{
        {"ray_group_layout", "layout.json"},
        {"ray_bindings", "bindings.json"}});
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
              "schema":"pelican.ray_bindings","version":2,"bindings":[]
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

// RTL-1b: the task parser's ray tracing refusals carry their code and the
// fields known where they are raised (RayTracingRefusal); what() is the
// sentence it always was.
TEST_CASE("RTL-1b the ray tracing task parser refuses by a typed code with its fields and the same message",
          "[ray-tracing][rt1b][rtl1b][rejection]") {
    const auto root = Pelican::TestSupport::renderGraphRoot(nlohmann::json{{"ray_group_layout", "layout.json"}});
    const auto project = loadRayTracingProjectConfig(
        root, [](std::string_view) { return std::string{layoutJson}; }, true);
    REQUIRE(project);
    const auto config = nlohmann::json::parse(R"json({
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
    REQUIRE_NOTHROW(parseComputeTaskDefinitionsFromConfigJson(config, project.get()));
    struct Refused {
        std::string code;
        nlohmann::json fields;
        std::string message;
    };
    const auto refuse = [&](const std::function<void(nlohmann::json &)> &change,
                            const RayTracingProjectConfig *layout) {
        auto invalid = config;
        change(invalid["compute_tasks"][0]["ray_tracing"]);
        try {
            (void)parseComputeTaskDefinitionsFromConfigJson(invalid, layout);
        } catch (const RayTracingRefusal &refusal) {
            return Refused{refusal.code(), refusal.fields(), refusal.what()};
        }
        FAIL("the task was not refused");
        return Refused{};
    };
    const auto with_layout = [&](const std::function<void(nlohmann::json &)> &change) {
        return refuse(change, project.get());
    };

    {
        const auto refused = with_layout([](auto &rt) { rt["hit_shaders"]["primary"].erase("foliage"); });
        CHECK(refused.code == "pelican.ray_tracing.hit_shader_missing@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}, {"ray_type", "primary"}, {"group", "foliage"}});
        CHECK(refused.message ==
              "pelican.ray_tracing.hit_shader_missing@1: compute task 'trace' group='foliage' ray_type='primary' "
              "available=[opaque]");
    }
    {
        const auto refused = with_layout([](auto &rt) { rt["misses"]["skyy"] = "shaders/sky"; });
        CHECK(refused.code == "pelican.ray_tracing.miss_shader_unknown@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}, {"miss", "skyy"}});
        CHECK(refused.message == "pelican.ray_tracing.miss_shader_unknown@1: compute task 'trace' miss='skyy'");
    }
    {
        const auto refused = with_layout([](auto &rt) { rt["misses"].erase("shadow_open"); });
        CHECK(refused.code == "pelican.ray_tracing.miss_shader_missing@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}, {"miss", "shadow_open"}});
    }
    {
        const auto refused = with_layout([](auto &rt) { rt["hit_shaders"]["primary"]["bark"] = {{"closesthit", "x"}}; });
        CHECK(refused.code == "pelican.ray_tracing.hit_shader_unknown_group@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}, {"ray_type", "primary"}, {"group", "bark"}});
    }
    {
        const auto refused = with_layout([](auto &rt) { rt["hit_shaders"]["shadow"] = nlohmann::json::object(); });
        CHECK(refused.code == "pelican.ray_tracing.hit_shader_untraced@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}, {"ray_type", "shadow"}});
    }
    {
        const auto refused = with_layout([](auto &rt) { rt["traced_ray_types"] = nlohmann::json::array(); });
        CHECK(refused.code == "pelican.ray_tracing.traced_ray_types_invalid@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}});
    }
    {
        // A ray type the layout does not have: the layout lookup's code, with
        // the task.
        const auto refused = with_layout([](auto &rt) { rt["traced_ray_types"] = {"primary", "glow"}; });
        CHECK(refused.code == "pelican.ray_group_name_unknown@1");
        CHECK(refused.fields == nlohmann::json{{"name", "glow"}, {"kind", "ray_type"}, {"task", "trace"}});
        CHECK(refused.message == "pelican.ray_group_name_unknown@1: ray type 'glow'");
    }
    {
        const auto refused = with_layout([](auto &rt) {
            rt["hit_shaders"]["primary"]["opaque"]["intersection"] = "shaders/box";
        });
        CHECK(refused.code == "pelican.ray_tracing.unsupported_shader_stage@1");
        CHECK(refused.fields ==
              nlohmann::json{{"task", "trace"}, {"ray_type", "primary"}, {"group", "opaque"}, {"stage", "intersection"}});
    }
    {
        const auto refused = with_layout([](auto &rt) {
            rt["miss"] = "shaders/legacy";
            rt["closesthit"] = "shaders/legacy";
        });
        CHECK(refused.code == "pelican.ray_tracing.legacy_syntax_with_layout@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}, {"keys", {"closesthit", "miss"}}});
        CHECK(refused.message ==
              "pelican.ray_tracing.legacy_syntax_with_layout@1: compute task 'trace' must use "
              "traced_ray_types/misses/hit_shaders");
    }
    {
        // Without a layout: the stage the form without one does not take.
        const auto refused = refuse([](auto &rt) {
            rt = {{"raygen", "shaders/trace"}, {"miss", "shaders/m"}, {"closesthit", "shaders/c"},
                  {"anyhit", "shaders/a"}};
        }, nullptr);
        CHECK(refused.code == "pelican.ray_tracing.unsupported_shader_stage@1");
        CHECK(refused.fields == nlohmann::json{{"task", "trace"}, {"stage", "anyhit"}});
    }
    {
        // The layout's own name lookup without a task: the name and its kind.
        try {
            (void)project->layout.groupIndex("bark");
            FAIL("an unknown group was found");
        } catch (const RayTracingRefusal &refusal) {
            CHECK(refusal.code() == "pelican.ray_group_name_unknown@1");
            CHECK(refusal.fields() == nlohmann::json{{"name", "bark"}, {"kind", "group"}});
        }
    }
}

TEST_CASE("WP366a compiler contrast executes the real root-sidecar gate",
          "[ray-tracing][rt1b][matrix][compiler][production][mutation]") {
    std::size_t loads = 0;
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto project = loadRt1bSidecar(
        PELICAN_RUNTIME_SHADER_COMPILER != 0, &loads);
    REQUIRE(project != nullptr);
    CHECK(loads == 2);
    CHECK(project->layout.source_reference == "layout.json");
    REQUIRE(project->bindings.has_value());
    CHECK(project->bindings->source_reference == "bindings.json");
#else
    CHECK_THROWS_WITH(
        loadRt1bSidecar(
            PELICAN_RUNTIME_SHADER_COMPILER != 0, &loads),
        Catch::Matchers::ContainsSubstring(
            "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
    CHECK(loads == 0);
#endif
}

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("WP366a preview production compiler retains RT1b sidecar provenance",
          "[ray-tracing][rt1b][matrix][preview][production][mutation]") {
    Rt1bCompilerFixture fixture;
    fixture.config = rt1bCompilerConfig(false);
    fixture.variants = {
        RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::preview,
            .artifact = RenderCompilerProgramArtifact::data_only,
        },
    };
    const auto sidecar = loadRt1bSidecar(true);
    const auto output = runRenderCompilerProgram(
        defaultVulkanRenderCompilerProgram(),
        fixture.input(sidecar, true));
    REQUIRE(output.variants.size() == 1);
    const auto &preview = output.variants.front();
    REQUIRE(preview.compiled_pipeline != nullptr);
    CHECK(preview.graph_variant ==
          RenderPipelineGraphVariant::preview);
    CHECK(preview.ray_tracing_project == sidecar);
    CHECK(preview.compiled_pipeline->ray_group_layout_reference ==
          std::optional<std::string>{"layout.json"});
    CHECK(preview.compiled_pipeline->ray_bindings_reference ==
          std::optional<std::string>{"bindings.json"});
}
#endif

TEST_CASE("WP366a OpenXR variant owner retains every pipeline SBT",
          "[ray-tracing][rt1b][matrix][openxr][owner][production][mutation]") {
#if PELICAN_WITH_OPENXR
    const auto flat_owner = renderPipelineVariantGpuOwnerScope(
        true, RenderPipelineGraphVariant::flat);
    const auto xr_owner = renderPipelineVariantGpuOwnerScope(
        true, RenderPipelineGraphVariant::xr);
    REQUIRE(flat_owner == "render_pipeline/variants");
    REQUIRE(xr_owner == flat_owner);

    RenderPipelineGpuScopePreparation prepared{
        .owner_scope = flat_owner,
        .resources = {
            {RenderPipelineGpuResourceKind::pipeline, 41,
             "flat_pipeline_with_sbt", 0},
            {RenderPipelineGpuResourceKind::pipeline, 42,
             "xr_pipeline_with_sbt", 0},
        },
    };
    const auto arena = compileRenderPipelineGpuArena(
        {}, 366, std::move(prepared));
    REQUIRE(arena != nullptr);
    const auto *scope = arena->findScope(flat_owner);
    REQUIRE(scope != nullptr);
    REQUIRE(scope->resources.size() == 2);
    CHECK(scope->resources[0].name ==
          "flat_pipeline_with_sbt");
    CHECK(scope->resources[1].name ==
          "xr_pipeline_with_sbt");
#else
    CHECK(renderPipelineVariantGpuOwnerScope(
              false, RenderPipelineGraphVariant::flat) ==
          "render_pipeline/flat");
    CHECK_THROWS_WITH(
        renderPipelineVariantGpuOwnerScope(
            true, RenderPipelineGraphVariant::xr),
        Catch::Matchers::ContainsSubstring(
            "XR graph variant is unavailable in this build"));
#endif
}

TEST_CASE("WP366a production compiler rejects an RT1b plan without device capability",
          "[ray-tracing][rt1b][matrix][capability][production][mutation]") {
    Rt1bCompilerFixture fixture;
    fixture.config = rt1bCompilerConfig(true);
    fixture.variants = {
        RenderCompilerProgramVariantRequest{
            .graph_variant = RenderPipelineGraphVariant::flat,
            .artifact = RenderCompilerProgramArtifact::runtime_package,
        },
    };
    std::shared_ptr<const RayTracingProjectConfig> sidecar;
#if PELICAN_RUNTIME_SHADER_COMPILER
    sidecar = loadRt1bSidecar(true);
#endif
    CHECK_THROWS_WITH(
        runRenderCompilerProgram(
            defaultVulkanRenderCompilerProgram(),
            fixture.input(
                std::move(sidecar),
                PELICAN_RUNTIME_SHADER_COMPILER != 0)),
        Catch::Matchers::ContainsSubstring(
            "pelican.plan.ray_query_required_unavailable@1"));
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
          "schema":"pelican.ray_bindings","version":2,"bindings":[
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
          "schema":"pelican.ray_bindings","version":2,"bindings":[
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
                         .geometry_match_identity = "allocation/1@transform/B",
                         .material_asset = "models/tree.glb", .source_material_index = 2,
                         .source_index = 41},
        RaySceneRowInput{.scene_identity = "TreeA/0", .geometry_identity = "mesh/0",
                         .geometry_match_identity = "allocation/2@transform/A",
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
    // RTL-1a: an authored selector no row matches is listed, not refused.
    SECTION("an unresolved authored selector is listed and its row set still resolves") {
        std::array only_leaf{input[1]};
        const auto rows = resolveRaySceneRows(only_leaf, layout, &bindings);
        REQUIRE(rows.rows.size() == 1);
        CHECK(rows.rows[0].group_index == layout.groupIndex("foliage"));
        CHECK(rows.bindings_digest_sha256 == bindings.digest_sha256);
        REQUIRE(rows.unresolved_binding_indices.size() == 1);
        CHECK(bindings.bindings.at(rows.unresolved_binding_indices[0]).selector.reference ==
              "models/tree.glb");
        CHECK(forward.unresolved_binding_indices.empty());
    }
    SECTION("one geometric row cannot receive divergent bindings") {
        auto ambiguous = input;
        ambiguous[1].geometry_match_identity =
            ambiguous[0].geometry_match_identity;
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
        classifyRayQueryGeometry(forward_inputs);
    const auto reverse_classification =
        classifyRayQueryGeometry(reverse_inputs);
    const auto forward_tlas = lowerRayTracingTlasInstancePlan(
        forward_inputs, forward_classification.eligible_instance_indices,
        &forward_rows, 2);
    const auto reverse_tlas = lowerRayTracingTlasInstancePlan(
        reverse_inputs, reverse_classification.eligible_instance_indices,
        &reverse_rows, 2);
    REQUIRE(forward_tlas.size() == 2);
    REQUIRE(reverse_tlas.size() == 2);
    for (std::size_t q = 0; q < forward_tlas.size(); ++q) {
        const auto &left = forward_tlas[q];
        const auto &right = reverse_tlas[q];
        CHECK(left.q == q);
        // MG-2a: v1 wrote q here; a version 3 row takes the layout default.
        CHECK(left.instance_custom_index ==
              rayAccelerationStructureCanonicalDefaults.custom_index);
        CHECK(left.instance_sbt_record_offset == q * 2);
        CHECK(right.q == left.q);
        CHECK(right.instance_custom_index == left.instance_custom_index);
        CHECK(right.instance_sbt_record_offset ==
              left.instance_sbt_record_offset);
        CHECK(forward_inputs[left.source_index].world_transform ==
              reverse_inputs[right.source_index].world_transform);
    }

    SECTION("legacy null-layout bytes number only included instances") {
        auto legacy = forward_inputs;
        legacy[0].index_count = 0;
        const auto classification =
            classifyRayQueryGeometry(legacy);
        REQUIRE(classification.eligible_instance_indices ==
                std::vector<std::size_t>{1});
        const auto planned = lowerRayTracingTlasInstancePlan(
            legacy, classification.eligible_instance_indices,
            nullptr, 0);
        REQUIRE(planned.size() == 1);
        CHECK(planned[0].source_index == 1);
        CHECK(planned[0].instance_custom_index == 0);
        const vk::AccelerationStructureInstanceKHR actual{
            vk::TransformMatrixKHR{},
            planned[0].instance_custom_index,
            0xffu,
            planned[0].instance_sbt_record_offset,
            vk::GeometryInstanceFlagsKHR{},
            0x366aull};
        const vk::AccelerationStructureInstanceKHR parent_bytes{
            vk::TransformMatrixKHR{}, 0u, 0xffu, 0u,
            vk::GeometryInstanceFlagsKHR{}, 0x366aull};
        CHECK(std::memcmp(&actual, &parent_bytes,
                          sizeof(actual)) == 0);
    }

    SECTION("geometrically coincident production rows reject divergent material bindings") {
        auto ambiguous = forward_inputs;
        ambiguous[1].geometry_allocation_id =
            ambiguous[0].geometry_allocation_id;
        ambiguous[1].index_offset = ambiguous[0].index_offset;
        ambiguous[1].vertex_offset = ambiguous[0].vertex_offset;
        ambiguous[1].material_asset_reference =
            "models/tree-alias.glb";
        ambiguous[1].mesh_index =
            ambiguous[0].mesh_index + 7;
        ambiguous[1].primitive_index =
            ambiguous[0].primitive_index + 3;
        ambiguous[1].node_index =
            ambiguous[0].node_index + 5;
        ambiguous[1].world_transform = ambiguous[0].world_transform;
        project.bindings = parseRayBindings(R"json({
          "schema":"pelican.ray_bindings","version":2,"bindings":[
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
                         .geometry_match_identity = "allocation/1@transform/B",
                         .material_asset = "models/tree.glb", .source_material_index = 2,
                         .source_index = 1},
        RaySceneRowInput{.scene_identity = "TreeA/0", .geometry_identity = "mesh/0",
                         .geometry_match_identity = "allocation/2@transform/A",
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
          "schema":"pelican.ray_group_layout","version":3,
          "ray_types":["primary"],"default_group":"opaque",
          "groups":[{"name":"opaque","type":"triangles"}],"misses":[{"name":"sky"}],"record":[]
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
TEST_CASE("WP366/WP367 final SPIR-V hit-pair ABI is a load-time invariant",
          "[WP367][ray-tracing][rt1b][rt2a][spirv][abi][production][mutation]") {
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
      layout(location=0) rayPayloadInEXT vec4 payload;
      hitAttributeEXT vec2 attributes;
      void main() {}
    )glsl", vk::ShaderStageFlagBits::eClosestHitKHR, "noop.rchit");
    auto anyhit = compile(R"glsl(
      #version 460
      #extension GL_EXT_ray_tracing : require
      layout(location=0) rayPayloadInEXT vec4 payload;
      hitAttributeEXT vec2 attributes;
      layout(shaderRecordEXT, std430) buffer Record {
        vec3 tint;
        float flags;
      } record_data;
      void main() { if (record_data.flags == 999.0) { ignoreIntersectionEXT; } }
    )glsl", vk::ShaderStageFlagBits::eAnyHitKHR, "abi.rahit");

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
    std::array paired_hits{
        RayTracingHitGroupOccurrence{
            .closest_hit = hits[0],
            .any_hit = occurrence(anyhit, ShaderStage::anyhit, 3,
                                  "abi.rahit")},
        RayTracingHitGroupOccurrence{.closest_hit = hits[1]},
        RayTracingHitGroupOccurrence{.closest_hit = hits[2]},
        RayTracingHitGroupOccurrence{.closest_hit = hits[3]},
    };
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
    const auto legacy_digest =
        validateRayTracingPipelineInterfaces(request(), facts);
    CHECK(legacy_digest.starts_with("rt1b-fnv1a64:"));
    const std::array legacy_pairs{
        RayTracingHitGroupOccurrence{.closest_hit = hits[0]},
        RayTracingHitGroupOccurrence{.closest_hit = hits[1]},
        RayTracingHitGroupOccurrence{.closest_hit = hits[2]},
        RayTracingHitGroupOccurrence{.closest_hit = hits[3]},
    };
    CHECK(validateRayTracingPipelineInterfaces(
              {.raygen = raygen_occurrence,
               .misses = misses,
               .group_layout = &layout,
               .traced_ray_type_indices = traced,
               .hit_group_occurrences = legacy_pairs},
              facts) == legacy_digest);
    const auto paired_request = [&] {
        return RayTracingPipelineInterfaceRequest{
            .raygen = raygen_occurrence,
            .misses = misses,
            .group_layout = &layout,
            .traced_ray_type_indices = traced,
            .hit_group_occurrences = paired_hits};
    };
    CHECK(validateRayTracingPipelineInterfaces(paired_request(), facts)
              .starts_with("rt1b-fnv1a64:"));

    SECTION("WP367 anyhit payload mismatch is named") {
        auto wrong = compile(R"glsl(
          #version 460
          #extension GL_EXT_ray_tracing : require
          layout(location=0) rayPayloadInEXT uvec4 payload;
          hitAttributeEXT vec2 attributes;
          layout(shaderRecordEXT, std430) buffer Record {
            vec3 tint; float flags;
          } record_data;
          void main() {}
        )glsl", vk::ShaderStageFlagBits::eAnyHitKHR,
            "payload-mismatch.rahit");
        paired_hits[0].any_hit = occurrence(
            wrong, ShaderStage::anyhit, 3, "payload-mismatch.rahit");
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(paired_request(), facts),
            ContainsSubstring("pelican.ray_tracing.payload_mismatch@1"));
    }
    SECTION("WP367 an unreached physical ray slot still compares its hit pair") {
        auto wrong = compile(R"glsl(
          #version 460
          #extension GL_EXT_ray_tracing : require
          layout(location=0) rayPayloadInEXT uvec4 payload;
          hitAttributeEXT vec2 attributes;
          void main() {}
        )glsl", vk::ShaderStageFlagBits::eAnyHitKHR,
            "unreached-payload-mismatch.rahit");
        paired_hits[1].any_hit = occurrence(
            wrong, ShaderStage::anyhit, 4,
            "unreached-payload-mismatch.rahit");
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(paired_request(), facts),
            ContainsSubstring("pelican.ray_tracing.payload_mismatch@1"));
    }
    SECTION("WP367 anyhit vec3 triangle attribute is named") {
        auto wrong = compile(R"glsl(
          #version 460
          #extension GL_EXT_ray_tracing : require
          layout(location=0) rayPayloadInEXT vec4 payload;
          hitAttributeEXT vec3 attributes;
          layout(shaderRecordEXT, std430) buffer Record {
            vec3 tint; float flags;
          } record_data;
          void main() {}
        )glsl", vk::ShaderStageFlagBits::eAnyHitKHR,
            "attribute-mismatch.rahit");
        paired_hits[0].any_hit = occurrence(
            wrong, ShaderStage::anyhit, 3, "attribute-mismatch.rahit");
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(paired_request(), facts),
            ContainsSubstring(
                "pelican.ray_tracing.hit_attribute_forbidden@1"));
    }
    SECTION("WP367 anyhit record mismatch is named") {
        auto wrong = compile(R"glsl(
          #version 460
          #extension GL_EXT_ray_tracing : require
          layout(location=0) rayPayloadInEXT vec4 payload;
          hitAttributeEXT vec2 attributes;
          layout(shaderRecordEXT, std430) buffer Record {
            vec4 tint; float flags;
          } record_data;
          void main() {}
        )glsl", vk::ShaderStageFlagBits::eAnyHitKHR,
            "record-mismatch.rahit");
        paired_hits[0].any_hit = occurrence(
            wrong, ShaderStage::anyhit, 3, "record-mismatch.rahit");
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(paired_request(), facts),
            ContainsSubstring(
                "pelican.ray_tracing.shader_record_abi_mismatch@1"));
    }
    SECTION("WP367 anyhit multiple incoming payloads are named") {
        CHECK_THROWS_WITH(
            compile(R"glsl(
              #version 460
              #extension GL_EXT_ray_tracing : require
              layout(location=0) rayPayloadInEXT vec4 payload0;
              layout(location=1) rayPayloadInEXT vec4 payload1;
              hitAttributeEXT vec2 attributes;
              layout(shaderRecordEXT, std430) buffer Record {
                vec3 tint; float flags;
              } record_data;
              void main() {}
            )glsl", vk::ShaderStageFlagBits::eAnyHitKHR,
                "multiple-incoming.rahit"),
            ContainsSubstring("pelican.ray_tracing.interface_count@1"));
    }

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
    SECTION("record storage is forbidden outside traced hit slots") {
        auto record_raygen = compile(R"glsl(
          #version 460
          #extension GL_EXT_ray_tracing : require
          layout(shaderRecordEXT, std430) buffer Record {
            vec3 tint; float flags;
          } record_data;
          void main() {}
        )glsl", vk::ShaderStageFlagBits::eRaygenKHR,
            "record.rgen");
        auto invalid_request = request();
        invalid_request.raygen = occurrence(
            record_raygen, ShaderStage::raygen, 0,
            "record.rgen");
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(
                invalid_request, facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.shader_record_abi_mismatch@1"));

        auto record_miss = compile(R"glsl(
          #version 460
          #extension GL_EXT_ray_tracing : require
          layout(location=0) rayPayloadInEXT vec4 payload;
          layout(shaderRecordEXT, std430) buffer Record {
            vec3 tint; float flags;
          } record_data;
          void main() { payload = vec4(record_data.tint, record_data.flags); }
        )glsl", vk::ShaderStageFlagBits::eMissKHR,
            "record.rmiss");
        invalid_request = request();
        std::array invalid_misses = misses;
        invalid_misses[0] = occurrence(
            record_miss, ShaderStage::miss, 1,
            "record.rmiss");
        invalid_request.misses = invalid_misses;
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(
                invalid_request, facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.shader_record_abi_mismatch@1"));

        invalid_request = request();
        auto invalid_hits = hits;
        invalid_hits[1] = occurrence(
            hit, ShaderStage::closesthit, 4,
            "record-untraced.rchit");
        invalid_request.hit_groups = invalid_hits;
        CHECK_THROWS_WITH(
            validateRayTracingPipelineInterfaces(
                invalid_request, facts),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.shader_record_abi_mismatch@1"));
    }
}

#endif

TEST_CASE("Callable record schema packs std430 values and rejects malformed updates",
          "[ray-tracing][rt4][record]") {
    const auto schema = nlohmann::json::array({
        {{"name", "gain"}, {"type", "f32"}},
        {{"name", "color"}, {"type", "vec3"}},
        {{"name", "mode"}, {"type", "u32"}},
    });
    const auto layout = parseRayCallableRecordLayout(schema, "callable test");
    REQUIRE(layout.members.size() == 3);
    CHECK(layout.members[0].offset == 0);
    CHECK(layout.members[1].offset == 16);
    CHECK(layout.members[2].offset == 28);
    CHECK(layout.data_size == 32);
    nlohmann::json values{{"gain", 2.0}, {"color", {0.25, 0.5, 1.0}}, {"mode", 7}};
    const auto bytes = encodeRayCallableRecord(values, layout);
    REQUIRE(bytes.size() == 32);
    CHECK(readU32(std::span{bytes}.subspan(0, 4)) == std::bit_cast<std::uint32_t>(2.0f));
    CHECK(readU32(std::span{bytes}.subspan(16, 4)) == std::bit_cast<std::uint32_t>(0.25f));
    CHECK(readU32(std::span{bytes}.subspan(28, 4)) == 7u);
    CHECK(std::all_of(bytes.begin() + 4, bytes.begin() + 16,
                      [](std::byte value) { return value == std::byte{0}; }));
    values["color"] = {0.25, 0.5};
    CHECK_THROWS(encodeRayCallableRecord(values, layout));
    values.erase("color");
    CHECK_THROWS(encodeRayCallableRecord(values, layout));
    auto duplicate = schema;
    duplicate.push_back(schema[0]);
    CHECK_THROWS(parseRayCallableRecordLayout(duplicate));
    CHECK(encodeRayCallableRecord(nlohmann::json::object(),
        parseRayCallableRecordLayout(nlohmann::json::array())).empty());
}

} // namespace
} // namespace Pelican
