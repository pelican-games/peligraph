#pragma once
// Shared RT3-1 fixture helpers for the CPU (raytracing_rt3_1_test) and GPU
// (raytracing_rt3_1_production_test) executables.
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <initializer_list>
#include <optional>
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
            {{"name", "ray_instance"}, {"mask", 9}});
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
