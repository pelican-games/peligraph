#pragma once
// Shared RT3-1 fixture helpers for the CPU (raytracing_rt3_1_test) and GPU
// (raytracing_rt3_1_production_test) executables.
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican::RayTracingRt31Fixture {

using Json = nlohmann::json;

inline Json layoutRoot(std::uint32_t version = 2) {
    return {
        {"schema", "pelican.ray_group_layout"},
        {"version", version},
        {"ray_types", {"primary", "shadow"}},
        {"default_group", "opaque"},
        {"groups", {{{"name", "opaque"}}}},
        {"misses", {{{"name", "sky"}}, {{"name", "shadow_open"}}}},
        {"record", Json::array()},
    };
}

inline Json oakRule(std::initializer_list<std::string_view> flags) {
    Json encoded_flags = Json::array();
    for (const auto flag : flags) encoded_flags.push_back(flag);
    return {
        {"material",
         {{"document", "materials/multi.json"},
          {"material_name", "Oak"}}},
        {"geometry_flags", std::move(encoded_flags)},
    };
}

inline Json accelerationDeclaration() {
    return {
        {"defaults",
         {{"mask", 127},
          {"instance_flags", {"triangle_facing_cull_disable"}},
          {"geometry_flags", Json::array()}}},
        {"rules", {oakRule({"opaque"})}},
    };
}

inline std::string wp366RaygenSource(std::string_view ray_type,
                                    std::string_view miss,
                                    std::string_view output) {
    std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
layout(set=0,binding=6) uniform accelerationStructureEXT pelicanRayTracingScene;
layout(rgba32ui,set=1,binding=0) writeonly uniform uimage2D OUTPUT_NAME;
struct Payload { uint marker; uint q; uint flags; uint tint; };
layout(location=0) rayPayloadEXT Payload payload;
void main() {
    ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
    float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);
    payload = Payload(0u, 0xffffffffu, 0u, 0u);
    traceRayEXT(pelicanRayTracingScene, gl_RayFlagsOpaqueEXT, 0xffu,
                RAY_TYPE, pelican_ray_type_count, MISS_INDEX,
                vec3(x, 0.0, 0.0), 0.001,
                vec3(0.0, 0.0, 1.0), 4096.0, 0);
    imageStore(OUTPUT_NAME, pixel,
               uvec4(payload.marker, payload.q, payload.flags, payload.tint));
}
)glsl";
    const auto replace_all = [&](std::string_view token,
                                 std::string_view value) {
        for (auto at = source.find(token); at != std::string::npos;
             at = source.find(token, at + value.size())) {
            source.replace(at, token.size(), value);
        }
    };
    replace_all("RAY_TYPE", ray_type);
    replace_all("MISS_INDEX", miss);
    replace_all("OUTPUT_NAME", output);
    return source;
}

inline std::string wp369RaygenSource(std::string_view ray_type,
                                    std::string_view miss,
                                    std::string_view output) {
    auto source = wp366RaygenSource(ray_type, miss, output);
    const std::string old_lane =
        "float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);";
    const std::string new_lane =
        "float x = pixel.x == 0 ? -2.625 : (pixel.x == 1 ? -1.875 : "
        "(pixel.x == 2 ? -1.125 : (pixel.x == 3 ? -0.375 : "
        "(pixel.x == 4 ? 0.375 : (pixel.x == 5 ? 1.125 : "
        "(pixel.x == 6 ? 1.875 : (pixel.x == 7 ? 2.625 : 8.0)))))));";
    const auto lane_at = source.find(old_lane);
    if (lane_at == std::string::npos) {
        throw std::logic_error("WP369 ray lane source is missing");
    }
    source.replace(lane_at, old_lane.size(), new_lane);
    const std::string opaque = "gl_RayFlagsOpaqueEXT";
    const auto opaque_at = source.find(opaque);
    if (opaque_at == std::string::npos) {
        throw std::logic_error("WP369 ray flags source is missing");
    }
    source.replace(opaque_at, opaque.size(), "gl_RayFlagsNoneEXT");
    return source;
}

inline std::string wp366MissSource(std::string_view marker,
                                  std::string_view miss_index) {
    std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
struct Payload { uint marker; uint q; uint flags; uint tint; };
layout(location=0) rayPayloadInEXT Payload payload;
void main() { payload = Payload(MARKER, 0xffffffffu, MISS_INDEX, 0u); }
)glsl";
    source.replace(source.find("MARKER"), 6, marker);
    source.replace(source.find("MISS_INDEX"), 10, miss_index);
    return source;
}

inline std::string wp366ClosestHitSource(std::uint32_t marker,
                                        bool with_engine_record = false,
                                        bool with_fixture_record = true) {
    std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
struct Payload { uint marker; uint q; uint flags; uint tint; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord {
ENGINE_RECORD_MEMBERSFIXTURE_RECORD_MEMBERS
} ray_record;
void main() {
    payload = Payload(MARKER, gl_InstanceCustomIndexEXT,
                      FIXTURE_RECORD_VALUES);
}
)glsl";
    const std::string_view engine_record = with_engine_record
        ? "    PELICAN_RAY_ENGINE_RECORD_MEMBERS\n"
        : "";
    source.replace(source.find("ENGINE_RECORD_MEMBERS"),
                   std::string_view{"ENGINE_RECORD_MEMBERS"}.size(),
                   engine_record);
    const std::string_view fixture_record = with_fixture_record
        ? "    vec4 tint;\n    uint flags;"
        : "";
    source.replace(source.find("FIXTURE_RECORD_MEMBERS"),
                   std::string_view{"FIXTURE_RECORD_MEMBERS"}.size(),
                   fixture_record);
    const std::string_view fixture_values = with_fixture_record
        ? "ray_record.flags, floatBitsToUint(ray_record.tint.x)"
        : "0u, 0u";
    source.replace(source.find("FIXTURE_RECORD_VALUES"),
                   std::string_view{"FIXTURE_RECORD_VALUES"}.size(),
                   fixture_values);
    source.replace(source.find("MARKER"), 6,
                   std::to_string(marker) + "u");
    return source;
}

template <typename WriteText>
inline void writeWp366RaygenAndMissShaders(
    const std::filesystem::path &directory, WriteText write_text,
    bool include_secondary_raygen = true,
    bool material_alpha_profile = false) {
    std::filesystem::create_directories(directory / "shaders");
    const auto raygen = material_alpha_profile
        ? wp369RaygenSource
        : wp366RaygenSource;
    write_text(
        directory / "shaders" / "wp366_a.rgen",
        raygen("PELICAN_RAY_TYPE_primary", "PELICAN_MISS_sky",
               "pelican_resource_wp366_a"));
    if (include_secondary_raygen) {
        write_text(
            directory / "shaders" / "wp366_b.rgen",
            raygen("PELICAN_RAY_TYPE_shadow",
                   "PELICAN_MISS_shadow_open",
                   "pelican_resource_wp366_b"));
    }
    write_text(directory / "shaders" / "wp366_sky.rmiss",
               wp366MissSource("1280u", "PELICAN_MISS_sky"));
    write_text(directory / "shaders" / "wp366_open.rmiss",
               wp366MissSource("1281u", "PELICAN_MISS_shadow_open"));
}

template <typename WriteText>
inline void writeWp366ClosestHitShaders(
    const std::filesystem::path &directory, WriteText write_text,
    bool include_foliage = true, bool with_engine_record = false,
    bool with_fixture_record = true) {
    write_text(directory / "shaders" / "wp366_opaque_primary.rchit",
               wp366ClosestHitSource(
                   with_engine_record ? 0x3690u : 0xa0u,
                   with_engine_record, with_fixture_record));
    write_text(directory / "shaders" / "wp366_opaque_shadow.rchit",
               wp366ClosestHitSource(
                   with_engine_record ? 0x3691u : 0xa1u,
                   with_engine_record, with_fixture_record));
    if (include_foliage) {
        write_text(directory / "shaders" / "wp366_foliage_primary.rchit",
                   wp366ClosestHitSource(
                       with_engine_record ? 0x3692u : 0xb0u,
                       with_engine_record, with_fixture_record));
        write_text(directory / "shaders" / "wp366_foliage_shadow.rchit",
                   wp366ClosestHitSource(
                       with_engine_record ? 0x3693u : 0xb1u,
                       with_engine_record, with_fixture_record));
    }
}

inline Json wp366TypedRayTracingTask(std::string name,
                                    std::string output,
                                    std::string raygen_name,
                                    bool include_foliage = true) {
    Json primary{
        {"opaque",
         {{"closesthit", "project://shaders/wp366_opaque_primary"}}},
    };
    Json shadow{
        {"opaque",
         {{"closesthit", "project://shaders/wp366_opaque_shadow"}}},
    };
    if (include_foliage) {
        primary["foliage"] = {
            {"closesthit", "project://shaders/wp366_foliage_primary"}};
        shadow["foliage"] = {
            {"closesthit", "project://shaders/wp366_foliage_shadow"}};
    }
    return {
        {"name", std::move(name)},
        {"ray_tracing",
         {{"raygen", std::move(raygen_name)},
          {"traced_ray_types", {"primary", "shadow"}},
          {"misses",
           {{"sky", "project://shaders/wp366_sky"},
            {"shadow_open", "project://shaders/wp366_open"}}},
          {"hit_shaders",
           {{"primary", std::move(primary)},
            {"shadow", std::move(shadow)}}}}},
        {"writes", {output}},
        {"resource_ports",
         {{output, {{"resource", output}, {"access", "storage"}}}}},
        {"dispatch", {{"rays_from", {{"port", output}}}}},
    };
}

inline RayGroupLayout v2Layout(std::optional<Json> acceleration =
                            accelerationDeclaration()) {
    auto root = layoutRoot();
    if (acceleration) {
        root["acceleration_structure"] = std::move(*acceleration);
    }
    return parseRayGroupLayout(root.dump(), "rt3-layout.json");
}

inline RaySceneRowInput rowInput(std::string scene, std::string material_name,
                          std::size_t source_index,
                          std::uint64_t authoring_object_id) {
    return {
        .scene_identity = std::move(scene),
        .geometry_identity = "geometry-" +
                             std::to_string(source_index),
        .geometry_match_identity = "match-" +
                                   std::to_string(source_index),
        .material_document = "materials/multi.json",
        .material_asset = "models/chairs.glb",
        .material_name = std::move(material_name),
        .source_material_index =
            static_cast<std::uint32_t>(source_index),
        .geometry_allocation_id = 41,
        .authoring_object_id = authoring_object_id,
        .source_index = source_index,
    };
}

inline const RaySceneRow &rowByAuthor(const RaySceneRowSnapshot &rows,
                               std::uint64_t authoring_object_id) {
    const auto found = std::ranges::find(
        rows.rows, authoring_object_id,
        &RaySceneRow::authoring_object_id);
    REQUIRE(found != rows.rows.end());
    return *found;
}

inline const ResolvedSceneView &resolveProductionScene(bool with_ray_instance,
                                                ResolvedScene &storage) {
    Json components = Json::array(
        {{{"name", "simplemodelview"}, {"model", "chair"}}});
    if (with_ray_instance) {
        components.push_back(
            {{"name", "pelican_ray_instance"}, {"mask", 9}});
    }
    const Json scene{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes",
         {{"main",
           {{"objects",
             Json::array({{{"name", "Chair"},
                           {"components", std::move(components)}}})}}}}},
    };
    const auto authored = AuthoringSceneDocument::load(
        scene.dump(), SceneRevision{1});
    storage = ResolvedSceneResolver::resolve(
        authored, SceneResolverGeneration{1});
    const auto *result = storage.findScene("main");
    REQUIRE(result != nullptr);
    return *result;
}

} // namespace Pelican::RayTracingRt31Fixture
