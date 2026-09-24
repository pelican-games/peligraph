#include "../src/core/appflow/enginetime.hpp"
#include "render_graph_root_test_support.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "../src/core/appflow/teardown.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "scene_load_test_support.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/imageloader.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/log.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/light/lightcontainer.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "materialcontainer_testaccess.hpp"
#include "../src/core/material/projectmaterialasset.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/vkcore/accelerationstructure.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/gizmo.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderer/shadowdepthpasscontainer.hpp"
#include "../src/core/renderer/velocitypasscontainer.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderingpassconfigregistration.hpp"
#include "../src/core/renderingpass/renderpipelinegpuarena.hpp"
#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/rendertargetconfigregistration.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/renderingpass/rendertargetimageviewresolver.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/shader/raytracingoracle.hpp"
#include "../src/core/shader/raytracingvalidation.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/startup.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "renderer_testaccess.hpp"
#include "rendertarget_testaccess.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/vkcore/util.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/vulkanviewplanning.hpp"
#include "vulkan_test_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>
#include "ktx2_test_writer.hpp"
#include "gltf_fragment_fixture.hpp"
#include "raytracing_rt3_1_fixture.hpp"
#include "synthetic_stereo_target.hpp"

namespace Pelican {

namespace {

std::filesystem::path makeTempProjectDir() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("pelican_headless_render_" + std::to_string(suffix));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeTextFile(const std::filesystem::path &path, const std::string &contents) {
    if (!path.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(
            path.parent_path(), directory_error);
        if (directory_error) {
            throw std::runtime_error(
                "failed to create text fixture parent: " +
                path.parent_path().string() + ": " +
                directory_error.message());
        }
    }
    std::ofstream file{
        path, std::ios::binary | std::ios::trunc};
    if (!file) {
        throw std::runtime_error(
            "failed to open text fixture for writing: " +
            path.string());
    }
    file.write(contents.data(),
               static_cast<std::streamsize>(contents.size()));
    if (!file) {
        throw std::runtime_error(
            "failed to write text fixture: " + path.string());
    }
    file.flush();
    if (!file) {
        throw std::runtime_error(
            "failed to flush text fixture: " + path.string());
    }
    file.close();
    if (file.fail()) {
        throw std::runtime_error(
            "failed to close text fixture: " + path.string());
    }
    std::ifstream verify{path, std::ios::binary};
    if (!verify) {
        throw std::runtime_error(
            "failed to reopen text fixture: " + path.string());
    }
    const std::string reloaded{
        std::istreambuf_iterator<char>{verify},
        std::istreambuf_iterator<char>{}};
    if (reloaded != contents) {
        throw std::runtime_error(
            "text fixture verification mismatch: " + path.string());
    }
}

void writeBinaryFile(const std::filesystem::path &path,
                     std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file.write(contents.data(),
               static_cast<std::streamsize>(contents.size()));
}

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error(
            "failed to open text fixture: " + path.string());
    }
    return {std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{}};
}

nlohmann::json makeProjectConfig(const std::filesystem::path &scene_path, const std::filesystem::path &asset_path) {
    return nlohmann::json{
        {"basic_config",
         {
             {"window_size", {{"width", 32}, {"height", 32}}},
             {"scene_data_json", scene_path.generic_string()},
             {"asset_data_json", asset_path.generic_string()},
         }},
    };
}

void writeRayQueryTestProject(
    const std::filesystem::path &directory,
    bool require_ray_query) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    nlohmann::json rendering{
        {"pipeline",
         {{"preset",
           "engine://render_pipelines/hybrid_v1.json"}}},
    };
    if (require_ray_query) {
        rendering["target_planning"] = {
            {"graphs",
             {{"main_render",
               {{"required_capabilities",
                 nlohmann::json::array(
                     {vulkanRayQueryCapability})}}}}},
        };
    }
    writeTextFile(directory / "hybrid.json",
                  Pelican::TestSupport::renderGraphRootWithTaskFeature(rendering, directory).dump(2));
}

void writeRtShadowMaskTestProject(
    const std::filesystem::path &directory,
    bool raster_shadow,
    bool ray_tracing_pipeline = false) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    nlohmann::json features = nlohmann::json::array({
        "engine://features/rt_shadow_mask.json"});
    if (ray_tracing_pipeline) {
        features.push_back(
            "engine://features/rt_shadow_mask_pipeline.json");
    }
    if (raster_shadow) {
        features.push_back(
            "engine://features/shadow_directional.json");
    }
    writeTextFile(
        directory / "hybrid.json",
        Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json{
            {"pipeline",
             {{"preset",
               "engine://render_pipelines/hybrid_v1.json"}}},
            {"features", std::move(features)},
        }, directory).dump(2));
}

void writeEmbeddedRtShadowMaskTestProject(
    const std::filesystem::path &directory,
    std::optional<std::uint32_t> max_recursion = std::nullopt,
    bool reloadable_project_shaders = false) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    // This intentionally has no shader defines or generated includes. The
    // compiler-OFF build must resolve every stage to embedded SPIR-V.
    auto config = nlohmann::json::parse(R"json({
  "render_targets": [
    {
      "name": "gbuffer_worldpos",
      "extent_scale": 1.0,
      "format": "R16G16B16A16_SFLOAT",
      "format_class": "data",
      "role": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "gbuffer_normal",
      "extent_scale": 1.0,
      "format": "R16G16B16A16_SFLOAT",
      "format_class": "data",
      "role": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "rt_shadow_mask",
      "extent_scale": 1.0,
      "format": "R8_UNORM",
      "format_class": "data",
      "role": "data",
      "usage": ["COLOR_ATTACHMENT", "TRANSFER_SRC"]
    },
    {
      "name": "rt_shadow_mask_pipeline",
      "extent_scale": 1.0,
      "format": "R8_UNORM",
      "format_class": "data",
      "role": "data",
      "usage": ["STORAGE", "TRANSFER_SRC"]
    }
  ],
  "rendering_passes": [
    {
      "name": "main_render",
      "passes": [
        {
          "name": "deferred_geometry",
          "type": "fullscreen",
          "output": {"color": "gbuffer_worldpos", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://shader_lab_hello"
          }
        },
        {
          "name": "deferred_normal",
          "type": "fullscreen",
          "output": {"color": "gbuffer_normal", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://shader_lab_hello"
          }
        },
        {
          "name": "rt_shadow_mask",
          "type": "fullscreen",
          "ray_query": true,
          "output": {"color": "rt_shadow_mask", "depth": null},
          "input": ["gbuffer_worldpos", "gbuffer_normal"],
          "input_sampling": [
            {"filter": "nearest", "address": "clamp_to_edge"},
            {"filter": "nearest", "address": "clamp_to_edge"}
          ],
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://rt_shadow_mask"
          },
          "uses_light_data": true,
          "clear_color": [1.0, 1.0, 1.0, 1.0],
          "color_store_op": "store"
        },
        {
          "name": "present",
          "type": "fullscreen",
          "input": ["gbuffer_worldpos"],
          "input_sampling": [
            {"filter": "nearest", "address": "clamp_to_edge"}
          ],
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://shader_lab_present"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "rt_shadow_mask_pipeline",
      "ray_tracing": {
        "raygen": "engine://rt_shadow_mask_pipeline",
        "miss": "engine://rt_shadow_mask_pipeline",
        "closesthit": "engine://rt_shadow_mask_pipeline"
      },
      "reads": ["gbuffer_worldpos", "gbuffer_normal"],
      "writes": ["rt_shadow_mask_pipeline"],
      "resource_ports": {
        "world_position": {
          "resource": "gbuffer_worldpos",
          "access": "sampled",
          "sampling": {"filter": "nearest", "address": "clamp_to_edge"}
        },
        "normal": {
          "resource": "gbuffer_normal",
          "access": "sampled",
          "sampling": {"filter": "nearest", "address": "clamp_to_edge"}
        },
        "shadow_mask": {
          "resource": "rt_shadow_mask_pipeline",
          "access": "storage"
        }
      },
      "dispatch": {"rays_from": {"port": "shadow_mask"}}
    }
  ],
  "target_planning": {
    "graphs": {
      "main_render": {
        "required_capabilities": [
          "pelican.vulkan.ray_query@1",
          "pelican.vulkan.ray_tracing_pipeline@1"
        ]
      }
    }
  }
})json");
    auto &ray_tracing =
        config["compute_tasks"][0]["ray_tracing"];
    if (max_recursion) {
        ray_tracing["max_recursion"] = *max_recursion;
    }
    if (reloadable_project_shaders) {
        constexpr std::string_view stem =
            "project://shaders/wp364_reload";
        ray_tracing["raygen"] = stem;
        ray_tracing["miss"] = stem;
        ray_tracing["closesthit"] = stem;
        for (const auto extension : {"rgen", "rmiss", "rchit"}) {
            const auto name = std::string{
                                  "rt_shadow_mask_pipeline."} +
                              extension + ".spv";
            writeBinaryFile(
                directory / "shaders" /
                    (std::string{"wp364_reload."} + extension +
                     ".spv"),
                engineResourceOrThrow(name));
        }
    }
    writeTextFile(directory / "hybrid.json", Pelican::TestSupport::renderGraphRootWithTaskFeature(config, directory).dump(2));
}

void writeRt1aOracleProject(
    const std::filesystem::path &directory) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    const auto fixture_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
        "fixtures" / "wp364";
    std::filesystem::create_directories(directory / "shaders");
    for (const auto name : {
             "oracle_offset0.rgen", "oracle_offset1.rgen",
             "oracle.rmiss", "oracle_recursive.rmiss",
             "oracle_hit0.rchit", "oracle_hit1.rchit",
             "oracle_shift_hit0.rchit",
             "oracle_shift_hit1.rchit"}) {
        writeTextFile(directory / "shaders" / name,
                      readTextFile(fixture_root / name));
    }
    writeTextFile(
        directory / "hybrid.json",
        Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json::parse(R"json({
  "render_targets": [
    {
      "name": "wp364_oracle",
      "extent_scale": 1.0,
      "format": "R32G32B32A32_UINT",
      "format_class": "data",
      "role": "data",
      "usage": ["STORAGE", "TRANSFER_SRC"]
    },
    {
      "name": "wp364_oracle_shifted",
      "extent_scale": 1.0,
      "format": "R32G32B32A32_UINT",
      "format_class": "data",
      "role": "data",
      "usage": ["STORAGE", "TRANSFER_SRC"]
    }
  ],
  "rendering_passes": [
    {
      "name": "main_render",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://shader_lab_hello"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "wp364_oracle",
      "ray_tracing": {
        "max_recursion": 2,
        "raygen": "project://shaders/oracle_offset0",
        "miss": [
          "project://shaders/oracle",
          "project://shaders/oracle_recursive"
        ],
        "closesthit": [
          "project://shaders/oracle_hit0",
          "project://shaders/oracle_hit1"
        ]
      },
      "writes": ["wp364_oracle"],
      "resource_ports": {
        "oracle": {
          "resource": "wp364_oracle",
          "access": "storage"
        }
      },
      "dispatch": {"rays_from": {"port": "oracle"}}
    },
    {
      "name": "wp364_oracle_shifted",
      "ray_tracing": {
        "max_recursion": 2,
        "raygen": "project://shaders/oracle_offset1",
        "miss": "project://shaders/oracle",
        "closesthit": [
          "project://shaders/oracle_shift_hit0",
          "project://shaders/oracle_shift_hit1"
        ]
      },
      "writes": ["wp364_oracle_shifted"],
      "resource_ports": {
        "oracle_shifted": {
          "resource": "wp364_oracle_shifted",
          "access": "storage"
        }
      },
      "dispatch": {"rays_from": {"port": "oracle_shifted"}}
    }
  ],
  "target_planning": {
    "graphs": {
      "main_render": {
        "required_capabilities": [
          "pelican.vulkan.ray_query@1",
          "pelican.vulkan.ray_tracing_pipeline@1"
        ]
      }
    }
  }
})json"), directory).dump(2));
}

void writeRt1bLayoutProject(
    const std::filesystem::path &directory) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    std::filesystem::create_directories(
        directory / "models");
    TestGltfFragmentFixture::writeGlb(
        directory / "models" / "tree.glb");
    writeTextFile(
        directory / "materials" / "project.material.json",
        R"json({"schema":"pelican.material","version":1,"materials":[{"name":"ProjectLeaf","surface":"engine://surfaces/openpbr/opaque_single.surface","routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeTextFile(
        directory / "materials" / "tree.bindings.json",
        R"json({"schema":"pelican.material_bindings","version":1,"model":"project://models/tree.glb","bindings":[{"usd_path":"/World/ProjectLeaf","mesh":0,"primitive":0,"material":"ProjectLeaf","routing":{"alpha_mode":"opaque","double_sided":false}},{"usd_path":"/World/EmbeddedLeaf","mesh":1,"primitive":0,"material":"MatB","routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[{"name":"tree","path":"models/tree.glb","material_bindings":"materials/tree.bindings.json"}],"materials":[{"path":"materials/project.material.json"}],"textures":[]})json");
    writeTextFile(
        directory / "ray_layout.json",
        R"json({"schema":"pelican.ray_group_layout","version":3,"ray_types":["primary","shadow"],"default_group":"opaque","groups":[{"name":"opaque","type":"triangles"},{"name":"foliage","type":"triangles"}],"misses":[{"name":"sky"},{"name":"shadow_open"}],"record":[{"name":"tint","type":"vec4"},{"name":"flags","type":"u32"}]})json");
    writeTextFile(
        directory / "ray_bindings.json",
        R"json({"schema":"pelican.ray_bindings","version":2,"bindings":[{"material":{"asset":"models/tree.glb","source_material_index":2},"group":"opaque","record":{"tint":[1.0,0.0,0.0,1.0],"flags":101}},{"material":{"asset":"models/tree.glb","source_material_index":3},"group":"foliage","record":{"tint":[0.0,1.0,0.0,1.0],"flags":202}}]})json");
    RayTracingRt31Fixture::writeWp366RaygenAndMissShaders(
        directory, writeTextFile);
    RayTracingRt31Fixture::writeWp366ClosestHitShaders(
        directory, writeTextFile);
    const nlohmann::json config{
        {"ray_group_layout", "ray_layout.json"},
        {"ray_bindings", "ray_bindings.json"},
        {"render_targets",
         {{{"name", "wp366_a"},
           {"extent_scale", 1.0},
           {"format", "R32G32B32A32_UINT"},
           {"format_class", "data"},
           {"role", "data"},
           {"usage", {"STORAGE", "TRANSFER_SRC"}}},
          {{"name", "wp366_b"},
           {"extent_scale", 1.0},
           {"format", "R32G32B32A32_UINT"},
           {"format_class", "data"},
           {"role", "data"},
           {"usage", {"STORAGE", "TRANSFER_SRC"}}},
          {{"name", "gbuffer_albedo"},
           {"extent_scale", 1.0},
           {"format", "B8G8R8A8_UNORM"},
           {"format_class", "scene"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "gbuffer_normal"},
           {"extent_scale", 1.0},
           {"format", "R16G16B16A16_SFLOAT"},
           {"format_class", "data"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "gbuffer_material"},
           {"extent_scale", 1.0},
           {"format", "R8G8B8A8_UNORM"},
           {"format_class", "data"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "gbuffer_worldpos"},
           {"extent_scale", 1.0},
           {"format", "R16G16B16A16_SFLOAT"},
           {"format_class", "data"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "g_emissive"},
           {"extent_scale", 1.0},
           {"format", "B8G8R8A8_UNORM"},
           {"format_class", "scene"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "scene_depth"},
           {"extent_scale", 1.0},
           {"format", "D32_SFLOAT"},
           {"format_class", "data"},
           {"usage", {"DEPTH_STENCIL_ATTACHMENT", "TRANSFER_SRC"}}}}},
        {"rendering_passes",
         {{{"name", "main_render"},
           {"passes",
            {{{"name", "deferred_geometry"},
              {"type", "material"},
              {"material_contract", "deferred_geometry_v1"},
              {"output",
               {{"color",
                 {"gbuffer_albedo", "gbuffer_normal", "gbuffer_material",
                  "gbuffer_worldpos", "g_emissive"}},
                {"depth", "scene_depth"}}},
              {"depth_store_op", "store"}},
             {{"name", "present"},
              {"type", "fullscreen"},
              {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader",
               {{"vertex", "engine://fullscreen"},
                {"fragment", "engine://shader_lab_hello"}}}}}}}}},
        {"compute_tasks",
         {RayTracingRt31Fixture::wp366TypedRayTracingTask(
              "wp366_a", "wp366_a", "project://shaders/wp366_a"),
          RayTracingRt31Fixture::wp366TypedRayTracingTask(
              "wp366_b", "wp366_b", "project://shaders/wp366_b")}},
        {"target_planning",
         {{"graphs",
           {{"main_render",
             {{"required_capabilities",
               {"pelican.vulkan.ray_query@1",
                "pelican.vulkan.ray_tracing_pipeline@1"}}}}}}}},
    };
    writeTextFile(directory / "hybrid.json", Pelican::TestSupport::renderGraphRootWithTaskFeature(config, directory).dump(2));
}

void writeRt2bTableProject(
    const std::filesystem::path &directory) {
    writeRt1bLayoutProject(directory);

    TestGltfFragmentFixture::writeWp368Glb(
        directory / "models" / "tree.glb");
    writeTextFile(
        directory / "materials" / "forest.material.json",
        R"json({"schema":"pelican.material","version":1,"materials":[{"name":"ProjectOak","surface":"engine://surfaces/openpbr/opaque_single.surface","base":{"baseColorFactor":[0.25,0.5,0.75,1.0]},"routing":{"alpha_mode":"opaque","double_sided":false}},{"name":"ProjectPine","surface":"engine://surfaces/openpbr/opaque_single.surface","base":{"baseColorFactor":[0.75,0.5,0.25,1.0]},"routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeTextFile(
        directory / "materials" / "tree.bindings.json",
        R"json({"schema":"pelican.material_bindings","version":1,"model":"project://models/tree.glb","bindings":[{"usd_path":"/World/EmbeddedRed","mesh":0,"primitive":0,"material":"EmbeddedRed","routing":{"alpha_mode":"opaque","double_sided":false}},{"usd_path":"/World/EmbeddedGreen","mesh":1,"primitive":0,"material":"EmbeddedGreen","routing":{"alpha_mode":"opaque","double_sided":false}},{"usd_path":"/World/ProjectOak","mesh":2,"primitive":0,"material":"ProjectOak","routing":{"alpha_mode":"opaque","double_sided":false}},{"usd_path":"/World/ProjectPine","mesh":3,"primitive":0,"material":"ProjectPine","routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[{"name":"tree","path":"models/tree.glb","material_bindings":"materials/tree.bindings.json"}],"materials":[{"path":"materials/forest.material.json"}],"textures":[]})json");
    writeTextFile(
        directory / "ray_bindings.json",
        R"json({"schema":"pelican.ray_bindings","version":2,"bindings":[{"material":{"asset":"models/tree.glb","source_material_index":0},"group":"opaque","record":{"tint":[1.0,0.0,0.0,1.0],"flags":101}},{"material":{"asset":"models/tree.glb","source_material_index":1},"group":"foliage","record":{"tint":[0.0,1.0,0.0,1.0],"flags":202}},{"material":{"document":"materials/forest.material.json"},"group":"opaque","record":{"tint":[0.25,0.5,0.75,1.0],"flags":303}}]})json");

    for (const auto shader_name : {"wp366_a.rgen", "wp366_b.rgen"}) {
        const auto path = directory / "shaders" / shader_name;
        auto source = readTextFile(path);
        const std::string old_lane =
            "float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);";
        const std::string new_lane =
            "float x = pixel.x == 0 ? -3.0 : (pixel.x == 1 ? -1.0 : "
            "(pixel.x == 2 ? 1.0 : (pixel.x == 3 ? 3.0 : 8.0)));";
        const auto at = source.find(old_lane);
        if (at == std::string::npos) {
            throw std::logic_error("WP368 ray lane source is missing");
        }
        source.replace(at, old_lane.size(), new_lane);
        writeTextFile(path, source);
    }

    auto layout = nlohmann::json::parse(
        readTextFile(directory / "ray_layout.json"));
    // Reverse the authored spelling deliberately. The physical engine prefix
    // remains the canonical [material_index, geometry_index] order.
    layout["engine_record"] = {
        "geometry_index", "material_index"};
    writeTextFile(directory / "ray_layout.json", layout.dump(2));

    const auto hit = [](bool geometry_lane) {
        std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
struct Payload { uint x; uint y; uint z; uint w; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
// A real reflected set-2 declaration supplies the two samplers called out by
// the WP368a device-limit example. It is intentionally not statically used;
// set 2 remains outside this engine-generated table shader's data path while
// its full descriptorCount still participates in layout limit validation.
layout(set=2, binding=0) uniform sampler2D wp368_set2_textures[2];
layout(shaderRecordEXT, std430) buffer RayRecord {
    PELICAN_RAY_ENGINE_RECORD_MEMBERS
    vec4 tint;
    uint flags;
} ray_record;
void main() {
    vec2 uv = pelicanRayFetchUV(ray_record.geometry_index,
                                gl_PrimitiveID, hit_attribute);
    vec3 normal = pelicanRayFetchNormal(ray_record.geometry_index,
                                        gl_PrimitiveID, hit_attribute);
    vec4 color = pelicanRayFetchColor0(ray_record.geometry_index,
                                       gl_PrimitiveID, hit_attribute);
    PelicanRayMaterialRow material =
        pelicanRayMaterial(ray_record.material_index);
    vec4 sampled = pelicanRaySampleBaseColor(
        ray_record.material_index, uv);
    if (GEOMETRY_LANE) {
        payload = Payload(floatBitsToUint(uv.x),
                          floatBitsToUint(normal.z),
                          floatBitsToUint(color.x),
                          pelican_ray_geometry_rows[
                              ray_record.geometry_index].attribute_presence);
    } else {
        payload = Payload(ray_record.material_index,
                          ray_record.geometry_index,
                          floatBitsToUint(material.base_color_factor.x),
                          floatBitsToUint(sampled.x));
    }
}

)glsl";
        const auto token = source.find("GEOMETRY_LANE");
        if (token == std::string::npos) {
            throw std::logic_error(
                "WP368 fixture lost its lane token");
        }
        source.replace(token, std::string_view{"GEOMETRY_LANE"}.size(),
                       geometry_lane ? "true" : "false");
        return source;
    };
    for (const auto group : {"opaque", "foliage"}) {
        writeTextFile(
            directory / "shaders" /
                (std::string{"wp366_"} + group + "_primary.rchit"),
            hit(false));
        writeTextFile(
            directory / "shaders" /
                (std::string{"wp366_"} + group + "_shadow.rchit"),
            hit(true));
    }
}

void setWp369PortableProfile(
    const std::filesystem::path &directory, bool active) {
    auto layout = nlohmann::json::parse(
        readTextFile(directory / "ray_layout.json"));
    if (active) {
        layout["portable_profile"] = "material_alpha_v1";
    } else {
        layout.erase("portable_profile");
    }
    writeTextFile(directory / "ray_layout.json", layout.dump(2));
}

void setWp369GeometryOpaque(
    const std::filesystem::path &directory, bool opaque) {
    auto layout = nlohmann::json::parse(
        readTextFile(directory / "ray_layout.json"));
    auto flags = nlohmann::json::array();
    if (opaque) flags.push_back("opaque");
    layout.at("acceleration_structure")
        .at("defaults")["geometry_flags"] = flags;
    layout.at("acceleration_structure")
        .at("rules").at(0)["geometry_flags"] = std::move(flags);
    writeTextFile(directory / "ray_layout.json", layout.dump(2));
}

void setWp369MaterialRuleOpaque(
    const std::filesystem::path &directory, bool opaque) {
    auto layout = nlohmann::json::parse(
        readTextFile(directory / "ray_layout.json"));
    auto flags = nlohmann::json::array();
    if (opaque) flags.push_back("opaque");
    layout.at("acceleration_structure")
        .at("rules").at(0)["geometry_flags"] = std::move(flags);
    writeTextFile(directory / "ray_layout.json", layout.dump(2));
}

void writeWp369MaterialAlphaProject(
    const std::filesystem::path &directory) {
    writeRt1bLayoutProject(directory);
    TestGltfFragmentFixture::writeWp369Glb(
        directory / "models" / "tree.glb");
    writeTextFile(
        directory / "materials" / "wp369.material.json",
        R"json({"schema":"pelican.material","version":1,"materials":[{"name":"ProjectOpaque","surface":"engine://surfaces/openpbr/opaque_single.surface","base":{"baseColorFactor":[0.8,0.4,0.1,1.0]},"routing":{"alpha_mode":"opaque","double_sided":false}}]})json");
    writeTextFile(
        directory / "materials" / "tree.bindings.json",
        R"json({"schema":"pelican.material_bindings","version":1,"model":"project://models/tree.glb","bindings":[{"usd_path":"/World/CheckerLow","mesh":0,"primitive":0,"material":"CheckerLowMask","routing":{"alpha_mode":"mask","double_sided":false}},{"usd_path":"/World/CheckerHigh","mesh":1,"primitive":0,"material":"CheckerHighMask","routing":{"alpha_mode":"mask","double_sided":false}},{"usd_path":"/World/NegativeClamp","mesh":2,"primitive":0,"material":"NegativeClampMask","routing":{"alpha_mode":"mask","double_sided":false}},{"usd_path":"/World/ColorLow","mesh":3,"primitive":0,"material":"ColorLowMask","routing":{"alpha_mode":"mask","double_sided":false}},{"usd_path":"/World/ColorHigh","mesh":4,"primitive":0,"material":"ColorHighMask","routing":{"alpha_mode":"mask","double_sided":false}},{"usd_path":"/World/CutoffClamp","mesh":5,"primitive":0,"material":"CutoffClampMask","routing":{"alpha_mode":"mask","double_sided":false}},{"usd_path":"/World/ProjectOpaque","mesh":6,"primitive":0,"material":"ProjectOpaque","routing":{"alpha_mode":"opaque","double_sided":false}},{"usd_path":"/World/Blend","mesh":7,"primitive":0,"material":"BlendMaterial","routing":{"alpha_mode":"blend","double_sided":false}}]})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[{"name":"tree","path":"models/tree.glb","material_bindings":"materials/tree.bindings.json"}],"materials":[{"path":"materials/wp369.material.json"}],"textures":[]})json");
    nlohmann::json ray_bindings{
        {"schema", "pelican.ray_bindings"},
        {"version", 2},
        {"bindings", nlohmann::json::array()},
    };
    for (const auto source : {0u, 1u, 2u, 3u, 4u, 5u, 7u}) {
        ray_bindings["bindings"].push_back({
            {"material", {{"asset", "models/tree.glb"},
                          {"source_material_index", source}}},
            {"group", "opaque"},
            {"record", {{"tint", {1.0, 1.0, 1.0, 1.0}},
                        {"flags", 3690u + source}}},
        });
    }
    ray_bindings["bindings"].push_back({
        {"material", {{"document", "materials/wp369.material.json"}}},
        {"group", "opaque"},
        {"record", {{"tint", {0.8, 0.4, 0.1, 1.0}},
                    {"flags", 3696}}},
    });
    writeTextFile(directory / "ray_bindings.json", ray_bindings.dump(2));

    auto layout = nlohmann::json::parse(
        readTextFile(directory / "ray_layout.json"));
    layout["engine_record"] = {
        "material_index", "geometry_index"};
    layout["portable_profile"] = "material_alpha_v1";
    layout["acceleration_structure"] = {
        {"defaults",
         {{"mask", 255},
          {"instance_flags", nlohmann::json::array()},
          {"geometry_flags", nlohmann::json::array()}}},
        {"rules",
         nlohmann::json::array(
             {{{"material",
                {{"asset", "models/tree.glb"},
                 {"source_material_index", 0}}},
               {"geometry_flags", nlohmann::json::array()}}})},
    };
    writeTextFile(directory / "ray_layout.json", layout.dump(2));

    RayTracingRt31Fixture::writeWp366RaygenAndMissShaders(
        directory, writeTextFile, true, true);

    // MG-2b: the project written above is a graph root; edit its section.
    auto root = nlohmann::json::parse(
        readTextFile(directory / "hybrid.json"));
    auto &rendering = renderingSectionOf(root);
    const auto albedo = std::ranges::find(
        rendering["render_targets"], "gbuffer_albedo",
        [](const auto &target) {
            return target.at("name").template get<std::string>();
        });
    if (albedo == rendering["render_targets"].end()) {
        throw std::logic_error("WP369 raster oracle target is missing");
    }
    (*albedo)["usage"].push_back("TRANSFER_SRC");
    rendering["render_targets"].push_back({
        {"name", "wp369_transparent"},
        {"extent_scale", 1.0},
        {"format", "R16G16B16A16_SFLOAT"},
        {"format_class", "explicit(R16G16B16A16_SFLOAT)"},
        {"role", "color"},
        {"usage", {"COLOR_ATTACHMENT"}},
    });
    auto &passes = rendering["rendering_passes"][0]["passes"];
    nlohmann::json transparent_pass{
        {"name", "wp369_forward_transparent"},
        {"type", "material"},
        {"material_contract", "forward_transparent_v1"},
        {"output", {{"color", {"wp369_transparent"}},
                    {"depth", "scene_depth"}}},
        {"depth_load_op", "load"},
        {"depth_store_op", "store"},
    };
    passes.insert(passes.end() - 1, std::move(transparent_pass));
    writeTextFile(directory / "hybrid.json", root.dump(2));

    RayTracingRt31Fixture::writeWp366ClosestHitShaders(
        directory, writeTextFile, true, true);
}

std::string wp368InactiveRenderingConfig(
    std::string_view active_config) {
    // MG-2b: the active config is a graph root. Its authored tasks live in
    // the task feature, so dropping that reference drops them.
    auto root = nlohmann::json::parse(active_config);
    auto &inactive = renderingSectionOf(root);
    inactive.erase("ray_group_layout");
    inactive.erase("ray_bindings");
    if (const auto features = inactive.find("features");
        features != inactive.end()) {
        features->erase(
            std::remove(features->begin(), features->end(),
                        nlohmann::json(
                            "project://features/authored_tasks.json")),
            features->end());
    }
    inactive.erase("target_planning");
    auto &targets = inactive.at("render_targets");
    targets.erase(
        std::remove_if(
            targets.begin(), targets.end(),
            [](const auto &target) {
                const auto name = target.value("name", std::string{});
                return name == "wp366_a" || name == "wp366_b";
            }),
        targets.end());
    return root.dump(2);
}

constexpr std::uint32_t wp367SingleRecord = 111u;
constexpr std::uint32_t wp367FrontRecord = 222u;
constexpr std::uint32_t wp367BackRecord = 333u;
constexpr std::uint32_t wp367ShadowRayMarker = 1u;
constexpr std::uint32_t wp367SingleGroup = 0x510u;
constexpr std::uint32_t wp367FrontGroup = 0x520u;
constexpr std::uint32_t wp367BackGroup = 0x530u;

constexpr std::uint32_t wp367MarkerProof(
    std::uint32_t ray_marker, std::uint32_t group_marker) {
    const auto packed = (ray_marker << 16u) | group_marker;
    const auto anyhit = 0xa3670000u ^ packed;
    const auto closesthit = 0xc3670000u ^ packed;
    return anyhit * 33u + closesthit;
}

std::string wp367AnyHitSource(
    const RayTracingAnyHitIntegerPredicate &predicate,
    std::uint32_t ray_marker, std::uint32_t group_marker) {
    std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint q; uint t_bits; uint record_marker; uint marker_proof; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord {
    uint marker;
    uint ray_marker;
    uint group_marker;
} ray_record;
void main() {
    if (!((ray_record.ray_marker == R_MARKER) &&
          (ray_record.group_marker == GROUP_MARKER) &&
          (PREDICATE))) {
        ignoreIntersectionEXT;
    }
    payload.marker_proof =
        0xa3670000u ^
        ((ray_record.ray_marker << 16u) | ray_record.group_marker);
}
)glsl";
    source.replace(source.find("PREDICATE"),
                   std::string_view{"PREDICATE"}.size(),
                   predicate.glslAcceptExpression("ray_record.marker"));
    source.replace(source.find("R_MARKER"),
                   std::string_view{"R_MARKER"}.size(),
                   std::to_string(ray_marker) + "u");
    source.replace(source.find("GROUP_MARKER"),
                   std::string_view{"GROUP_MARKER"}.size(),
                   std::to_string(group_marker) + "u");
    return source;
}

void writeWp367AnyHitShader(
    const std::filesystem::path &directory,
    std::string_view stem,
    const RayTracingAnyHitIntegerPredicate &predicate,
    std::uint32_t ray_marker, std::uint32_t group_marker) {
    writeTextFile(directory / "shaders" /
                      (std::string{stem} + ".rahit"),
                  wp367AnyHitSource(
                      predicate, ray_marker, group_marker));
}

void writeWp367ActualAnyHitShaders(
    const std::filesystem::path &directory,
    const RayTracingAnyHitIntegerPredicate &single,
    const RayTracingAnyHitIntegerPredicate &overlap) {
    writeWp367AnyHitShader(
        directory, "wp367_actual_single", single,
        wp367ShadowRayMarker, wp367SingleGroup);
    writeWp367AnyHitShader(
        directory, "wp367_actual_front", overlap,
        wp367ShadowRayMarker, wp367FrontGroup);
    writeWp367AnyHitShader(
        directory, "wp367_actual_back", overlap,
        wp367ShadowRayMarker, wp367BackGroup);
}

void writeRt2aAnyHitProject(
    const std::filesystem::path &directory) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    writeTextFile(
        directory / "ray_layout.json",
        R"json({"schema":"pelican.ray_group_layout","version":3,"ray_types":["primary","shadow"],"default_group":"single","groups":[{"name":"single","type":"triangles"},{"name":"front","type":"triangles"},{"name":"back","type":"triangles"}],"misses":[{"name":"open"}],"record":[{"name":"marker","type":"u32"},{"name":"ray_marker","type":"u32"},{"name":"group_marker","type":"u32"}]})json");
    writeTextFile(
        directory / "ray_bindings.json",
        R"json({"schema":"pelican.ray_bindings","version":2,"bindings":[{"material":{"asset":"models/wp367_single.glb","source_material_index":0},"group":"single","record":{"marker":111,"ray_marker":1,"group_marker":1296}},{"material":{"asset":"models/wp367_front.glb","source_material_index":0},"group":"front","record":{"marker":222,"ray_marker":1,"group_marker":1312}},{"material":{"asset":"models/wp367_back.glb","source_material_index":0},"group":"back","record":{"marker":333,"ray_marker":1,"group_marker":1328}}]})json");
    std::filesystem::create_directories(directory / "shaders");

    const auto raygen = [](std::string_view output) {
        std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
layout(set=0,binding=6) uniform accelerationStructureEXT pelicanRayTracingScene;
layout(rgba32ui,set=1,binding=0) writeonly uniform uimage2D OUTPUT_NAME;
struct Payload { uint q; uint t_bits; uint record_marker; uint marker_proof; };
layout(location=0) rayPayloadEXT Payload payload;
void main() {
    ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
    float x = pixel.x == 0 ? -2.0 : (pixel.x == 1 ? 2.0 : 8.0);
    payload = Payload(0xffffffffu, 0u, 0xffffffffu, 0u);
    traceRayEXT(pelicanRayTracingScene, 0u, 0xffu,
                PELICAN_RAY_TYPE_shadow, pelican_ray_type_count,
                PELICAN_MISS_open, vec3(x, 0.0, 0.0), 0.001,
                vec3(0.0, 0.0, 1.0), 4096.0, 0);
    imageStore(OUTPUT_NAME, pixel,
               uvec4(payload.q, payload.t_bits,
                     payload.record_marker, payload.marker_proof));
}
)glsl";
        for (auto at = source.find("OUTPUT_NAME");
             at != std::string::npos;
             at = source.find("OUTPUT_NAME", at + output.size())) {
            source.replace(at, std::string_view{"OUTPUT_NAME"}.size(),
                           output);
        }
        return source;
    };
    const auto closest_hit = [](std::uint32_t group_marker) {
        std::string source = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
struct Payload { uint q; uint t_bits; uint record_marker; uint marker_proof; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord {
    uint marker;
    uint ray_marker;
    uint group_marker;
} ray_record;
void main() {
    uint anyhit_marker_proof = payload.marker_proof;
    uint closesthit_marker_proof =
        (ray_record.ray_marker == PELICAN_RAY_TYPE_shadow &&
         ray_record.group_marker == GROUP_MARKER)
            ? (0xc3670000u ^
               ((ray_record.ray_marker << 16u) |
                ray_record.group_marker))
            : 0xfffffffeu;
    payload = Payload(uint(gl_InstanceCustomIndexEXT),
                      floatBitsToUint(gl_HitTEXT),
                      ray_record.marker,
                      anyhit_marker_proof * 33u +
                          closesthit_marker_proof);
}
)glsl";
        for (auto at = source.find("GROUP_MARKER");
             at != std::string::npos;
             at = source.find("GROUP_MARKER", at + 1)) {
            source.replace(at, std::string_view{"GROUP_MARKER"}.size(),
                           std::to_string(group_marker) + "u");
        }
        return source;
    };
    writeTextFile(directory / "shaders" / "wp367_actual.rgen",
                  raygen("pelican_resource_wp367_actual"));
    writeTextFile(directory / "shaders" / "wp367_always.rgen",
                  raygen("pelican_resource_wp367_always"));
    writeTextFile(
        directory / "shaders" / "wp367_open.rmiss",
        R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
struct Payload { uint q; uint t_bits; uint record_marker; uint marker_proof; };
layout(location=0) rayPayloadInEXT Payload payload;
void main() {
    payload = Payload(0xffffffffu, 0u, 0x6ffu, 0x5ffu);
}
)glsl");
    writeTextFile(directory / "shaders" / "wp367_single.rchit",
                  closest_hit(wp367SingleGroup));
    writeTextFile(directory / "shaders" / "wp367_front.rchit",
                  closest_hit(wp367FrontGroup));
    writeTextFile(directory / "shaders" / "wp367_back.rchit",
                  closest_hit(wp367BackGroup));

    const auto always = RayTracingAnyHitIntegerPredicate::alwaysAccept();
    writeWp367ActualAnyHitShaders(directory, always, always);
    for (const auto [group, marker] : {
             std::pair{"single", wp367SingleGroup},
             std::pair{"front", wp367FrontGroup},
             std::pair{"back", wp367BackGroup}}) {
        writeWp367AnyHitShader(
            directory, std::string{"wp367_always_"} + group, always,
            wp367ShadowRayMarker, marker);
    }

    const auto hit_shaders = [](std::string_view lane) {
        const auto group = [lane](std::string_view name) {
            nlohmann::json result{
                {"closesthit", "project://shaders/wp367_" +
                                   std::string{name}},
                {"anyhit", "project://shaders/wp367_" +
                               std::string{lane} + "_" +
                               std::string{name}},
            };
            return result;
        };
        return nlohmann::json{
            {"shadow",
             {{"single", group("single")},
              {"front", group("front")},
              {"back", group("back")}}},
        };
    };
    const auto task = [&](std::string_view lane) {
        const auto output = "wp367_" + std::string{lane};
        return nlohmann::json{
            {"name", output},
            {"ray_tracing",
             {{"raygen", "project://shaders/" + output},
              {"traced_ray_types", {"shadow"}},
              {"misses", {{"open", "project://shaders/wp367_open"}}},
              {"hit_shaders", hit_shaders(lane)}}},
            {"writes", {output}},
            {"resource_ports",
             {{output, {{"resource", output}, {"access", "storage"}}}}},
            {"dispatch", {{"rays_from", {{"port", output}}}}},
        };
    };
    const auto target = [](std::string_view name) {
        return nlohmann::json{
            {"name", name},
            {"extent_scale", 1.0},
            {"format", "R32G32B32A32_UINT"},
            {"format_class", "data"},
            {"role", "data"},
            {"usage", {"STORAGE", "TRANSFER_SRC"}},
        };
    };
    const nlohmann::json config{
        {"ray_group_layout", "ray_layout.json"},
        {"ray_bindings", "ray_bindings.json"},
        {"render_targets",
         {target("wp367_actual"),
          target("wp367_always"),
          {{"name", "gbuffer_albedo"},
           {"extent_scale", 1.0},
           {"format", "B8G8R8A8_UNORM"},
           {"format_class", "scene"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "gbuffer_normal"},
           {"extent_scale", 1.0},
           {"format", "R16G16B16A16_SFLOAT"},
           {"format_class", "data"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "gbuffer_material"},
           {"extent_scale", 1.0},
           {"format", "R8G8B8A8_UNORM"},
           {"format_class", "data"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "gbuffer_worldpos"},
           {"extent_scale", 1.0},
           {"format", "R16G16B16A16_SFLOAT"},
           {"format_class", "data"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "g_emissive"},
           {"extent_scale", 1.0},
           {"format", "B8G8R8A8_UNORM"},
           {"format_class", "scene"},
           {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
          {{"name", "scene_depth"},
           {"extent_scale", 1.0},
           {"format", "D32_SFLOAT"},
           {"format_class", "data"},
           {"usage", {"DEPTH_STENCIL_ATTACHMENT", "TRANSFER_SRC"}}}}},
        {"rendering_passes",
         {{{"name", "main_render"},
           {"passes",
            {{{"name", "deferred_geometry"},
              {"type", "material"},
              {"material_contract", "deferred_geometry_v1"},
              {"output",
               {{"color",
                 {"gbuffer_albedo", "gbuffer_normal", "gbuffer_material",
                  "gbuffer_worldpos", "g_emissive"}},
                {"depth", "scene_depth"}}},
              {"depth_store_op", "store"}},
             {{"name", "present"},
              {"type", "fullscreen"},
              {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader",
               {{"vertex", "engine://fullscreen"},
                {"fragment", "engine://shader_lab_hello"}}}}}}}}},
        {"compute_tasks", {task("actual"), task("always")}},
        {"target_planning",
         {{"graphs",
           {{"main_render",
             {{"required_capabilities",
               {"pelican.vulkan.ray_query@1",
                "pelican.vulkan.ray_tracing_pipeline@1"}}}}}}}},
    };
    writeTextFile(directory / "hybrid.json", Pelican::TestSupport::renderGraphRootWithTaskFeature(config, directory).dump(2));
}

enum class Wp367NegativeAnyHitLoadKind {
    payload_mismatch,
    unreached_payload_mismatch,
    vec3_attribute,
    record_mismatch,
    multiple_incoming_payloads,
};

void writeWp367NegativeAnyHitLoadProject(
    const std::filesystem::path &directory,
    Wp367NegativeAnyHitLoadKind kind) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    writeTextFile(
        directory / "ray_layout.json",
        kind == Wp367NegativeAnyHitLoadKind::unreached_payload_mismatch
            ? R"json({"schema":"pelican.ray_group_layout","version":3,"ray_types":["shadow","unused"],"default_group":"only","groups":[{"name":"only","type":"triangles"}],"misses":[{"name":"open"}],"record":[{"name":"marker","type":"u32"}]})json"
            : R"json({"schema":"pelican.ray_group_layout","version":3,"ray_types":["shadow"],"default_group":"only","groups":[{"name":"only","type":"triangles"}],"misses":[{"name":"open"}],"record":[{"name":"marker","type":"u32"}]})json");
    writeTextFile(
        directory / "shaders" / "wp367_negative.rgen",
        R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require
#include "__pelican_ray_group_layout.glsl"
layout(set=0,binding=6) uniform accelerationStructureEXT pelicanRayTracingScene;
struct Payload { uint q; uint t_bits; uint ray_type; uint group_marker; };
layout(location=0) rayPayloadEXT Payload payload;
void main() {
    traceRayEXT(pelicanRayTracingScene, 0u, 0xffu,
                PELICAN_RAY_TYPE_shadow, pelican_ray_type_count,
                PELICAN_MISS_open, vec3(0.0), 0.001,
                vec3(0.0, 0.0, 1.0), 4096.0, 0);
}
)glsl");
    writeTextFile(
        directory / "shaders" / "wp367_negative.rmiss",
        R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint q; uint t_bits; uint ray_type; uint group_marker; };
layout(location=0) rayPayloadInEXT Payload payload;
void main() { payload.q = 0xffffffffu; }
)glsl");
    writeTextFile(
        directory / "shaders" / "wp367_negative.rchit",
        R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint q; uint t_bits; uint ray_type; uint group_marker; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uint marker; } ray_record;
void main() { payload.q = ray_record.marker; }
)glsl");

    std::string anyhit;
    switch (kind) {
    case Wp367NegativeAnyHitLoadKind::payload_mismatch:
        anyhit = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT uvec4 payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uint marker; } ray_record;
void main() { if (ray_record.marker == 0xffffffffu) ignoreIntersectionEXT; }
)glsl";
        break;
    case Wp367NegativeAnyHitLoadKind::unreached_payload_mismatch:
        anyhit = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint q; uint t_bits; uint ray_type; uint group_marker; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uint marker; } ray_record;
void main() { if (ray_record.marker == 0xffffffffu) ignoreIntersectionEXT; }
)glsl";
        break;
    case Wp367NegativeAnyHitLoadKind::vec3_attribute:
        anyhit = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint q; uint t_bits; uint ray_type; uint group_marker; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec3 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uint marker; } ray_record;
void main() { if (ray_record.marker == 0xffffffffu) ignoreIntersectionEXT; }
)glsl";
        break;
    case Wp367NegativeAnyHitLoadKind::record_mismatch:
        anyhit = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint q; uint t_bits; uint ray_type; uint group_marker; };
layout(location=0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uvec2 marker; } ray_record;
void main() { if (ray_record.marker.x == 0xffffffffu) ignoreIntersectionEXT; }
)glsl";
        break;
    case Wp367NegativeAnyHitLoadKind::multiple_incoming_payloads:
        anyhit = R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
struct Payload { uint q; uint t_bits; uint ray_type; uint group_marker; };
layout(location=0) rayPayloadInEXT Payload payload0;
layout(location=1) rayPayloadInEXT Payload payload1;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uint marker; } ray_record;
void main() { if (ray_record.marker == 0xffffffffu) ignoreIntersectionEXT; }
)glsl";
        break;
    }
    writeTextFile(directory / "shaders" / "wp367_negative.rahit",
                  anyhit);
    if (kind == Wp367NegativeAnyHitLoadKind::unreached_payload_mismatch) {
        writeTextFile(
            directory / "shaders" / "wp367_negative_unused.rchit",
            R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uint marker; } ray_record;
void main() { payload.x = float(ray_record.marker); }
)glsl");
        writeTextFile(
            directory / "shaders" / "wp367_negative_unused.rahit",
            R"glsl(#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT uvec4 payload;
hitAttributeEXT vec2 hit_attribute;
layout(shaderRecordEXT, std430) buffer RayRecord { uint marker; } ray_record;
void main() { if (ray_record.marker == 0xffffffffu) ignoreIntersectionEXT; }
)glsl");
    }
    auto config = nlohmann::json::parse(R"json({
  "ray_group_layout": "ray_layout.json",
  "render_targets": [
    {
      "name": "wp367_negative_output",
      "extent_scale": 1.0,
      "format": "R8_UNORM",
      "format_class": "data",
      "role": "data",
      "usage": ["STORAGE"]
    }
  ],
  "rendering_passes": [
    {
      "name": "main_render",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://shader_lab_hello"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "wp367_negative_load",
      "ray_tracing": {
        "raygen": "project://shaders/wp367_negative",
        "traced_ray_types": ["shadow"],
        "misses": {"open": "project://shaders/wp367_negative"},
        "hit_shaders": {
          "shadow": {
            "only": {
              "closesthit": "project://shaders/wp367_negative",
              "anyhit": "project://shaders/wp367_negative"
            }
          }
        }
      },
      "writes": ["wp367_negative_output"],
      "resource_ports": {
        "output": {
          "resource": "wp367_negative_output",
          "access": "storage"
        }
      },
      "dispatch": {"rays_from": {"port": "output"}}
    }
  ],
  "target_planning": {
    "graphs": {
      "main_render": {
        "required_capabilities": [
          "pelican.vulkan.ray_query@1",
          "pelican.vulkan.ray_tracing_pipeline@1"
        ]
      }
    }
  }
})json");
    if (kind == Wp367NegativeAnyHitLoadKind::unreached_payload_mismatch) {
        auto &ray = config["compute_tasks"][0]["ray_tracing"];
        ray["traced_ray_types"].push_back("unused");
        ray["hit_shaders"]["unused"] = {
            {"only",
             {{"closesthit",
               "project://shaders/wp367_negative_unused"},
              {"anyhit",
               "project://shaders/wp367_negative_unused"}}},
        };
    }
    writeTextFile(directory / "hybrid.json", Pelican::TestSupport::renderGraphRootWithTaskFeature(config, directory).dump(2));
}

enum class Rt1aNegativeLoadKind {
    payload_mismatch,
    forbidden_hit_attribute,
    dynamic_trace_operand,
};

void writeRt1aNegativeLoadProject(
    const std::filesystem::path &directory,
    Rt1aNegativeLoadKind kind) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
    const auto fixture_root =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
        "fixtures" / "wp364";
    std::filesystem::create_directories(directory / "shaders");
    const auto raygen =
        kind == Rt1aNegativeLoadKind::dynamic_trace_operand
            ? "negative_dynamic.rgen"
            : "negative_valid.rgen";
    const auto miss =
        kind == Rt1aNegativeLoadKind::payload_mismatch
            ? "negative_payload_mismatch.rmiss"
            : "negative_valid.rmiss";
    const auto closest_hit =
        kind == Rt1aNegativeLoadKind::forbidden_hit_attribute
            ? "negative_vec3.rchit"
            : "negative_valid.rchit";
    for (const auto &[source, destination] :
         {std::pair{raygen, "negative.rgen"},
          std::pair{miss, "negative.rmiss"},
          std::pair{closest_hit, "negative.rchit"}}) {
        writeTextFile(directory / "shaders" / destination,
                      readTextFile(fixture_root / source));
    }
    writeTextFile(
        directory / "hybrid.json",
        Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json::parse(R"json({
  "render_targets": [
    {
      "name": "wp364_negative_output",
      "extent_scale": 1.0,
      "format": "R8_UNORM",
      "format_class": "data",
      "role": "data",
      "usage": ["STORAGE"]
    }
  ],
  "rendering_passes": [
    {
      "name": "main_render",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "engine://shader_lab_hello"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "wp364_negative_load",
      "ray_tracing": {
        "raygen": "project://shaders/negative",
        "miss": "project://shaders/negative",
        "closesthit": "project://shaders/negative"
      },
      "writes": ["wp364_negative_output"],
      "resource_ports": {
        "output": {
          "resource": "wp364_negative_output",
          "access": "storage"
        }
      },
      "dispatch": {"rays_from": {"port": "output"}}
    }
  ],
  "target_planning": {
    "graphs": {
      "main_render": {
        "required_capabilities": [
          "pelican.vulkan.ray_query@1",
          "pelican.vulkan.ray_tracing_pipeline@1"
        ]
      }
    }
  }
})json"), directory).dump(2));
}

void configureRayQueryTestRuntime(
    const std::filesystem::path &directory) {
    auto project =
        makeProjectConfig("scene.json", "assets.json");
    project["basic_config"]["default_scene_id"] =
        "default_scene";
    project["basic_config"]["rendering_config_json"] =
        "hybrid.json";
    project["basic_config"]["default_rendering_pass"] =
        "main_render";
    GET_MODULE(ProjectSource).setSourceByData(
        project.dump());
    GET_MODULE(PathResolver).setup(directory, false);
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{32, 32};
    launch.headless_frames = 1;
    GET_MODULE(EngineTime).setup(
        EngineTime::Mode::fixed_step, 1.0 / 60.0);
}

void renderClearFrame(RenderTarget &render_target, vk::ClearColorValue clear_color) {
    auto begun = render_target.beginFrame(nullptr);
    REQUIRE(begun.disposition ==
            FrameBeginDisposition::ready);
    REQUIRE(begun.frame.has_value());
    auto token = std::move(*begun.frame);
    const auto frame = token.context();

    vk::RenderingAttachmentInfo color_attachment;
    color_attachment.imageView = frame.color_attachment;
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    color_attachment.clearValue.color = clear_color;

    vk::RenderingInfo rendering_info;
    rendering_info.renderArea = vk::Rect2D{{0, 0}, frame.extent};
    rendering_info.layerCount = 1;
    rendering_info.setColorAttachments(color_attachment);

    frame.cmd_buf.beginRendering(rendering_info);
    frame.cmd_buf.endRendering();

    render_target.submit(std::move(token));
}

CommonPolygonVertData makeScreenQuad(float half_extent, float z) {
    CommonPolygonVertData data;
    data.indices = {0, 1, 2, 0, 2, 3};
    data.pos = {{-half_extent, -half_extent, z},
                {half_extent, -half_extent, z},
                {half_extent, half_extent, z},
                {-half_extent, half_extent, z}};
    data.normal.assign(4, glm::vec3{0.0f, 0.0f, 1.0f});
    data.texcoord = {{0.0f, 0.0f}, {1.0f, 0.0f},
                     {1.0f, 1.0f}, {0.0f, 1.0f}};
    data.color.assign(4, glm::vec4{1.0f});
    return data;
}

std::uint8_t r8PixelAt(const R8RenderTargetReadback &readback,
                       std::uint32_t x, std::uint32_t y) {
    if (x >= readback.extent.width || y >= readback.extent.height) {
        throw std::out_of_range(
            "R8 readback pixel coordinate is out of range");
    }
    return readback.pixels.at(
        static_cast<std::size_t>(y) * readback.extent.width + x);
}

std::vector<std::uint8_t>
readFrameGraphBuffer(
    std::string_view name) {
    auto &resources =
        GET_MODULE(FrameGraphResourceContainer);
    const auto size = resources.bufferSize(name);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto staging = vkcore.allocBuf(
        size,
        vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::
            eHostAccessRandom);
    const auto &source = resources.buffer(name);
    GET_MODULE(VulkanUtils).executeOneTimeCmd(
        [&](vk::CommandBuffer command) {
            vk::BufferMemoryBarrier barrier;
            barrier.srcAccessMask =
                vk::AccessFlagBits::eShaderWrite;
            barrier.dstAccessMask =
                vk::AccessFlagBits::eTransferRead;
            barrier.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = source.buffer.get();
            barrier.offset = 0;
            barrier.size = size;
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::
                    eComputeShader,
                vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {barrier}, {});
            command.copyBuffer(
                source.buffer.get(),
                staging.buffer.get(),
                vk::BufferCopy{0, 0, size});
        },
        true);
    return vkcore.readBuf(staging, size);
}

std::uint32_t wordAt(
    std::span<const std::uint8_t> bytes,
    std::size_t index) {
    const auto offset =
        index * sizeof(std::uint32_t);
    if (offset + sizeof(std::uint32_t) >
        bytes.size()) {
        throw std::out_of_range(
            "buffer word is outside readback");
    }
    std::uint32_t value = 0;
    std::memcpy(
        &value, bytes.data() + offset,
        sizeof(value));
    return value;
}

CommonPolygonVertData makeOutlineCube(float half_extent) {
    CommonPolygonVertData data;
    const auto add_face =
        [&](glm::vec3 normal,
            std::array<glm::vec3, 4> positions) {
            const auto first =
                static_cast<std::uint32_t>(data.pos.size());
            data.pos.insert(
                data.pos.end(), positions.begin(), positions.end());
            data.normal.insert(data.normal.end(), 4, normal);
            data.texcoord.insert(
                data.texcoord.end(),
                {{0.0f, 0.0f}, {1.0f, 0.0f},
                 {1.0f, 1.0f}, {0.0f, 1.0f}});
            data.color.insert(data.color.end(), 4, glm::vec4{1.0f});
            data.indices.insert(
                data.indices.end(),
                {first, first + 1, first + 2,
                 first, first + 2, first + 3});
        };
    const auto h = half_extent;
    add_face(
        {0.0f, 0.0f, 1.0f},
        {{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}});
    add_face(
        {0.0f, 0.0f, -1.0f},
        {{{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}}});
    add_face(
        {1.0f, 0.0f, 0.0f},
        {{{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}}});
    add_face(
        {-1.0f, 0.0f, 0.0f},
        {{{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}}});
    add_face(
        {0.0f, 1.0f, 0.0f},
        {{{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}}});
    add_face(
        {0.0f, -1.0f, 0.0f},
        {{{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}});
    return data;
}

const char *gpuArenaFullscreenVertexShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec2 outUV;
vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);
void main() {
    vec2 pos = positions[gl_VertexIndex];
    outUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";
}

const char *gpuArenaComputeShader() {
    return R"glsl(
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 1, binding = 0) buffer ComputeColor {
    vec4 color;
} compute_color;
void main() {
    compute_color.color = vec4(0.1, 0.2, 0.3, 1.0);
}
)glsl";
}

const char *materialDisplacementComputeShader() {
    return R"glsl(
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 1, binding = 0) buffer Displacement {
    vec4 values[];
} displacement;
void main() {
    displacement.values[0] = vec4(0.55, 0.0, 0.0, 0.0);
}
)glsl";
}

const char *materialResourceTintFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(1.0, 0.01, 0.01, 1.0);
}
)glsl";
}

const char *materialResourceMipComputeShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 coordinate = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(
            coordinate, pelican_size_tint_mip()))) {
        return;
    }
    pelican_store_tint_mip(
        coordinate, vec4(0.01, 1.0, 0.01, 1.0));
}
)glsl";
}

const char *gpuArenaBufferFragmentShader() {
    return R"glsl(
#version 450
layout(std430, set = 1, binding = 0) readonly buffer ComputeColor {
    vec4 color;
} compute_color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = compute_color.color;
}
)glsl";
}

const char *gpuArenaCopyFragmentShader() {
    return R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D inputTexture;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = texture(inputTexture, inUV);
}
)glsl";
}

const char *depthPyramidSeedComputeShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 coordinate = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(
            coordinate, pelican_size_seed_depth()))) {
        return;
    }
    pelican_store_seed_depth(
        coordinate, vec4(0.12, 0.75, 0.25, 1.0));
}
)glsl";
}

const char *depthPyramidReduceComputeShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    ivec2 coordinate = ivec2(gl_GlobalInvocationID.xy);
    ivec2 output_size = pelican_size_reduced_depth();
    if (any(greaterThanEqual(coordinate, output_size))) {
        return;
    }
    vec2 uv = (vec2(coordinate) + vec2(0.5)) /
              vec2(output_size);
    pelican_store_reduced_depth(
        coordinate, pelican_sample_source_depth(uv));
}
)glsl";
}

const char *depthPyramidPresentFragmentShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = pelican_sample_pyramid(inUV);
}
)glsl";
}

const char *pipelineReloadFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.2, 0.4, 0.6, 1.0);
}
)glsl";
}

const char *wp353BlendDestinationFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.1, 0.2, 0.3, 0.4);
}
)glsl";
}

const char *wp353BlendSourceFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.4, 0.2, 0.1, 0.5);
}
)glsl";
}

const char *wp353BlendPresentFragmentShader() {
    return R"glsl(
#version 450
layout(set = 1, binding = 0) uniform sampler2D omittedTexture;
layout(set = 1, binding = 1) uniform sampler2D additiveTexture;
layout(set = 1, binding = 2) uniform sampler2D oneOneTexture;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    if (inUV.x < (1.0 / 3.0)) {
        outColor = texture(omittedTexture, inUV);
    } else if (inUV.x < (2.0 / 3.0)) {
        outColor = texture(additiveTexture, inUV);
    } else {
        outColor = texture(oneOneTexture, inUV);
    }
}
)glsl";
}

const char *localReadProducerFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.2, 0.4, 0.8, 1.0);
}
)glsl";
}

const char *localReadCopyFragmentShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    ivec2 target_size = pelican_size_input_color();
    ivec2 target_size_lod = pelican_size_lod_input_color(0);
    ivec2 old_frame_size =
        ivec2(pelicanResolution.render_resolution.xy);
    if (any(notEqual(target_size, ivec2(16, 16))) ||
        any(notEqual(target_size_lod, target_size)) ||
        all(equal(target_size, old_frame_size))) {
        outColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec4 value = pelican_sample_input_color(inUV);
    outColor = vec4(value.b, value.g, value.r, 1.0);
}
)glsl";
}

const char *physicalScopeBaseFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.2, 0.4, 0.8, 1.0);
}
)glsl";
}

const char *physicalScopeOverlayFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    if (inUV.x < 0.5) {
        discard;
    }
    outColor = vec4(0.8, 0.2, 0.4, 1.0);
}
)glsl";
}

const char *aliasProducerAFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)glsl";
}

const char *aliasProducerBFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.0, 1.0, 0.0, 1.0);
}
)glsl";
}

const char *upscaleContractGeneratorFragmentShader() {
    return R"glsl(
#version 450
layout(set = 0, binding = 4, std140) uniform ResolutionContract {
    vec4 render_resolution;
    vec4 output_resolution;
} resolution_contract;
layout(location = 0) out vec4 outColor;
void main() {
    bool valid =
        all(lessThan(abs(resolution_contract.render_resolution.xy -
                         vec2(16.0)), vec2(0.01))) &&
        all(lessThan(abs(resolution_contract.output_resolution.xy -
                         vec2(32.0)), vec2(0.01)));
    if (!valid) {
        outColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    outColor = gl_FragCoord.x < 8.0
        ? vec4(1.0, 0.0, 0.0, 1.0)
        : vec4(0.0, 0.0, 1.0, 1.0);
}
)glsl";
}

const char *upscaleContractCopyFragmentShader() {
    return R"glsl(
#version 450
layout(set = 0, binding = 4, std140) uniform ResolutionContract {
    vec4 render_resolution;
    vec4 output_resolution;
} resolution_contract;
layout(set = 1, binding = 0) uniform sampler2D lowColor;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    bool valid =
        all(lessThan(abs(resolution_contract.render_resolution.xy -
                         vec2(16.0)), vec2(0.01))) &&
        all(lessThan(abs(resolution_contract.output_resolution.xy -
                         vec2(32.0)), vec2(0.01)));
    outColor = valid
        ? texture(lowColor, inUV)
        : vec4(1.0, 0.0, 1.0, 1.0);
}
)glsl";
}

nlohmann::json upscaleContractRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "features": ["features/upscale_jitter.json"],
  "render_targets": [
    {
      "name": "low_color",
      "extent_scale": 0.5,
      "format": "R8G8B8A8_UNORM",
      "format_candidates": ["R16G16B16A16_SFLOAT"],
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "transient_scratch",
      "extent_scale": 0.5,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT"]
    }
  ],
  "rendering_passes": [
    {
      "name": "upscale_main",
      "passes": [
        {
          "name": "transient_probe",
          "type": "fullscreen",
          "resolution_domain": "scene",
          "output": {"color": "transient_scratch", "depth": null},
          "shader": {
            "vertex": "shaders/upscale_fullscreen",
            "fragment": "shaders/upscale_generate"
          }
        },
        {
          "name": "produce_low",
          "type": "fullscreen",
          "resolution_domain": "scene",
          "output": {"color": "low_color", "depth": null},
          "shader": {
            "vertex": "shaders/upscale_fullscreen",
            "fragment": "shaders/upscale_generate"
          }
        },
        {
          "name": "upscale",
          "type": "fullscreen",
          "input": ["low_color"],
          "input_sampling": [
            {"filter": "nearest", "address": "clamp_to_edge"}
          ],
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/upscale_fullscreen",
            "fragment": "shaders/upscale_copy"
          }
        }
      ]
    }
  ]
}
)json");
}

nlohmann::json gpuArenaRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_strategy": {
    "name": "headless.gpu_arena"
  },
  "graph_transforms": [
    {"name": "gpu_arena.identity"}
  ],
  "features": [
    "engine://features/debug_draw.json",
    "engine://features/gizmo.json",
    "engine://features/debug_text.json"
  ],
  "render_targets": [
    {
      "name": "gpu_arena_scratch",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "gpu_arena_shadow",
      "extent_scale": 1.0,
      "format": "D32_SFLOAT",
      "format_class": "data",
      "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "gpu_arena_velocity",
      "extent_scale": 1.0,
      "format": "R16G16_SFLOAT",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "gpu_arena_velocity_depth",
      "extent_scale": 1.0,
      "format": "D32_SFLOAT",
      "format_class": "data",
      "usage": ["DEPTH_STENCIL_ATTACHMENT"]
    }
  ],
  "buffers": [
    {
      "name": "gpu_arena_buffer",
      "size": 16,
      "lifetime": "persistent"
    }
  ],
  "rendering_passes": [
    {
      "name": "gpu_arena_main",
      "region_replacements": [
        {"region": "region.gpu_arena.output"}
      ],
      "passes": [
        {
          "name": "gpu_arena_shadow_pass",
          "type": "shadow_depth",
          "output": {
            "color": null,
            "depth": "gpu_arena_shadow"
          },
          "depth_store_op": "store",
          "shader": {
            "vertex": "engine://shadow_depth"
          }
        },
        {
          "name": "gpu_arena_velocity_pass",
          "type": "velocity",
          "output": {
            "color": "gpu_arena_velocity",
            "depth": "gpu_arena_velocity_depth"
          },
          "shader": {
            "vertex": "engine://velocity",
            "skinned_vertex": "engine://velocity_skinned",
            "fragment": "engine://velocity"
          }
        },
        {
          "name": "gpu_arena_present",
          "type": "fullscreen",
          "input": ["gpu_arena_buffer"],
          "output": {
            "color": "gpu_arena_scratch",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/gpu_arena_fullscreen",
            "fragment": "shaders/gpu_arena_buffer"
          }
        },
        {
          "name": "gpu_arena_output",
          "type": "fullscreen",
          "regions": ["region.gpu_arena.output"],
          "input": ["gpu_arena_scratch"],
          "output": {
            "color": "swapchain",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/gpu_arena_fullscreen",
            "fragment": "shaders/gpu_arena_copy"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "gpu_arena_compute",
      "shader": "shaders/gpu_arena_compute",
      "writes": ["gpu_arena_buffer"],
      "before": ["gpu_arena_present"],
      "dispatch": {"groups": [1, 1, 1]},
      "schedule": "per_frame"
    }
  ]
}
)json");
}

nlohmann::json depthPyramidRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [
    {
      "name": "depth_pyramid",
      "extent_scale": 1.0,
      "format": "R16G16B16A16_SFLOAT",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "STORAGE", "SAMPLED"],
      "mip_levels": "full",
      "layers": 2
    }
  ],
  "rendering_passes": [
    {
      "name": "depth_pyramid_main",
      "passes": [
        {
          "name": "depth_initialize",
          "type": "fullscreen",
          "output": {
            "color": "depth_pyramid",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/depth_pyramid_fullscreen",
            "fragment": "shaders/depth_pyramid_initialize"
          }
        },
        {
          "name": "depth_present",
          "type": "fullscreen",
          "after": ["depth_reduce"],
          "input": ["depth_pyramid"],
          "resource_ports": {
            "pyramid": {
              "resource": "depth_pyramid",
              "access": "sampled",
              "sampling": {
                "filter": "nearest",
                "address": "clamp_to_edge"
              },
              "subresource": {
                "mip": 1,
                "layer": 1
              }
            }
          },
          "output": {
            "color": "swapchain",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/depth_pyramid_fullscreen",
            "fragment": "shaders/depth_pyramid_present"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "depth_seed",
      "shader": "shaders/depth_pyramid_seed",
      "writes": ["depth_pyramid"],
      "after": ["depth_initialize"],
      "before": ["depth_reduce"],
      "resource_ports": {
        "seed_depth": {
          "resource": "depth_pyramid",
          "access": "storage",
          "subresource": {
            "mip": 0,
            "layer": 1
          }
        }
      },
      "dispatch": {
        "groups_from": {
          "port": "seed_depth"
        }
      },
      "schedule": "per_frame"
    },
    {
      "name": "depth_reduce",
      "shader": "shaders/depth_pyramid_reduce",
      "reads": ["depth_pyramid"],
      "writes": ["depth_pyramid"],
      "after": ["depth_seed"],
      "before": ["depth_present"],
      "resource_ports": {
        "source_depth": {
          "resource": "depth_pyramid",
          "access": "sampled",
          "sampling": {
            "filter": "nearest",
            "address": "clamp_to_edge"
          },
          "subresource": {
            "mip": 0,
            "layer": 1
          }
        },
        "reduced_depth": {
          "resource": "depth_pyramid",
          "access": "storage",
          "subresource": {
            "mip": 1,
            "layer": 1
          }
        }
      },
      "dispatch": {
        "groups_from": {
          "port": "reduced_depth"
        }
      },
      "schedule": "per_frame"
    }
  ]
}
)json");
}

nlohmann::json materialResourceRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [
    {
      "name": "material_tint",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "STORAGE", "SAMPLED"],
      "mip_levels": 2
    },
    {
      "name": "material_lit",
      "extent_scale": 1.0,
      "format": "R16G16B16A16_SFLOAT",
      "format_class": "explicit(R16G16B16A16_SFLOAT)",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "material_depth",
      "extent_scale": 1.0,
      "format": "D32_SFLOAT",
      "format_class": "data",
      "usage": ["DEPTH_STENCIL_ATTACHMENT"]
    }
  ],
  "buffers": [
    {
      "name": "material_displacement",
      "size": 16,
      "lifetime": "persistent"
    }
  ],
  "rendering_passes": [
    {
      "name": "material_resource_main",
      "passes": [
        {
          "name": "material_tint_source",
          "type": "fullscreen",
          "output": {
            "color": "material_tint",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/material_resource_fullscreen",
            "fragment": "shaders/material_resource_tint"
          }
        },
        {
          "name": "material_geometry",
          "type": "material",
          "material_contract": "forward_opaque_v1",
          "material_resources": {
            "displacement": {
              "resource": "material_displacement",
              "access": "storage",
              "footprint": "arbitrary"
            },
            "simulation_color": {
              "resource": "material_tint",
              "access": "sampled",
              "sampling": {
                "filter": "nearest",
                "address": "clamp_to_edge"
              },
              "subresource": {
                "mip": 0,
                "mip_count": "remaining"
              },
              "footprint": "arbitrary"
            }
          },
          "output": {
            "color": "material_lit",
            "depth": "material_depth"
          },
          "depth_store_op": "store"
        },
        {
          "name": "material_present",
          "type": "fullscreen",
          "input": ["material_lit"],
          "input_sampling": [
            {
              "filter": "nearest",
              "address": "clamp_to_edge"
            }
          ],
          "output": {
            "color": "swapchain",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/material_resource_fullscreen",
            "fragment": "shaders/material_resource_present"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "material_tint_mip",
      "shader": "shaders/material_resource_mip",
      "writes": ["material_tint"],
      "after": ["material_tint_source"],
      "before": ["material_geometry"],
      "resource_ports": {
        "tint_mip": {
          "resource": "material_tint",
          "access": "storage",
          "subresource": {
            "mip": 1
          }
        }
      },
      "dispatch": {
        "groups_from": {
          "port": "tint_mip"
        }
      },
      "schedule": "per_frame"
    },
    {
      "name": "material_deform",
      "shader": "shaders/material_resource_deform",
      "writes": ["material_displacement"],
      "before": ["material_geometry"],
      "dispatch": {
        "groups": [1, 1, 1]
      },
      "schedule": "per_frame"
    }
  ]
}
)json");
}

nlohmann::json pipelineReloadRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "features": ["features/reload_marker.json"],
  "shader_defines": ["WP196_INITIAL"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "pipeline_reload_main",
      "passes": [
        {
          "name": "pipeline_reload_present",
          "type": "fullscreen",
          "shader": {
            "vertex": "shaders/gpu_arena_fullscreen",
            "fragment": "shaders/pipeline_reload"
          },
          "output": {
            "color": "swapchain",
            "depth": null
          }
        }
      ]
    }
  ]
}
)json");
}

nlohmann::json wp353BlendRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [
    {
      "name": "wp353_omitted",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"]
    },
    {
      "name": "wp353_additive",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"]
    },
    {
      "name": "wp353_one_one",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"]
    }
  ],
  "rendering_passes": [{
    "name": "wp353_blend_main",
    "passes": [
      {
        "name": "wp353_seed_omitted",
        "type": "fullscreen",
        "output": {"color": "wp353_omitted", "depth": null},
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_destination"
        }
      },
      {
        "name": "wp353_seed_additive",
        "type": "fullscreen",
        "output": {"color": "wp353_additive", "depth": null},
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_destination"
        }
      },
      {
        "name": "wp353_seed_one_one",
        "type": "fullscreen",
        "output": {"color": "wp353_one_one", "depth": null},
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_destination"
        }
      },
      {
        "name": "wp353_overlay_omitted",
        "type": "fullscreen",
        "after": ["wp353_seed_omitted"],
        "color_load_op": "Load",
        "output": {"color": "wp353_omitted", "depth": null},
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_source"
        }
      },
      {
        "name": "wp353_overlay_additive",
        "type": "fullscreen",
        "after": ["wp353_seed_additive"],
        "color_load_op": "Load",
        "output": {"color": "wp353_additive", "depth": null},
        "raster_state": {
          "color_attachments": [{"blend": "additive", "write_mask": "rgba"}]
        },
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_source"
        }
      },
      {
        "name": "wp353_overlay_one_one",
        "type": "fullscreen",
        "after": ["wp353_seed_one_one"],
        "color_load_op": "Load",
        "output": {"color": "wp353_one_one", "depth": null},
        "raster_state": {
          "color_attachments": [{
            "blend": {
              "color": {"src": "one", "dst": "one", "op": "add"},
              "alpha": {"src": "one", "dst": "one", "op": "add"}
            },
            "write_mask": "rgba"
          }]
        },
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_source"
        }
      },
      {
        "name": "wp353_present",
        "type": "fullscreen",
        "input": ["wp353_omitted", "wp353_additive", "wp353_one_one"],
        "output": {"color": "swapchain", "depth": null},
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_present"
        }
      }
    ]
  }]
}
)json");
}

nlohmann::json wp353IntegerBlendEarlyFailureConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [{
    "name": "wp353_integer_target",
    "extent_scale": 1.0,
    "format": "R32_UINT",
    "format_class": "data",
    "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
  }],
  "rendering_passes": [{
    "name": "wp353_integer_main",
    "passes": [
      {
        "name": "wp353_integer_blend",
        "type": "fullscreen",
        "output": {"color": "wp353_integer_target", "depth": null},
        "raster_state": {
          "color_attachments": [{"blend": "additive"}]
        },
        "shader": {
          "vertex": "shaders/wp353_intentionally_missing",
          "fragment": "shaders/wp353_intentionally_missing"
        }
      },
      {
        "name": "wp353_integer_present",
        "type": "fullscreen",
        "input": ["wp353_integer_target"],
        "output": {"color": "swapchain", "depth": null},
        "shader": {
          "vertex": "shaders/wp353_fullscreen",
          "fragment": "shaders/wp353_present"
        }
      }
    ]
  }]
}
)json");
}

nlohmann::json localReadRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [
    {
      "name": "local_source",
      "extent_scale": 0.5,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "local_output",
      "extent_scale": 0.5,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    }
  ],
  "rendering_passes": [
    {
      "name": "local_read_main",
      "passes": [
        {
          "name": "local_producer",
          "type": "fullscreen",
          "resolution_domain": "independent",
          "output": {"color": "local_source", "depth": null},
          "shader": {
            "vertex": "shaders/local_read_fullscreen",
            "fragment": "shaders/local_read_producer"
          }
        },
        {
          "name": "local_consumer",
          "type": "fullscreen",
          "resolution_domain": "independent",
          "input": ["local_source"],
          "input_footprints": {
            "local_source": "same_pixel"
          },
          "output": {"color": "local_output", "depth": null},
          "raster_state": {
            "color_attachments": [{"blend": "blend", "write_mask": "rgba"}]
          },
          "resource_ports": {
            "input_color": {
              "resource": "local_source"
            }
          },
          "shader": {
            "vertex": "shaders/local_read_fullscreen",
            "fragment": "shaders/local_read_copy"
          }
        },
        {
          "name": "local_present",
          "type": "fullscreen",
          "input": ["local_output"],
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/local_read_fullscreen",
            "fragment": "shaders/local_read_present"
          }
        }
      ]
    }
  ]
}
)json");
}

nlohmann::json materialLocalReadRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [
    {
      "name": "material_local_source",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "material_local_output",
      "extent_scale": 1.0,
      "format": "R16G16B16A16_SFLOAT",
      "format_class": "explicit(R16G16B16A16_SFLOAT)",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "material_local_depth",
      "extent_scale": 1.0,
      "format": "D32_SFLOAT",
      "format_class": "data",
      "usage": ["DEPTH_STENCIL_ATTACHMENT"]
    }
  ],
  "rendering_passes": [
    {
      "name": "material_local_main",
      "passes": [
        {
          "name": "material_local_producer",
          "type": "fullscreen",
          "output": {
            "color": "material_local_source",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/material_local_fullscreen",
            "fragment": "shaders/material_local_producer"
          }
        },
        {
          "name": "material_local_consumer",
          "type": "material",
          "material_contract": "forward_opaque_v1",
          "material_resources": {
            "local_color": {
              "resource": "material_local_source",
              "access": "sampled",
              "sampling": {
                "filter": "nearest",
                "address": "clamp_to_edge"
              },
              "footprint": "same_pixel"
            }
          },
          "output": {
            "color": "material_local_output",
            "depth": "material_local_depth"
          }
        },
        {
          "name": "material_local_present",
          "type": "fullscreen",
          "input": ["material_local_output"],
          "input_sampling": [
            {
              "filter": "nearest",
              "address": "clamp_to_edge"
            }
          ],
          "output": {
            "color": "swapchain",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/material_local_fullscreen",
            "fragment": "shaders/material_local_present"
          }
        }
      ]
    }
  ]
}
)json");
}

nlohmann::json dependencySafePhysicalScopeRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "target_planning": {
    "graphs": {
      "physical_scope_main": {
        "nodes": {
          "physical_scope_overlay": {
            "isolate": true
          }
        }
      }
    }
  },
  "render_targets": [
    {
      "name": "physical_scope_color",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    }
  ],
  "buffers": [
    {
      "name": "physical_scope_buffer",
      "size": 16,
      "lifetime": "persistent"
    }
  ],
  "rendering_passes": [
    {
      "name": "physical_scope_main",
      "passes": [
        {
          "name": "physical_scope_base",
          "type": "fullscreen",
          "output": {
            "color": "physical_scope_color",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/physical_scope_fullscreen",
            "fragment": "shaders/physical_scope_base"
          }
        },
        {
          "name": "physical_scope_overlay",
          "type": "fullscreen",
          "color_load_op": "load",
          "color_store_op": "store",
          "output": {
            "color": "physical_scope_color",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/physical_scope_fullscreen",
            "fragment": "shaders/physical_scope_overlay"
          }
        },
        {
          "name": "physical_scope_present",
          "type": "fullscreen",
          "input": ["physical_scope_color"],
          "output": {
            "color": "swapchain",
            "depth": null
          },
          "shader": {
            "vertex": "shaders/physical_scope_fullscreen",
            "fragment": "shaders/physical_scope_present"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "physical_scope_independent",
      "shader": "shaders/physical_scope_independent",
      "writes": ["physical_scope_buffer"],
      "dispatch": {
        "groups": [1, 1, 1]
      },
      "schedule": "per_frame"
    }
  ]
}
)json");
}

nlohmann::json aliasLifetimeRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [
    {
      "name": "alias_a",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "alias_b",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    },
    {
      "name": "alias_sink",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT"]
    }
  ],
  "rendering_passes": [
    {
      "name": "alias_runtime_main",
      "passes": [
        {
          "name": "alias_produce_a",
          "type": "fullscreen",
          "output": {"color": "alias_a", "depth": null},
          "shader": {
            "vertex": "shaders/alias_fullscreen",
            "fragment": "shaders/alias_producer_a"
          }
        },
        {
          "name": "alias_consume_a",
          "type": "fullscreen",
          "input": ["alias_a"],
          "input_footprints": {"alias_a": "arbitrary"},
          "output": {"color": "alias_sink", "depth": null},
          "shader": {
            "vertex": "shaders/alias_fullscreen",
            "fragment": "shaders/alias_copy"
          }
        },
        {
          "name": "alias_produce_b",
          "type": "fullscreen",
          "after": ["alias_consume_a"],
          "output": {"color": "alias_b", "depth": null},
          "shader": {
            "vertex": "shaders/alias_fullscreen",
            "fragment": "shaders/alias_producer_b"
          }
        },
        {
          "name": "alias_consume_b",
          "type": "fullscreen",
          "input": ["alias_b"],
          "input_footprints": {"alias_b": "arbitrary"},
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/alias_fullscreen",
            "fragment": "shaders/alias_copy"
          }
        }
      ]
    }
  ]
}
)json");
}

RenderingPassConfigRegistrationDependencies
gpuArenaRegistrationDependencies(
    RenderingPassConfigRegistrationDependencies::Options
        options) {
    return {
        {GET_MODULE(RenderTargetContainer)},
        {
            GET_MODULE(RenderTarget),
            GET_MODULE(ShaderLibrary),
            GET_MODULE(FullscreenPassContainer),
            GET_MODULE(PipelineFactory),
            GET_MODULE(ShadowDepthPassContainer),
            GET_MODULE(VelocityPassContainer),
            GET_MODULE(PathResolver),
            {},
            true,
            []() -> DebugDraw & {
                return GET_MODULE(DebugDraw);
            },
            []() -> Gizmo & {
                return GET_MODULE(Gizmo);
            },
            []() -> DebugText & {
                return GET_MODULE(DebugText);
            },
        },
        GET_MODULE(FrameGraphResourceContainer),
        GET_MODULE(ComputeTaskContainer),
        GET_MODULE(FrameGraphRuntimeContainer),
        GET_MODULE(RenderingPassContainer),
        std::move(options),
    };
}

class DelegatingVulkanCompilerProgram final
    : public RenderCompilerProgram {
  public:
    mutable std::size_t compile_calls = 0;
    mutable std::vector<
        RenderPipelineGraphVariant>
        compiled_variants;
    mutable std::vector<
        RenderCompilerProgramArtifact>
        compiled_artifacts;

    RenderCompilerProgramSelection selection(
        const RenderCompilerBackendContext
            &backend_context) const override {
        auto selected =
            defaultVulkanRenderCompilerProgram()
                .selection(backend_context);
        selected.name =
            "test.delegating_vulkan";
        selected.implementation =
            "test.default_delegate_v1";
        return selected;
    }

    RenderCompilerProgramOutput compile(
        const RenderCompilerProgramInput
            &input) const override {
        ++compile_calls;
        compiled_variants.clear();
        compiled_artifacts.clear();
        for (const auto &variant :
             input.variants) {
            compiled_variants.push_back(
                variant.graph_variant);
            compiled_artifacts.push_back(
                variant.artifact);
        }
        return defaultVulkanRenderCompilerProgram()
            .compile(input);
    }
};

RenderPipelineGpuRegistrationDependencies
gpuArenaRegistryDependencies() {
    return {
        GET_MODULE(RenderTargetContainer),
        GET_MODULE(FrameGraphResourceContainer),
        GET_MODULE(ComputeTaskContainer),
        GET_MODULE(FullscreenPassContainer),
        GET_MODULE(ShaderLibrary),
        GET_MODULE(PipelineFactory),
        GET_MODULE(ShadowDepthPassContainer),
        GET_MODULE(VelocityPassContainer),
    };
}

} // namespace

TEST_CASE(
    "non-ray frame descriptor pool retains its exact allocation plan",
    "[headless][render][wp283][ray-query]") {
    const auto base_pool =
        makeFrameDescriptorPoolPlan(5, false);
    REQUIRE(base_pool.max_sets == 5);
    REQUIRE(base_pool.pool_sizes.size() == 2);
    REQUIRE(base_pool.pool_sizes[0].type ==
            vk::DescriptorType::eUniformBuffer);
    REQUIRE(base_pool.pool_sizes[0].descriptorCount == 15);
    REQUIRE(base_pool.pool_sizes[1].type ==
            vk::DescriptorType::eStorageBuffer);
    REQUIRE(base_pool.pool_sizes[1].descriptorCount == 15);

    const auto ray_pool =
        makeFrameDescriptorPoolPlan(5, true);
    REQUIRE(ray_pool.max_sets == 10);
    REQUIRE(ray_pool.pool_sizes.size() == 3);
    REQUIRE(ray_pool.pool_sizes[0].descriptorCount == 30);
    REQUIRE(ray_pool.pool_sizes[1].descriptorCount == 30);
    REQUIRE(ray_pool.pool_sizes[2].type ==
            vk::DescriptorType::eAccelerationStructureKHR);
    REQUIRE(ray_pool.pool_sizes[2].descriptorCount == 5);
}

TEST_CASE("GPU mip generation rejects missing linear-blit features with a named format",
          "[headless][render][mipmap]") {
    const auto required =
        vk::FormatFeatureFlagBits::eBlitSrc |
        vk::FormatFeatureFlagBits::eBlitDst |
        vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    REQUIRE_NOTHROW(VulkanUtils::requireLinearBlitSupport(
        vk::Format::eR8G8B8A8Unorm, required));
    REQUIRE_THROWS_WITH(
        VulkanUtils::requireLinearBlitSupport(
            vk::Format::eR8G8B8A8Unorm, {}),
        Catch::Matchers::ContainsSubstring("R8G8B8A8Unorm") &&
            Catch::Matchers::ContainsSubstring("BLIT_SRC") &&
            Catch::Matchers::ContainsSubstring("BLIT_DST") &&
            Catch::Matchers::ContainsSubstring(
                "SAMPLED_IMAGE_FILTER_LINEAR"));
}

TEST_CASE("headless render target renders and reads back RGBA8 frames", "[headless][render]") {
    setupLogger();
    std::filesystem::path temp_dir;

    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        const auto scene_path = temp_dir / "scene.json";
        const auto asset_path = temp_dir / "assets.json";
        writeTextFile(scene_path, "{}");
        writeTextFile(asset_path, "{}");

        GET_MODULE(ProjectSource).setSourceByData(makeProjectConfig(scene_path, asset_path).dump());

        auto &launch_config = GET_MODULE(EngineLaunchConfig);
        launch_config.headless = true;
        launch_config.headless_extent = vk::Extent2D{32, 32};
        launch_config.headless_frames = 3;

        auto &engine_time = GET_MODULE(EngineTime);
        engine_time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &render_target = GET_MODULE(RenderTarget);
        const std::array<uint8_t, 4> dual_use_pixel{188, 188, 188, 255};
        const auto dual_use_texture = GET_MODULE(MaterialContainer).registerTexture(
            vk::Extent3D{1, 1, 1}, dual_use_pixel.data());
        const auto [data_view, color_view] =
            MaterialContainerTestAccess::textureViews(GET_MODULE(MaterialContainer), dual_use_texture);
        REQUIRE(data_view);
        REQUIRE(color_view);
        REQUIRE(data_view != color_view);
        std::array<uint8_t, 4 * 4 * 4> checker_pixels{};
        for (std::size_t pixel = 0; pixel < 16; ++pixel) {
            const auto x = pixel % 4;
            const auto y = pixel / 4;
            const auto value = static_cast<uint8_t>(
                (x + y) % 2 == 0 ? 0 : 255);
            checker_pixels[pixel * 4 + 0] = value;
            checker_pixels[pixel * 4 + 1] = value;
            checker_pixels[pixel * 4 + 2] = value;
            checker_pixels[pixel * 4 + 3] = 255;
        }
        const auto mipmapped_texture =
            GET_MODULE(MaterialContainer).registerTexture(
                vk::Extent3D{4, 4, 1}, checker_pixels.data());
        REQUIRE(MaterialContainerTestAccess::textureMipLevels(GET_MODULE(MaterialContainer), mipmapped_texture) == 3);
        REQUIRE(MaterialContainerTestAccess::texturePixels(GET_MODULE(MaterialContainer),
                    mipmapped_texture, 0) ==
                std::vector<uint8_t>(checker_pixels.begin(),
                                     checker_pixels.end()));
        const auto require_neutral_filtered_mip = [](
            const std::vector<uint8_t> &pixels,
            std::size_t expected_pixel_count) {
            REQUIRE(pixels.size() == expected_pixel_count * 4);
            for (std::size_t pixel = 0;
                 pixel < expected_pixel_count; ++pixel) {
                INFO("filtered mip pixel " << pixel);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const auto value = pixels[pixel * 4 + channel];
                    REQUIRE(value >= 126);
                    REQUIRE(value <= 129);
                }
                REQUIRE(pixels[pixel * 4 + 3] == 255);
            }
        };
        require_neutral_filtered_mip(
            MaterialContainerTestAccess::texturePixels(GET_MODULE(MaterialContainer),
                mipmapped_texture, 1),
            4);
        require_neutral_filtered_mip(
            MaterialContainerTestAccess::texturePixels(GET_MODULE(MaterialContainer),
                mipmapped_texture, 2),
            1);
        const auto ktx_bytes = TestKtx2::makeRgba8Srgb188();
        const auto ktx_loaded = loadImageMemory(ktx_bytes, "headless-known-188.ktx2");
        REQUIRE(static_cast<unsigned>(ktx_loaded.pixels.front()) == 188);
        const auto ktx_texture = GET_MODULE(MaterialContainer).registerTexture(
            ktx_loaded, "headless-known-188.ktx2");
        const auto [ktx_data_view, ktx_color_view] =
            MaterialContainerTestAccess::textureViews(GET_MODULE(MaterialContainer), ktx_texture);
        REQUIRE(ktx_data_view);
        REQUIRE(ktx_color_view);
        REQUIRE(ktx_data_view != ktx_color_view);
        REQUIRE(MaterialContainerTestAccess::textureMipLevels(GET_MODULE(MaterialContainer), ktx_texture) == 2);
        REQUIRE(MaterialContainerTestAccess::texturePixels(GET_MODULE(MaterialContainer),
                    ktx_texture, 0) ==
                std::vector<uint8_t>(16, 188));
        REQUIRE(MaterialContainerTestAccess::texturePixels(GET_MODULE(MaterialContainer),
                    ktx_texture, 1) ==
                std::vector<uint8_t>(4, 188));
        const std::array clears{
            vk::ClearColorValue{std::array{1.0f, 0.0f, 0.0f, 1.0f}},
            vk::ClearColorValue{std::array{0.0f, 0.0f, 1.0f, 1.0f}},
            vk::ClearColorValue{std::array{0.0f, 1.0f, 0.0f, 1.0f}},
        };

        // A validation or pass-execution exception may unwind after begin.
        // The same in-flight slot must remain immediately reusable.
        auto abandoned =
            render_target.beginFrame(nullptr);
        REQUIRE(abandoned.frame.has_value());
        render_target.abandon(
            std::move(*abandoned.frame));

        for (const auto clear : clears) {
            engine_time.advance();
            renderClearFrame(render_target, clear);
        }

        const auto pixels = render_target.readbackLastFrameRGBA8();
        const auto extent = render_target.getExtent();
        REQUIRE(extent.width == 32);
        REQUIRE(extent.height == 32);
        REQUIRE(pixels.size() == static_cast<size_t>(extent.width) * extent.height * 4);
        REQUIRE(engine_time.frameIndex() == 3);

        size_t mismatched_pixels = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i + 0] != 0 || pixels[i + 1] != 255 || pixels[i + 2] != 0 || pixels[i + 3] != 255) {
                ++mismatched_pixels;
            }
        }
        REQUIRE(mismatched_pixels == 0);

        renderClearFrame(render_target,
                         vk::ClearColorValue{std::array{0.5f, 0.5f, 0.5f, 0.25f}});
        const auto known_value = render_target.readbackLastFrameRGBA8();
        size_t known_value_mismatches = 0;
        for (size_t i = 0; i < known_value.size(); i += 4) {
            if (known_value[i + 0] != 188 || known_value[i + 1] != 188 ||
                known_value[i + 2] != 188 || known_value[i + 3] != 64) {
                ++known_value_mismatches;
            }
        }
        REQUIRE(known_value_mismatches == 0);

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            ex, "Vulkan headless rendering unavailable");
        throw;
    }
}

TEST_CASE(
    "KTX2 cubemap material samples the declared face through Vulkan",
    "[headless][render][texture][wp209a]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        writeTextFile(
            temp_dir / "hybrid.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
            }, temp_dir).dump(2));

        auto project =
            makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        project["basic_config"]["rendering_config_json"] =
            "hybrid.json";
        project["basic_config"]["default_rendering_pass"] =
            "main_render";
        GET_MODULE(ProjectSource).setSourceByData(
            project.dump());
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        GET_MODULE(EngineTime).setup(
            EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        const auto &preview_program =
            renderer.previewGraphProgram();
        REQUIRE(
            preview_program.render_pipeline !=
            nullptr);
        REQUIRE(
            preview_program.render_pipeline
                ->render_compiler_program
                .has_value());
        CHECK(
            preview_program.graph_variant_policy
                .variant ==
            RenderPipelineGraphVariant::preview);
        CHECK(preview_program.generation != 0);
        const auto main_render_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName("main_render");
        const auto execution =
            GET_MODULE(FrameGraphRuntimeContainer)
                .find(main_render_id);
        REQUIRE(execution != nullptr);
        REQUIRE(execution->render_pipeline != nullptr);
        REQUIRE(
            execution->render_pipeline
                ->render_compiler_program
                .has_value());
        CHECK(
            *preview_program.render_pipeline
                 ->render_compiler_program ==
            *execution->render_pipeline
                 ->render_compiler_program);

        constexpr std::string_view surface_source =
            R"surface(//! pelican.surface v1
//! language: glsl
//! textures:
//!   - { name: environment, default: "project://textures/environment.ktx2", color_space: linear, dimension: cube, sampler: { filter: linear, mip_filter: nearest, address: clamp_to_edge, anisotropy: 4 } }

void pelican_surface_v1(in PelicanSurfaceInputV1 input_data,
                        inout PelicanSurfaceV1 surface) {
    surface.base_color = vec4(1.0);
    surface.emissive =
        pelican_sample_environment(
            vec3(1.0, 0.0, 0.0)).rgb;
}

vec3 pelican_lighting_v1(
    in PelicanSurfaceV1 surface,
    in PelicanSurfaceInputV1 input_data) {
    return surface.emissive;
}
)surface";
        constexpr std::string_view surface_reference =
            "project://shaders/cubemap.surface";
        const auto surface = parseSurfaceFormat(
            surface_source, surface_reference);
        const auto lowered = lowerSurfaceDefaults(
            surface, surface_reference);
        REQUIRE(lowered.route ==
                MaterialRouteClass::forward_opaque);
        const auto shaders =
            GET_MODULE(ShaderLibrary)
                .loadFromSurfaceForMaterial(
                    surface, surface_reference,
                    lowered,
                    execution->render_pipeline
                        ->shader_defines);

        constexpr std::array cube_texels{
            std::array<std::uint8_t, 4>{
                255, 0, 0, 255},
            std::array<std::uint8_t, 4>{
                0, 255, 0, 255},
            std::array<std::uint8_t, 4>{
                0, 0, 255, 255},
            std::array<std::uint8_t, 4>{
                255, 255, 0, 255},
            std::array<std::uint8_t, 4>{
                255, 0, 255, 255},
            std::array<std::uint8_t, 4>{
                0, 255, 255, 255},
        };
        const auto loaded_cube = loadImageMemory(
            TestKtx2::makeRgba8Unorm1x1(
                0, 0, 6, cube_texels),
            "environment.ktx2");
        auto &materials = GET_MODULE(MaterialContainer);
        const auto cube_texture =
            materials.registerTexture(
                loaded_cube, "environment.ktx2");
        REQUIRE(
            MaterialContainerTestAccess::textureDimension(materials,
                cube_texture) ==
            SurfaceTextureDimension::cube);
        REQUIRE(
            MaterialContainerTestAccess::textureViewType(materials,
                cube_texture) ==
            vk::ImageViewType::eCube);

        constexpr std::array array_texels{
            std::array<std::uint8_t, 4>{
                16, 32, 48, 255},
            std::array<std::uint8_t, 4>{
                64, 80, 96, 255},
            std::array<std::uint8_t, 4>{
                112, 128, 144, 255},
        };
        const auto array_texture =
            materials.registerTexture(
                loadImageMemory(
                    TestKtx2::makeRgba8Unorm1x1(
                        0, 3, 1, array_texels),
                    "layers.ktx2"),
                "layers.ktx2");
        REQUIRE(
            MaterialContainerTestAccess::textureDimension(materials,
                array_texture) ==
            SurfaceTextureDimension::two_d_array);
        REQUIRE(
            MaterialContainerTestAccess::textureViewType(materials,
                array_texture) ==
            vk::ImageViewType::e2DArray);

        constexpr std::array<
            std::array<std::uint8_t, 4>, 1>
            volume_texels{
            std::array<std::uint8_t, 4>{
                160, 176, 192, 255},
        };
        const auto volume_texture =
            materials.registerTexture(
                loadImageMemory(
                    TestKtx2::makeRgba8Unorm1x1(
                        1, 0, 1, volume_texels),
                    "volume.ktx2"),
                "volume.ktx2");
        REQUIRE(
            MaterialContainerTestAccess::textureDimension(materials,
                volume_texture) ==
            SurfaceTextureDimension::three_d);
        REQUIRE(
            MaterialContainerTestAccess::textureViewType(materials,
                volume_texture) ==
            vk::ImageViewType::e3D);

        const auto &standard =
            GET_MODULE(StandardMaterialResource);
        const auto make_material = [&] {
            return MaterialInfo{
                .vert_shader = shaders.vertex,
                .frag_shader = shaders.fragment,
                .base_color_texture =
                    standard.whiteTexture(),
                .metallic_roughness_texture =
                    standard
                        .metallicRoughnessDefaultTexture(),
                .normal_texture =
                    standard.normalDefaultTexture(),
                .emissive_texture =
                    standard.emissiveDefaultTexture(),
                .occlusion_texture =
                    standard.occlusionDefaultTexture(),
            };
        };

        auto mismatched = make_material();
        applyLoweredMaterialForRoute(
            mismatched, lowered,
            [&](std::string_view,
                SurfaceTextureRole) {
                return standard.whiteTexture();
            });
        REQUIRE_THROWS_WITH(
            materials.registerMaterial(
                std::move(mismatched)),
            Catch::Matchers::ContainsSubstring(
                "environment") &&
                Catch::Matchers::ContainsSubstring(
                    "declared dimension cube") &&
                Catch::Matchers::ContainsSubstring(
                    "loaded texture dimension 2d"));

        auto material = make_material();
        applyLoweredMaterialForRoute(
            material, lowered,
            [&](std::string_view reference,
                SurfaceTextureRole role) {
                REQUIRE(static_cast<bool>(
                    reference ==
                    "project://textures/environment.ktx2"));
                REQUIRE(
                    role == SurfaceTextureRole::data);
                return cube_texture;
            });
        const auto material_id =
            materials.registerMaterial(
                std::move(material));
        REQUIRE(isValidMaterialId(material_id));
        const auto sampler =
            MaterialContainerTestAccess::materialSamplerResolution(materials,
                material_id, "environment");
        REQUIRE(sampler.has_value());
        REQUIRE(sampler->filter ==
                SurfaceTextureFilter::linear);
        REQUIRE(sampler->mip_filter ==
                SurfaceTextureFilter::nearest);
        REQUIRE(sampler->address ==
                SurfaceTextureAddressMode::clamp_to_edge);
        REQUIRE((
            sampler->resolution == "exact" ||
            sampler->resolution ==
                "anisotropy_clamped_to_device_limit" ||
            sampler->resolution ==
                "anisotropy_disabled_feature_unavailable"));

        ModelTemplate model;
        model.asset_id = ModelAssetId{209};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = material_id,
                .primitives = {
                    GET_MODULE(VertBufContainer)
                        .addPrimitiveEntry(
                            makeScreenQuad(
                                0.8f, 0.0f))},
                .source_material_index = 0,
            },
        };
        const auto instance =
            GET_MODULE(PolygonInstanceContainer)
                .placeModelInstance(model);
        REQUIRE(
            GET_MODULE(PolygonInstanceContainer)
                .isModelInstanceAlive(instance));
        auto &camera = GET_MODULE(Camera);
        camera.setPos({0.0f, 0.0f, 2.0f});
        camera.setDir({0.0f, 0.0f, -1.0f});
        camera.setUp({0.0f, 1.0f, 0.0f});

        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(pixels.size() == 32u * 32u * 4u);
        const auto center =
            (16u * 32u + 16u) * 4u;
        REQUIRE(pixels[center] > 180);
        REQUIRE(pixels[center + 1] < 40);
        REQUIRE(pixels[center + 2] < 40);

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan cubemap rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "compute output displaces material vertices through typed resource ports",
    "[headless][render][material-resource][wp207b]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "shaders" /
                "material_resource_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "material_resource_tint.frag",
            materialResourceTintFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "material_resource_mip.comp",
            materialResourceMipComputeShader());
        writeTextFile(
            temp_dir / "shaders" /
                "material_resource_present.frag",
            gpuArenaCopyFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "material_resource_deform.comp",
            materialDisplacementComputeShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(materialResourceRenderingConfig(), temp_dir).dump(2));

        const auto surface_source = std::string{
            "//! pelican.surface v1\n"
            "//! language: glsl\n"
            "//! resource_ports:\n"
            "//!   - { name: displacement, kind: buffer, element: vec4, stage: vertex }\n"
            "//!   - { name: simulation_color, kind: image, stage: fragment }\n\n"
            "void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {\n"
            "    vertex.position += pelican_load_displacement(0u).xyz;\n"
            "}\n"
            "void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, "
            "inout PelicanSurfaceV1 surface) {\n"
            "    uint last_mip = pelican_mip_count_simulation_color() - 1u;\n"
            "    surface.base_color = pelican_sample_lod_simulation_color(\n"
            "        input_data.uv, float(last_mip));\n"
            "    surface.roughness = 1.0;\n"
            "}\n"
            "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
            "in PelicanSurfaceInputV1 input_data) {\n"
            "    return surface.base_color.rgb;\n"
            "}\n"};
        const auto surface_reference =
            std::string{
                "project://shaders/material_resource.surface"};
        writeTextFile(
            temp_dir / "shaders" /
                "material_resource.surface",
            surface_source);

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "material_resource_main";
        GET_MODULE(ProjectSource)
            .setSourceByData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false);
        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 3;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        auto &runtime =
            GET_MODULE(FrameGraphRuntimeContainer);
        const auto initial_generation =
            runtime.snapshot();
        REQUIRE(initial_generation != nullptr);
        const auto pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "material_resource_main");
        const auto program =
            initial_generation->find(pass_id);
        REQUIRE(program != nullptr);
        REQUIRE(std::any_of(
            program->frame_graph.plan.barriers.begin(),
            program->frame_graph.plan.barriers.end(),
            [](const auto &barrier) {
                return barrier.resource ==
                           "material_displacement" &&
                       barrier.from ==
                           "material_deform" &&
                       barrier.to ==
                           "material_geometry";
            }));

        const auto geometry_pass = std::find_if(
            program->rendering_pass.passes.begin(),
            program->rendering_pass.passes.end(),
            [](const auto &pass) {
                return pass.definition.name ==
                       "material_geometry";
            });
        REQUIRE(
            geometry_pass !=
            program->rendering_pass.passes.end());
        REQUIRE(
            geometry_pass->definition.materialInfo()
                .material_resources.size() == 2);
        const auto displacement_binding =
            std::find_if(
                geometry_pass->definition.materialInfo()
                    .material_resources.begin(),
                geometry_pass->definition.materialInfo()
                    .material_resources.end(),
                [](const auto &binding) {
                    return binding.port.name ==
                           "displacement";
                });
        REQUIRE(
            displacement_binding !=
            geometry_pass->definition.materialInfo()
                .material_resources.end());
        REQUIRE(isValidFrameGraphBufferId(
            displacement_binding->buffer_id));
        const auto initial_displacement_buffer =
            displacement_binding->buffer_id;

        const auto surface =
            parseSurfaceFormat(
                surface_source,
                surface_reference);
        MaterialDefinition definition;
        definition.name =
            "material_resource_quad";
        definition.surface =
            surface_reference;
        definition.render_path =
            MaterialRenderPath::forward;
        const auto lowered =
            lowerMaterial(definition, surface);
        REQUIRE(
            lowered.route ==
            MaterialRouteClass::forward_opaque);
        const auto shaders =
            GET_MODULE(ShaderLibrary)
                .loadFromSurfaceForMaterial(
                    surface, surface_reference,
                    lowered,
                    program->frame_graph
                        .render_pipeline
                        ->shader_defines);
        auto &materials =
            GET_MODULE(MaterialContainer);
        const std::array<std::uint8_t, 4>
            white_pixel{255, 255, 255, 255};
        const std::array<std::uint8_t, 4>
            normal_pixel{128, 128, 255, 255};
        const std::array<std::uint8_t, 4>
            black_pixel{0, 0, 0, 255};
        const auto white_texture =
            materials.registerTexture(
                vk::Extent3D{1, 1, 1},
                white_pixel.data());
        const auto normal_texture =
            materials.registerTexture(
                vk::Extent3D{1, 1, 1},
                normal_pixel.data());
        const auto black_texture =
            materials.registerTexture(
                vk::Extent3D{1, 1, 1},
                black_pixel.data());
        MaterialInfo material{
            .vert_shader = shaders.vertex,
            .frag_shader = shaders.fragment,
            .base_color_texture =
                white_texture,
            .metallic_roughness_texture =
                white_texture,
            .normal_texture =
                normal_texture,
            .emissive_texture =
                black_texture,
            .occlusion_texture =
                white_texture,
        };
        applyLoweredMaterialForRoute(
            material, lowered);
        const auto material_id =
            materials.registerMaterial(
                std::move(material));
        REQUIRE(isValidMaterialId(material_id));
        const auto initial_binding_revision =
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    geometry_pass->definition);
        REQUIRE(initial_binding_revision != 0);
        auto non_consumer_pass =
            geometry_pass->definition;
        non_consumer_pass.materialInfo().contract =
            MaterialPassContract::
                deferred_geometry_v1;
        REQUIRE_THROWS_WITH(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    non_consumer_pass),
            Catch::Matchers::ContainsSubstring(
                "incompatible pass"));
        const auto tint =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "material_tint");
        const ImageSubresourceRange
            full_tint_mips{
                .mip_count_mode =
                    ImageSubresourceMipCountMode::
                        remaining,
            };
        REQUIRE(
            GET_MODULE(RenderTargetContainer)
                .getMetadata(tint)
                .mip_levels == 2);
        const auto initial_tint_view =
            GET_MODULE(RenderTargetContainer)
                .getImageView(tint);
        const auto initial_tint_mip_view =
            GET_MODULE(RenderTargetContainer)
                .getImageSubresourceView(
                    tint, full_tint_mips,
                    ImageSubresourceViewDimension::two_d);
        REQUIRE(
            initial_tint_mip_view !=
            initial_tint_view);
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                    material_id,
                    geometry_pass->definition) ==
            std::vector<vk::ImageView>{
                initial_tint_mip_view});
        const auto tint_mip_task =
            GET_MODULE(ComputeTaskContainer)
                .getComputeTaskIdByName(
                    "material_tint_mip");
        REQUIRE(tint_mip_task.value >= 0);
        REQUIRE(
            GET_MODULE(ComputeTaskContainer)
                .dispatchGroupsForTesting(
                    tint_mip_task) ==
            (std::array<std::uint32_t, 3>{
                2, 2, 1}));

        auto &deletion_queue =
            GET_MODULE(DeletionQueue);
        const auto pending_before_material_rebind =
            deletion_queue.pendingCountForTesting();
        materials.rebindScreenInputs(
            RenderTargetImageViewResolver{
                GET_MODULE(RenderTargetContainer)});
        REQUIRE(
            deletion_queue.pendingCountForTesting() ==
            pending_before_material_rebind + 1);
        const auto direct_material_rebind_revision =
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    geometry_pass->definition);
        REQUIRE(
            direct_material_rebind_revision >
            initial_binding_revision);
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                    material_id,
                    geometry_pass->definition) ==
            std::vector<vk::ImageView>{
                initial_tint_mip_view});

        auto mismatched_view_pass =
            geometry_pass->definition;
        const auto mismatched_view_binding =
            std::find_if(
                mismatched_view_pass.materialInfo()
                    .material_resources.begin(),
                mismatched_view_pass.materialInfo()
                    .material_resources.end(),
                [](const auto &binding) {
                    return binding.port.name ==
                           "simulation_color";
                });
        REQUIRE(
            mismatched_view_binding !=
            mismatched_view_pass.materialInfo()
                .material_resources.end());
        mismatched_view_binding->port.view =
            ShaderResourcePortView::per_view;
        REQUIRE_THROWS_WITH(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    mismatched_view_pass),
            Catch::Matchers::ContainsSubstring(
                "requires per_view"));

        auto missing_port_pass =
            geometry_pass->definition;
        std::erase_if(
            missing_port_pass.materialInfo()
                .material_resources,
            [](const auto &binding) {
                return binding.port.name ==
                       "displacement";
            });
        REQUIRE_THROWS_WITH(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    missing_port_pass),
            Catch::Matchers::ContainsSubstring(
                "is not provided"));

        RendererTestAccess::recreateRenderTargetsAndRebind(renderer,
            {32, 32});
        REQUIRE(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    geometry_pass->definition) >
            direct_material_rebind_revision);
        const auto recreated_tint =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "material_tint");
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                    material_id,
                    geometry_pass->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getImageSubresourceView(
                        recreated_tint,
                        full_tint_mips,
                        ImageSubresourceViewDimension::two_d)});

        auto &geometry =
            GET_MODULE(VertBufContainer);
        ModelTemplate model;
        model.asset_id = ModelAssetId{207};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = material_id,
                .primitives = {
                    geometry.addPrimitiveEntry(
                        makeScreenQuad(
                            0.25f, 0.0f))},
                .source_material_index = 0,
            },
        };
        const auto instance =
            GET_MODULE(PolygonInstanceContainer)
                .placeModelInstance(model);
        REQUIRE(
            GET_MODULE(PolygonInstanceContainer)
                .isModelInstanceAlive(instance));
        auto &camera = GET_MODULE(Camera);
        camera.setPos(
            {0.0f, 0.0f, 2.0f});
        camera.setDir(
            {0.0f, 0.0f, -1.0f});
        camera.setUp(
            {0.0f, 1.0f, 0.0f});

        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        REQUIRE(
            deletion_queue.pendingCountForTesting() == 0);
        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            pixels.size() ==
            32u * 32u * 4u);
        std::size_t green_pixels = 0;
        std::size_t green_x_sum = 0;
        for (std::size_t pixel = 0;
             pixel < 32u * 32u; ++pixel) {
            const auto offset = pixel * 4u;
            if (pixels[offset + 1] >
                    pixels[offset] + 40 &&
                pixels[offset + 1] >
                    pixels[offset + 2] + 40) {
                ++green_pixels;
                green_x_sum += pixel % 32u;
            }
        }
        REQUIRE(green_pixels > 0);
        const auto green_centroid_x =
            static_cast<double>(green_x_sum) /
            static_cast<double>(green_pixels);
        INFO(
            "displaced green centroid x = " <<
            green_centroid_x);
        REQUIRE(green_centroid_x > 18.0);

        const auto resized_binding_revision =
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    geometry_pass->definition);
        auto replacement =
            materialResourceRenderingConfig();
        replacement["buffers"][0]["size"] =
            32;
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(replacement, temp_dir).dump(2));
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "pipeline.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        const auto reloaded_generation =
            runtime.snapshot();
        REQUIRE(reloaded_generation != nullptr);
        REQUIRE(
            reloaded_generation->generation ==
            initial_generation->generation + 1);
        const auto reloaded_pass_id =
            reloaded_generation->name_to_id.at(
                "material_resource_main");
        const auto reloaded_program =
            reloaded_generation->find(
                reloaded_pass_id);
        REQUIRE(reloaded_program != nullptr);
        const auto reloaded_geometry_pass =
            std::find_if(
                reloaded_program
                    ->rendering_pass.passes.begin(),
                reloaded_program
                    ->rendering_pass.passes.end(),
                [](const auto &pass) {
                    return pass.definition.name ==
                           "material_geometry";
                });
        REQUIRE(
            reloaded_geometry_pass !=
            reloaded_program
                ->rendering_pass.passes.end());
        const auto reloaded_displacement =
            std::find_if(
                reloaded_geometry_pass->definition
                    .materialInfo()
                    .material_resources.begin(),
                reloaded_geometry_pass->definition
                    .materialInfo()
                    .material_resources.end(),
                [](const auto &binding) {
                    return binding.port.name ==
                           "displacement";
                });
        REQUIRE(
            reloaded_displacement !=
            reloaded_geometry_pass->definition
                .materialInfo()
                .material_resources.end());
        REQUIRE(isValidFrameGraphBufferId(
            reloaded_displacement->buffer_id));
        REQUIRE(
            reloaded_displacement->buffer_id !=
            initial_displacement_buffer);
        const auto reloaded_binding_revision =
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    reloaded_geometry_pass
                        ->definition);
        REQUIRE(
            reloaded_binding_revision >
            resized_binding_revision);
        const auto reloaded_tint =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "material_tint");
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                    material_id,
                    reloaded_geometry_pass
                        ->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getImageSubresourceView(
                        reloaded_tint,
                        full_tint_mips,
                        ImageSubresourceViewDimension::two_d)});

        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto reloaded_pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(reloaded_pixels == pixels);

        auto invalid_replacement = replacement;
        invalid_replacement
            ["rendering_passes"][0]["passes"][1]
            ["material_resources"]["displacement"]
            ["resource"] =
                "missing_material_displacement";
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(invalid_replacement, temp_dir).dump(2));
        REQUIRE_FALSE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "pipeline.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        REQUIRE(
            runtime.snapshot() ==
            reloaded_generation);
        REQUIRE(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    reloaded_geometry_pass
                        ->definition) ==
            reloaded_binding_revision);

        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        REQUIRE(
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8() ==
            reloaded_pixels);

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(
            temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan material resource rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "fixed spatial upscale carries render/output extents and per-input sampling",
    "[headless][render][upscale][resolution-contract]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(temp_dir / "assets.json",
                      R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        std::filesystem::create_directories(
            temp_dir / "features");
        writeTextFile(
            temp_dir / "shaders" /
                "upscale_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "upscale_generate.frag",
            upscaleContractGeneratorFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "upscale_copy.frag",
            upscaleContractCopyFragmentShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(upscaleContractRenderingConfig(), temp_dir).dump(2));
        writeTextFile(
            temp_dir / "features" /
                "upscale_jitter.json",
            R"json({
  "schema":"pelican.render_feature",
  "version":1,
  "name":"upscale_jitter",
  "projection_jitter":{
    "pattern":"table",
    "offsets_px":[[-0.5,0.0]]
  }
})json");

        auto project =
            makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "upscale_main";
        project["schema"] = "pelican.project";
        project["version"] = 1;
        project["name"] =
            "upscale-resolution-contract";
        GET_MODULE(ProjectSource)
            .setProjectData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false, project.dump());

        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        const auto low_id =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "low_color");
        REQUIRE(
            GET_MODULE(RenderTargetContainer)
                .getMetadata(low_id)
                .extent ==
            vk::Extent2D{16, 16});
        REQUIRE(
            GET_MODULE(RenderTargetContainer)
                .getMetadata(low_id)
                .format ==
            vk::Format::eR8G8B8A8Unorm);
        const auto transient_id =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "transient_scratch");
        const auto transient_metadata =
            GET_MODULE(RenderTargetContainer)
                .getMetadata(transient_id);
        REQUIRE(
            transient_metadata.storage_mode ==
            RenderTargetStorageMode::
                transient_attachment);
        REQUIRE(
            transient_metadata.usage &
            vk::ImageUsageFlagBits::
                eTransientAttachment);
        REQUIRE_FALSE(
            transient_metadata.usage &
            vk::ImageUsageFlagBits::eSampled);

        const auto rendering_pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "upscale_main");
        const auto program =
            GET_MODULE(FrameGraphRuntimeContainer)
                .findProgram(rendering_pass_id);
        REQUIRE(program != nullptr);
        REQUIRE(
            program->frame_graph.target_plan !=
            nullptr);
        REQUIRE(
            program->frame_graph.target_plan
                ->resolution_plan.has_value());
        REQUIRE(
            program->frame_graph.target_plan
                ->resolution_plan
                ->render_source_resource ==
            "low_color");
        REQUIRE(
            program->frame_graph.target_plan
                ->backend_selection
                .selected_candidate ==
            "pelican.vulkan.transient_plan@1");
        const auto transient_resource =
            std::find_if(
                program->frame_graph.target_plan
                    ->resources.begin(),
                program->frame_graph.target_plan
                    ->resources.end(),
                [](const auto &resource) {
                    return resource.logical_resource ==
                           "transient_scratch";
                });
        REQUIRE(
            transient_resource !=
            program->frame_graph.target_plan
                ->resources.end());
        REQUIRE(
            transient_resource->representation ==
            VulkanResourceRepresentation::
                transient_attachment);
        const auto transient_attachment =
            std::find_if(
                program->frame_graph.target_plan
                    ->attachments.begin(),
                program->frame_graph.target_plan
                    ->attachments.end(),
                [](const auto &attachment) {
                    return attachment.node ==
                               "transient_probe" &&
                           attachment
                                   .logical_resource ==
                               "transient_scratch";
                });
        REQUIRE(
            transient_attachment !=
            program->frame_graph.target_plan
                ->attachments.end());
        REQUIRE(
            transient_attachment->store_op ==
            VulkanPhysicalAttachmentStoreOp::
                discard);
        const auto transient_pass =
            std::find_if(
                program->rendering_pass.passes.begin(),
                program->rendering_pass.passes.end(),
                [](const auto &pass) {
                    return pass.definition.name ==
                           "transient_probe";
                });
        REQUIRE(
            transient_pass !=
            program->rendering_pass.passes.end());
        REQUIRE(
            transient_pass->definition
                .colorAttachmentOperations(0)
                .store_op ==
            vk::AttachmentStoreOp::eDontCare);
        const auto upscale_pass =
            std::find_if(
                program->rendering_pass.passes.begin(),
                program->rendering_pass.passes.end(),
                [](const auto &pass) {
                    return pass.definition.name ==
                           "upscale";
                });
        REQUIRE(
            upscale_pass !=
            program->rendering_pass.passes.end());
        REQUIRE(
            GET_MODULE(FullscreenPassContainer)
                .inputSamplingForTesting(
                    upscale_pass->pass_id) ==
            std::vector<FullscreenInputSampling>{
                {FullscreenInputFilter::nearest,
                 FullscreenInputAddressMode::
                     clamp_to_edge}});

        const auto require_upscale_output = [] {
            const auto pixels =
                GET_MODULE(RenderTarget)
                    .readbackLastFrameRGBA8();
            REQUIRE(
                pixels.size() ==
                32u * 32u * 4u);
            std::size_t mismatches = 0;
            for (std::uint32_t y = 0; y < 32;
                 ++y) {
                for (std::uint32_t x = 0; x < 32;
                     ++x) {
                    const auto offset =
                        (static_cast<std::size_t>(y) *
                             32 +
                         x) *
                        4;
                    const std::array<
                        std::uint8_t, 4>
                        expected =
                            x < 16
                                ? std::array<
                                      std::uint8_t,
                                      4>{
                                      255, 0, 0,
                                      255}
                                : std::array<
                                      std::uint8_t,
                                      4>{
                                      0, 0, 255,
                                      255};
                    if (!std::equal(
                            expected.begin(),
                            expected.end(),
                            pixels.begin() +
                                static_cast<
                                    std::ptrdiff_t>(
                                    offset))) {
                        ++mismatches;
                    }
                }
            }
            REQUIRE(mismatches == 0);
        };

        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();

        bool found_resolution_contract = false;
        for (std::uint32_t frame = 0;
             frame < in_flight_frames_num;
             ++frame) {
            const auto &resolution =
                GET_MODULE(FrameResources)
                    .slotResolutionForTesting(
                        frame, 0);
            if (resolution.render_resolution.x ==
                    16.0f &&
                resolution.render_resolution.y ==
                    16.0f &&
                resolution.output_resolution.x ==
                    32.0f &&
                resolution.output_resolution.y ==
                    32.0f) {
                found_resolution_contract = true;
                const auto &frame_data =
                    GET_MODULE(FrameResources)
                        .slotDataForTesting(
                            frame, 0);
                REQUIRE(
                    frame_data.jitter_ndc.x ==
                    -0.0625f);
                REQUIRE(
                    frame_data.jitter_ndc.y ==
                    0.0f);
            }
        }
        REQUIRE(found_resolution_contract);

        require_upscale_output();

        auto physical_fragment =
            ejectVulkanPhysicalFragmentPackage(
                *program->frame_graph.target_plan);
        const auto low_fragment =
            std::find_if(
                physical_fragment.resources.begin(),
                physical_fragment.resources.end(),
                [](const auto &resource) {
                    return resource.logical_resource ==
                           "low_color";
                });
        REQUIRE(
            low_fragment !=
            physical_fragment.resources.end());
        low_fragment->format =
            vk::to_string(
                vk::Format::
                    eR16G16B16A16Sfloat);
        REQUIRE(
            physical_fragment.attachments
                .has_value());
        const auto low_attachment =
            std::find_if(
                physical_fragment.attachments
                    ->begin(),
                physical_fragment.attachments
                    ->end(),
                [](const auto &attachment) {
                    return attachment.node ==
                               "produce_low" &&
                           attachment
                                   .logical_resource ==
                               "low_color";
                });
        REQUIRE(
            low_attachment !=
            physical_fragment.attachments
                ->end());
        low_attachment->load_op =
            VulkanPhysicalAttachmentLoadOp::
                discard;

        auto replacement =
            upscaleContractRenderingConfig();
        replacement["vulkan_physical_fragments"] =
            nlohmann::json::object();
        replacement["vulkan_physical_fragments"]
                   ["flat"] =
            nlohmann::json::array(
                {vulkanPhysicalFragmentPackageToJson(
                    physical_fragment)});
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(replacement, temp_dir).dump(2));

        auto &runtime =
            GET_MODULE(FrameGraphRuntimeContainer);
        const auto before_generation =
            runtime.activeGeneration();
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "pipeline.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        REQUIRE(
            runtime.activeGeneration() ==
            before_generation + 1);

        const auto reloaded_low_id =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "low_color");
        REQUIRE(
            GET_MODULE(RenderTargetContainer)
                .getMetadata(reloaded_low_id)
                .format ==
            vk::Format::
                eR16G16B16A16Sfloat);
        const auto reloaded_pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "upscale_main");
        const auto reloaded_program =
            runtime.findProgram(
                reloaded_pass_id);
        REQUIRE(reloaded_program != nullptr);
        REQUIRE(
            reloaded_program->frame_graph
                .target_plan != nullptr);
        const auto reloaded_low_plan =
            std::find_if(
                reloaded_program->frame_graph
                    .target_plan->resources.begin(),
                reloaded_program->frame_graph
                    .target_plan->resources.end(),
                [](const auto &resource) {
                    return resource.logical_resource ==
                           "low_color";
                });
        REQUIRE(
            reloaded_low_plan !=
            reloaded_program->frame_graph
                .target_plan->resources.end());
        REQUIRE(
            reloaded_low_plan->format ==
            vk::to_string(
                vk::Format::
                    eR16G16B16A16Sfloat));
        const auto reloaded_transient_id =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "transient_scratch");
        const auto reloaded_transient_metadata =
            GET_MODULE(RenderTargetContainer)
                .getMetadata(
                    reloaded_transient_id);
        REQUIRE(
            reloaded_transient_metadata.storage_mode ==
            RenderTargetStorageMode::
                transient_attachment);
        REQUIRE(
            reloaded_transient_metadata.usage &
            vk::ImageUsageFlagBits::
                eTransientAttachment);
        const auto reloaded_transient_plan =
            std::find_if(
                reloaded_program->frame_graph
                    .target_plan->resources.begin(),
                reloaded_program->frame_graph
                    .target_plan->resources.end(),
                [](const auto &resource) {
                    return resource.logical_resource ==
                           "transient_scratch";
                });
        REQUIRE(
            reloaded_transient_plan !=
            reloaded_program->frame_graph
                .target_plan->resources.end());
        REQUIRE(
            reloaded_transient_plan
                ->representation ==
            VulkanResourceRepresentation::
                transient_attachment);
        const auto reloaded_produce_low =
            std::find_if(
                reloaded_program
                    ->rendering_pass.passes.begin(),
                reloaded_program
                    ->rendering_pass.passes.end(),
                [](const auto &pass) {
                    return pass.definition.name ==
                           "produce_low";
                });
        REQUIRE(
            reloaded_produce_low !=
            reloaded_program
                ->rendering_pass.passes.end());
        REQUIRE(
            reloaded_produce_low->definition
                .colorAttachmentOperations(0)
                .load_op ==
            vk::AttachmentLoadOp::eDontCare);

        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        require_upscale_output();

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan upscale rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "fullscreen integer blend fails before shader registration",
    "[wp353][headless][render][fullscreen][blend][integer]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "shaders" / "wp353_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" / "wp353_present.frag",
            gpuArenaCopyFragmentShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(wp353IntegerBlendEarlyFailureConfig(), temp_dir).dump(2));

        auto project =
            makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        project["basic_config"]["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]["default_rendering_pass"] =
            "wp353_integer_main";
        project["schema"] = "pelican.project";
        project["version"] = 1;
        project["name"] = "wp353-integer-blend";
        GET_MODULE(ProjectSource).setProjectData(
            project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false, project.dump());

        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{16, 16};
        launch.headless_frames = 1;
        GET_MODULE(EngineTime).setup(
            EngineTime::Mode::fixed_step, 1.0 / 60.0);

        REQUIRE_THROWS_WITH(
            (void)GET_MODULE(Renderer),
            Catch::Matchers::ContainsSubstring(
                "Fullscreen pass 'wp353_integer_blend'") &&
                Catch::Matchers::ContainsSubstring(
                    "integer color output 0"));

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan integer blend validation unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "fullscreen authored blend changes resolved factors and pixels",
    "[wp353][headless][render][fullscreen][blend]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "shaders" / "wp353_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" / "wp353_destination.frag",
            wp353BlendDestinationFragmentShader());
        writeTextFile(
            temp_dir / "shaders" / "wp353_source.frag",
            wp353BlendSourceFragmentShader());
        writeTextFile(
            temp_dir / "shaders" / "wp353_present.frag",
            wp353BlendPresentFragmentShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(wp353BlendRenderingConfig(), temp_dir).dump(2));

        auto project =
            makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        project["basic_config"]["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]["default_rendering_pass"] =
            "wp353_blend_main";
        project["schema"] = "pelican.project";
        project["version"] = 1;
        project["name"] = "wp353-fullscreen-blend";
        GET_MODULE(ProjectSource).setProjectData(
            project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false, project.dump());

        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{16, 16};
        launch.headless_frames = 1;
        auto &engine_time = GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        const auto rendering_pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "wp353_blend_main");
        const auto program =
            GET_MODULE(FrameGraphRuntimeContainer)
                .findProgram(rendering_pass_id);
        REQUIRE(program != nullptr);

        const auto find_pass =
            [&](std::string_view name)
            -> const CompiledPass & {
            const auto found = std::find_if(
                program->rendering_pass.passes.begin(),
                program->rendering_pass.passes.end(),
                [&](const auto &pass) {
                    return pass.definition.name == name;
                });
            REQUIRE(
                found !=
                program->rendering_pass.passes.end());
            return *found;
        };
        auto &fullscreen_passes =
            GET_MODULE(FullscreenPassContainer);
        const auto omitted =
            fullscreen_passes.graphicsPipelineDescForTesting(
                find_pass("wp353_overlay_omitted").pass_id);
        const auto additive =
            fullscreen_passes.graphicsPipelineDescForTesting(
                find_pass("wp353_overlay_additive").pass_id);
        const auto one_one =
            fullscreen_passes.graphicsPipelineDescForTesting(
                find_pass("wp353_overlay_one_one").pass_id);

        // This is the production compile contrast: omission retains the
        // legacy unspecified representation, while both authored states are
        // materialized and resolve to different source factors.
        REQUIRE(omitted.color_attachment_states.empty());
        REQUIRE(
            additive.color_attachment_states.size() == 1);
        REQUIRE(one_one.color_attachment_states.size() == 1);
        const auto &additive_state =
            additive.color_attachment_states.front();
        const auto &one_one_state =
            one_one.color_attachment_states.front();
        CHECK(additive_state.blend_enabled);
        CHECK(
            additive_state.source_color ==
            vk::BlendFactor::eSrcAlpha);
        CHECK(
            additive_state.destination_color ==
            vk::BlendFactor::eOne);
        CHECK(
            additive_state.source_alpha ==
            vk::BlendFactor::eOne);
        CHECK(
            additive_state.destination_alpha ==
            vk::BlendFactor::eOne);
        CHECK(one_one_state.blend_enabled);
        CHECK(
            one_one_state.source_color ==
            vk::BlendFactor::eOne);
        CHECK(
            one_one_state.destination_color ==
            vk::BlendFactor::eOne);
        CHECK(additive_state != one_one_state);

        engine_time.advance();
        renderer.render();
        auto &vkcore = GET_MODULE(VulkanManageCore);
        vkcore.waitIdle();

        const auto omitted_pixels =
            RendererTestAccess::readRenderTarget(renderer,
                "wp353_omitted", ImageSubresourceRange{});
        const auto additive_pixels =
            RendererTestAccess::readRenderTarget(renderer,
                "wp353_additive", ImageSubresourceRange{});
        const auto one_one_pixels =
            RendererTestAccess::readRenderTarget(renderer,
                "wp353_one_one", ImageSubresourceRange{});
        const auto require_center_pixel =
            [](const RenderTargetReadback &readback,
               const std::array<std::uint8_t, 4> &expected) {
            REQUIRE(
                readback.format ==
                vk::Format::eR8G8B8A8Unorm);
            REQUIRE(readback.layer_count == 1);
            REQUIRE(readback.image_layer_count == 1);
            REQUIRE(
                readback.bytes.size() ==
                static_cast<std::size_t>(
                    readback.extent.width) *
                    readback.extent.height * 4u);
            const auto pixel =
                (static_cast<std::size_t>(
                     readback.extent.height / 2u) *
                     readback.extent.width +
                 readback.extent.width / 2u) *
                4u;
            for (std::size_t channel = 0;
                 channel < expected.size(); ++channel) {
                const auto actual =
                    readback.bytes[pixel + channel];
                const auto delta = actual > expected[channel]
                                       ? actual - expected[channel]
                                       : expected[channel] - actual;
                CHECK(delta <= 1u);
            }
        };
        // destination=(.1,.2,.3,.4), source=(.4,.2,.1,.5).
        // Omitted overwrites; additive is SrcAlpha/One; explicit is One/One.
        require_center_pixel(
            omitted_pixels, {102, 51, 26, 128});
        require_center_pixel(
            additive_pixels, {77, 77, 89, 230});
        require_center_pixel(
            one_one_pixels, {128, 102, 102, 230});
        CHECK(
            omitted_pixels.bytes !=
            additive_pixels.bytes);
        CHECK(
            additive_pixels.bytes !=
            one_one_pixels.bytes);

        vkcore.waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan fullscreen blend rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "runtime target planner executes a fused tile-local scope",
    "[headless][render][tile-local][dynamic-rendering]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "shaders" /
                "local_read_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "local_read_producer.frag",
            localReadProducerFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "local_read_copy.frag",
            localReadCopyFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "local_read_present.frag",
            gpuArenaCopyFragmentShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(localReadRenderingConfig(), temp_dir).dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "local_read_main";
        project["schema"] = "pelican.project";
        project["version"] = 1;
        project["name"] =
            "tile-local-runtime";
        GET_MODULE(ProjectSource)
            .setProjectData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false, project.dump());

        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &vkcore =
            GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities()
                 .dynamic_rendering_local_read) {
            SKIP(
                "Vulkan device has no dynamic rendering local "
                "read support");
        }

        auto &renderer = GET_MODULE(Renderer);
        const auto source =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "local_source");
        const auto source_metadata =
            GET_MODULE(RenderTargetContainer)
                .getMetadata(source);
        REQUIRE(
            source_metadata.extent ==
            vk::Extent2D{16, 16});
        REQUIRE(
            source_metadata.extent !=
            launch.headless_extent);
        REQUIRE(
            source_metadata.storage_mode ==
            RenderTargetStorageMode::
                tile_local_attachment);
        REQUIRE(
            source_metadata.usage &
            vk::ImageUsageFlagBits::
                eInputAttachment);
        REQUIRE(
            source_metadata.usage &
            vk::ImageUsageFlagBits::
                eTransientAttachment);
        REQUIRE_FALSE(
            source_metadata.usage &
            vk::ImageUsageFlagBits::eSampled);

        const auto pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "local_read_main");
        const auto program =
            GET_MODULE(FrameGraphRuntimeContainer)
                .findProgram(pass_id);
        REQUIRE(program != nullptr);
        REQUIRE(
            program->frame_graph.target_plan !=
            nullptr);
        const auto &plan =
            *program->frame_graph.target_plan;
        REQUIRE(
            plan.backend_selection
                .selected_candidate ==
            "pelican.vulkan.tile_local_plan@1");
        REQUIRE(std::any_of(
            plan.scopes.begin(),
            plan.scopes.end(),
            [](const auto &scope) {
                return scope.nodes ==
                           std::vector<std::string>{
                               "local_producer",
                               "local_consumer"} &&
                       scope.local_reads ==
                           std::vector<std::string>{
                               "local_source"};
            }));
        REQUIRE(
            program->rendering_pass
                .passes.at(0)
                .rendering.local_read_scope);
        REQUIRE(
            program->rendering_pass
                .passes.at(1)
                .rendering.local_read_scope);
        REQUIRE_FALSE(
            program->rendering_pass
                .passes.at(2)
                .rendering.local_read_scope);
        const auto &consumer_pass =
            program->rendering_pass
                .passes.at(1);
        REQUIRE(
            consumer_pass.rendering
                .local_read_extent ==
            source_metadata.extent);
        REQUIRE(
            consumer_pass.rendering
                .color_attachment_locations ==
            std::vector<std::uint32_t>{
                unusedPhysicalAttachmentMapping, 0u});
        const auto consumer_pipeline =
            GET_MODULE(FullscreenPassContainer)
                .graphicsPipelineDescForTesting(
                    consumer_pass.pass_id);
        REQUIRE(
            consumer_pipeline.color_attachment_states.size() == 2);
        CHECK(
            consumer_pipeline.color_attachment_states[0]
                .write_mask == vk::ColorComponentFlags{});
        CHECK_FALSE(
            consumer_pipeline.color_attachment_states[0]
                .blend_enabled);
        CHECK(
            consumer_pipeline.color_attachment_states[1]
                .blend_enabled);
        CHECK(
            consumer_pipeline.color_attachment_states[1]
                .source_color ==
            vk::BlendFactor::eSrcAlpha);
        CHECK(
            consumer_pipeline.color_attachment_states[1]
                .destination_color ==
            vk::BlendFactor::eOneMinusSrcAlpha);
        const auto resource_interface =
            GET_MODULE(FullscreenPassContainer)
                .resourceInterfaceForTesting(
                    consumer_pass.pass_id);
        REQUIRE(resource_interface.size() == 1);
        REQUIRE(
            resource_interface.front().descriptor ==
            ShaderResourceDescriptorKind::
                input_attachment);
        REQUIRE(
            resource_interface.front()
                .input_attachment_extent ==
            source_metadata.extent);
        const auto fragment_shader =
            GET_MODULE(FullscreenPassContainer)
                .fragmentShaderForTesting(
                    consumer_pass.pass_id);
        const auto &fragment_bundle =
            GET_MODULE(ShaderLibrary)
                .get(fragment_shader);
        const auto generated_include =
            std::find_if(
                fragment_bundle.virtual_includes.begin(),
                fragment_bundle.virtual_includes.end(),
                [](const auto &include) {
                    return include.first ==
                           "pelican_resource_ports.glsl";
                });
        REQUIRE(
            generated_include !=
            fragment_bundle.virtual_includes.end());
        const auto active_size =
            generated_include->second.find(
                "ivec2 pelican_size_input_color() { return ivec2(16, 16); }");
        REQUIRE(active_size != std::string::npos);
        const auto inactive_begin =
            generated_include->second.find(
                "#else", active_size);
        REQUIRE(inactive_begin != std::string::npos);
        const auto inactive_end =
            generated_include->second.find(
                "#endif", inactive_begin);
        REQUIRE(inactive_end != std::string::npos);
        const auto inactive_accessors =
            generated_include->second.substr(
                inactive_begin,
                inactive_end - inactive_begin);
        REQUIRE(
            inactive_accessors.find(
                "ivec2 pelican_size_input_color() { return ivec2(16, 16); }") !=
            std::string::npos);
        REQUIRE(
            inactive_accessors.find(
                "ivec2 pelican_size_lod_input_color(int lod) { return ivec2(16, 16); }") !=
            std::string::npos);

        engine_time.advance();
        renderer.render();
        vkcore.waitIdle();
        bool observed_old_frame_size = false;
        for (std::uint32_t frame = 0;
             frame < in_flight_frames_num;
             ++frame) {
            const auto &resolution =
                GET_MODULE(FrameResources)
                    .slotResolutionForTesting(
                        frame, 0);
            if (resolution.render_resolution.x ==
                    32.0f &&
                resolution.render_resolution.y ==
                    32.0f) {
                observed_old_frame_size = true;
            }
        }
        REQUIRE(observed_old_frame_size);
        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            pixels.size() ==
            32u * 32u * 4u);
        std::size_t mismatches = 0;
        for (std::size_t offset = 0;
             offset < pixels.size();
             offset += 4) {
            // The offscreen frame target is sRGB: the linear
            // (0.8, 0.4, 0.2) local-read result is encoded on the
            // final attachment write.
            if (pixels[offset] != 231 ||
                pixels[offset + 1] != 170 ||
                pixels[offset + 2] != 124 ||
                pixels[offset + 3] != 255) {
                ++mismatches;
            }
        }
        INFO(
            "first pixel = (" <<
            static_cast<unsigned>(pixels[0]) << ", " <<
            static_cast<unsigned>(pixels[1]) << ", " <<
            static_cast<unsigned>(pixels[2]) << ", " <<
            static_cast<unsigned>(pixels[3]) << ")");
        REQUIRE(mismatches == 0);

        vkcore.waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan tile-local rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "material same-pixel resource executes through the tile-local input ABI",
    "[wp220][headless][render][material][tile-local]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "shaders" /
                "material_local_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "material_local_producer.frag",
            localReadProducerFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "material_local_present.frag",
            gpuArenaCopyFragmentShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(materialLocalReadRenderingConfig(), temp_dir).dump(2));

        constexpr std::string_view surface_source =
            R"surface(//! pelican.surface v1
//! language: glsl
//! resource_ports:
//!   - { name: local_color, kind: image, stage: fragment }
//! render_state: { blend: opaque, cull: none, depth: read_write }

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.base_color = vec4(1.0);
    surface.roughness = 1.0;
}

vec3 pelican_lighting_v1(
    in PelicanSurfaceV1 surface,
    in PelicanSurfaceInputV1 input_data) {
    return pelican_sample_local_color(input_data.uv).rgb;
}
)surface";
        constexpr std::string_view surface_reference =
            "project://shaders/material_local.surface";
        writeTextFile(
            temp_dir / "shaders" /
                "material_local.surface",
            std::string{surface_source});

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "material_local_main";
        GET_MODULE(ProjectSource)
            .setSourceByData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false);

        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &vkcore =
            GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities()
                 .dynamic_rendering_local_read) {
            std::filesystem::remove_all(
                temp_dir);
            temp_dir.clear();
            SKIP(
                "Vulkan device has no dynamic rendering local "
                "read support");
        }

        auto &renderer = GET_MODULE(Renderer);
        const auto pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "material_local_main");
        const auto program =
            GET_MODULE(FrameGraphRuntimeContainer)
                .findProgram(pass_id);
        REQUIRE(program != nullptr);
        REQUIRE(
            program->frame_graph.target_plan !=
            nullptr);
        REQUIRE(
            program->frame_graph.target_plan
                ->backend_selection
                .selected_candidate ==
            "pelican.vulkan.tile_local_plan@1");

        const auto source =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "material_local_source");
        const auto source_metadata =
            GET_MODULE(RenderTargetContainer)
                .getMetadata(source);
        REQUIRE(
            source_metadata.storage_mode ==
            RenderTargetStorageMode::
                tile_local_attachment);
        REQUIRE(
            source_metadata.usage &
            vk::ImageUsageFlagBits::
                eInputAttachment);
        REQUIRE_FALSE(
            source_metadata.usage &
            vk::ImageUsageFlagBits::eSampled);

        const auto consumer =
            std::find_if(
                program->rendering_pass
                    .passes.begin(),
                program->rendering_pass
                    .passes.end(),
                [](const auto &pass) {
                    return pass.definition.name ==
                           "material_local_consumer";
                });
        REQUIRE(
            consumer !=
            program->rendering_pass.passes.end());
        REQUIRE(
            consumer->rendering
                .local_read_scope);
        const auto source_slot =
            std::find(
                consumer->rendering
                    .color_attachments.begin(),
                consumer->rendering
                    .color_attachments.end(),
                source);
        REQUIRE(
            source_slot !=
            consumer->rendering
                .color_attachments.end());
        const auto source_slot_index =
            static_cast<std::size_t>(
                std::distance(
                    consumer->rendering
                        .color_attachments.begin(),
                    source_slot));
        REQUIRE(
            source_slot_index <
            consumer->rendering
                .color_attachment_input_indices
                .size());
        const auto physical_input_index =
            consumer->rendering
                .color_attachment_input_indices
                .at(source_slot_index);
        REQUIRE(
            physical_input_index !=
            unusedPhysicalAttachmentMapping);

        const auto surface =
            parseSurfaceFormat(
                surface_source,
                surface_reference);
        MaterialDefinition definition;
        definition.name =
            "material_local_quad";
        definition.surface =
            surface_reference;
        definition.render_path =
            MaterialRenderPath::forward;
        const auto lowered =
            lowerMaterial(
                definition, surface);
        REQUIRE(
            lowered.route ==
            MaterialRouteClass::
                forward_opaque);
        const auto shaders =
            GET_MODULE(ShaderLibrary)
                .loadFromSurfaceForMaterial(
                    surface,
                    surface_reference,
                    lowered,
                    program->frame_graph
                        .render_pipeline
                        ->shader_defines);
        const auto &fragment =
            GET_MODULE(ShaderLibrary)
                .get(shaders.fragment);
        const auto local_binding =
            std::find_if(
                fragment.reflection
                    .bindings.begin(),
                fragment.reflection
                    .bindings.end(),
                [](const auto &binding) {
                    return binding.set == 1 &&
                           binding.name ==
                               "pelican_resource_local_color";
                });
        REQUIRE(
            local_binding !=
            fragment.reflection
                .bindings.end());
        REQUIRE(
            local_binding->type ==
            vk::DescriptorType::
                eInputAttachment);
        REQUIRE(
            local_binding
                ->input_attachment_index ==
            physical_input_index);

        auto &materials =
            GET_MODULE(MaterialContainer);
        const std::array<std::uint8_t, 4>
            white_pixel{255, 255, 255, 255};
        const std::array<std::uint8_t, 4>
            normal_pixel{128, 128, 255, 255};
        const std::array<std::uint8_t, 4>
            black_pixel{0, 0, 0, 255};
        const auto white_texture =
            materials.registerTexture(
                vk::Extent3D{1, 1, 1},
                white_pixel.data());
        const auto normal_texture =
            materials.registerTexture(
                vk::Extent3D{1, 1, 1},
                normal_pixel.data());
        const auto black_texture =
            materials.registerTexture(
                vk::Extent3D{1, 1, 1},
                black_pixel.data());
        MaterialInfo material{
            .vert_shader = shaders.vertex,
            .frag_shader = shaders.fragment,
            .base_color_texture =
                white_texture,
            .metallic_roughness_texture =
                white_texture,
            .normal_texture =
                normal_texture,
            .emissive_texture =
                black_texture,
            .occlusion_texture =
                white_texture,
        };
        applyLoweredMaterialForRoute(
            material, lowered);
        const auto material_id =
            materials.registerMaterial(
                std::move(material));
        REQUIRE(
            isValidMaterialId(material_id));
        REQUIRE(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    material_id,
                    consumer->definition) != 0);
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                    material_id,
                    consumer->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getImageView(source)});

        auto &geometry =
            GET_MODULE(VertBufContainer);
        ModelTemplate model;
        model.asset_id =
            ModelAssetId{220};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = material_id,
                .primitives = {
                    geometry.addPrimitiveEntry(
                        makeScreenQuad(
                            0.6f, 0.0f))},
                .source_material_index = 0,
            },
        };
        const auto instance =
            GET_MODULE(
                PolygonInstanceContainer)
                .placeModelInstance(model);
        REQUIRE(
            GET_MODULE(
                PolygonInstanceContainer)
                .isModelInstanceAlive(
                    instance));
        auto &camera =
            GET_MODULE(Camera);
        camera.setPos(
            {0.0f, 0.0f, 2.0f});
        camera.setDir(
            {0.0f, 0.0f, -1.0f});
        camera.setUp(
            {0.0f, 1.0f, 0.0f});

        engine_time.advance();
        renderer.render();
        vkcore.waitIdle();
        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            pixels.size() ==
            32u * 32u * 4u);
        const auto center =
            (16u * 32u + 16u) * 4u;
        INFO(
            "material local-read center = (" <<
            static_cast<unsigned>(
                pixels[center]) << ", " <<
            static_cast<unsigned>(
                pixels[center + 1]) << ", " <<
            static_cast<unsigned>(
                pixels[center + 2]) << ", " <<
            static_cast<unsigned>(
                pixels[center + 3]) << ")");
        REQUIRE(
            pixels[center] >= 120);
        REQUIRE(
            pixels[center] <= 128);
        REQUIRE(
            pixels[center + 1] >= 166);
        REQUIRE(
            pixels[center + 1] <= 174);
        REQUIRE(
            pixels[center + 2] >= 227);
        REQUIRE(
            pixels[center + 2] <= 235);
        REQUIRE(
            pixels[center + 3] == 255);

        vkcore.waitIdle();
        std::filesystem::remove_all(
            temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan material local-read rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "dependency-safe physical scopes reorder and fuse real Vulkan rendering",
    "[headless][render][physical-scope][reorder][fusion]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "shaders" /
                "physical_scope_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "physical_scope_base.frag",
            physicalScopeBaseFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "physical_scope_overlay.frag",
            physicalScopeOverlayFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "physical_scope_present.frag",
            gpuArenaCopyFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "physical_scope_independent.comp",
            gpuArenaComputeShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(dependencySafePhysicalScopeRenderingConfig(), temp_dir).dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "physical_scope_main";
        project["schema"] = "pelican.project";
        project["version"] = 1;
        project["name"] =
            "dependency-safe-physical-scope";
        GET_MODULE(ProjectSource)
            .setProjectData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false, project.dump());

        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        auto &runtime =
            GET_MODULE(FrameGraphRuntimeContainer);
        const auto pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "physical_scope_main");
        const auto automatic_program =
            runtime.findProgram(pass_id);
        REQUIRE(automatic_program != nullptr);
        REQUIRE(
            automatic_program->frame_graph
                .target_plan != nullptr);
        const auto &automatic_plan =
            *automatic_program->frame_graph
                 .target_plan;
        const auto scope_for =
            [](const VulkanTargetPlan &plan,
               std::string_view node)
            -> const VulkanPhysicalScopePlan & {
            const auto found =
                std::find_if(
                    plan.scopes.begin(),
                    plan.scopes.end(),
                    [&](const auto &scope) {
                        return std::find(
                                   scope.nodes.begin(),
                                   scope.nodes.end(),
                                   node) !=
                               scope.nodes.end();
                    });
            if (found == plan.scopes.end()) {
                throw std::runtime_error(
                    "physical scope test node has no scope");
            }
            return *found;
        };
        REQUIRE(
            scope_for(
                automatic_plan,
                "physical_scope_base")
                .id !=
            scope_for(
                automatic_plan,
                "physical_scope_overlay")
                .id);

        auto fragment =
            ejectVulkanPhysicalFragmentPackage(
                automatic_plan);
        REQUIRE(fragment.schema_version == 3);
        REQUIRE(
            fragment.scope_edit_mode ==
            VulkanPhysicalScopeEditMode::
                dependency_safe);
        const auto contains_node =
            [](const VulkanPhysicalScopeFragment &scope,
               std::string_view node) {
                return std::find(
                           scope.nodes.begin(),
                           scope.nodes.end(),
                           node) !=
                       scope.nodes.end();
            };
        const auto independent_scope =
            std::find_if(
                fragment.scopes->begin(),
                fragment.scopes->end(),
                [&](const auto &scope) {
                    return contains_node(
                        scope,
                        "physical_scope_independent");
                });
        REQUIRE(
            independent_scope !=
            fragment.scopes->end());
        REQUIRE(
            independent_scope->nodes ==
            std::vector<std::string>{
                "physical_scope_independent"});
        std::vector<
            VulkanPhysicalScopeFragment>
            edited_scopes;
        auto reordered_independent =
            *independent_scope;
        reordered_independent.id =
            "manual:independent";
        edited_scopes.push_back(
            std::move(
                reordered_independent));
        bool fused_inserted = false;
        for (const auto &scope :
             *fragment.scopes) {
            if (contains_node(
                    scope,
                    "physical_scope_independent") ||
                contains_node(
                    scope,
                    "physical_scope_overlay")) {
                continue;
            }
            if (contains_node(
                    scope,
                    "physical_scope_base")) {
                REQUIRE(
                    scope.nodes ==
                    std::vector<std::string>{
                        "physical_scope_base"});
                edited_scopes.push_back({
                    .id =
                        "manual:base_overlay",
                    .nodes = {
                        "physical_scope_base",
                        "physical_scope_overlay"},
                });
                fused_inserted = true;
            } else {
                edited_scopes.push_back(
                    scope);
            }
        }
        REQUIRE(fused_inserted);
        fragment.scopes =
            std::move(edited_scopes);
        fragment.alias_groups =
            std::vector<
                VulkanPhysicalAliasGroupFragment>{};

        auto replacement =
            dependencySafePhysicalScopeRenderingConfig();
        replacement["vulkan_physical_fragments"] =
            nlohmann::json::object();
        replacement["vulkan_physical_fragments"]
                   ["flat"] =
            nlohmann::json::array(
                {vulkanPhysicalFragmentPackageToJson(
                    fragment)});
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(replacement, temp_dir).dump(2));

        const auto before_generation =
            runtime.activeGeneration();
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "pipeline.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        REQUIRE(
            runtime.activeGeneration() ==
            before_generation + 1);

        const auto reloaded_pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "physical_scope_main");
        const auto program =
            runtime.findProgram(
                reloaded_pass_id);
        REQUIRE(program != nullptr);
        REQUIRE(
            program->frame_graph.target_plan !=
            nullptr);
        const auto &plan =
            *program->frame_graph.target_plan;
        REQUIRE(
            plan.applied_fragment_package
                .has_value());
        REQUIRE(
            plan.scopes[0].nodes ==
            std::vector<std::string>{
                "physical_scope_independent"});
        const auto &fused_scope =
            scope_for(
                plan,
                "physical_scope_base");
        REQUIRE(
            fused_scope.nodes ==
            std::vector<std::string>{
                "physical_scope_base",
                "physical_scope_overlay"});
        REQUIRE(
            fused_scope
                .single_rendering_instance);
        REQUIRE(
            fused_scope.local_reads.empty());

        std::vector<std::string>
            lowering_order;
        for (const auto &node :
             plan.lowering_graph.nodes) {
            lowering_order.push_back(
                node.logical.name);
        }
        REQUIRE(
            lowering_order.front() ==
            "physical_scope_independent");
        const auto base_position =
            std::find(
                lowering_order.begin(),
                lowering_order.end(),
                "physical_scope_base");
        REQUIRE(
            base_position !=
            lowering_order.end());
        REQUIRE(
            std::next(base_position) !=
            lowering_order.end());
        REQUIRE(
            *std::next(base_position) ==
            "physical_scope_overlay");

        const auto compiled_pass =
            [&](std::string_view name)
            -> const CompiledPass & {
            const auto found =
                std::find_if(
                    program->rendering_pass
                        .passes.begin(),
                    program->rendering_pass
                        .passes.end(),
                    [&](const auto &pass) {
                        return pass.definition.name ==
                               name;
                    });
            if (found ==
                program->rendering_pass
                    .passes.end()) {
                throw std::runtime_error(
                    "physical scope test pass was not compiled");
            }
            return *found;
        };
        const auto &base =
            compiled_pass(
                "physical_scope_base");
        const auto &overlay =
            compiled_pass(
                "physical_scope_overlay");
        REQUIRE(
            base.rendering
                .fused_rendering_scope);
        REQUIRE(
            overlay.rendering
                .fused_rendering_scope);
        REQUIRE_FALSE(
            base.rendering.local_read_scope);
        REQUIRE(
            base.rendering.scope_id ==
            "manual:base_overlay");
        REQUIRE(
            overlay.rendering.scope_id ==
            base.rendering.scope_id);
        REQUIRE(
            base.rendering
                .scope_color_attachment_operations ==
            std::vector<
                PassAttachmentOperations>{
                PassAttachmentOperations{
                    vk::AttachmentLoadOp::
                        eClear,
                    vk::AttachmentStoreOp::
                        eStore,
                }});

        RendererTestAccess::setExecutionTracing(renderer,
            true);
        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore)
            .waitIdle();

        const auto &trace =
            RendererTestAccess::lastExecutionTrace(renderer);
        REQUIRE(trace.contains("nodes"));
        std::vector<std::string>
            executed_nodes;
        for (const auto &node :
             trace.at("nodes")) {
            executed_nodes.push_back(
                node.at("name")
                    .get<std::string>());
        }
        REQUIRE(
            executed_nodes ==
            lowering_order);

        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            pixels.size() ==
            32u * 32u * 4u);
        std::size_t mismatches = 0;
        for (std::uint32_t y = 0;
             y < 32; ++y) {
            for (std::uint32_t x = 0;
                 x < 32; ++x) {
                const auto offset =
                    (static_cast<std::size_t>(y) *
                         32 +
                     x) *
                    4;
                const auto expected =
                    x < 16
                        ? std::array<
                              std::uint8_t, 4>{
                              124, 170, 231,
                              255}
                        : std::array<
                              std::uint8_t, 4>{
                              231, 124, 170,
                              255};
                if (!std::equal(
                        expected.begin(),
                        expected.end(),
                        pixels.begin() +
                            static_cast<
                                std::ptrdiff_t>(
                                offset))) {
                    ++mismatches;
                }
            }
        }
        INFO(
            "left pixel = (" <<
            static_cast<unsigned>(
                pixels[0]) << ", " <<
            static_cast<unsigned>(
                pixels[1]) << ", " <<
            static_cast<unsigned>(
                pixels[2]) << ", " <<
            static_cast<unsigned>(
                pixels[3]) << ")");
        const auto right =
            (16u * 4u);
        INFO(
            "right pixel = (" <<
            static_cast<unsigned>(
                pixels[right]) << ", " <<
            static_cast<unsigned>(
                pixels[right + 1]) << ", " <<
            static_cast<unsigned>(
                pixels[right + 2]) << ", " <<
            static_cast<unsigned>(
                pixels[right + 3]) << ")");
        REQUIRE(mismatches == 0);

        GET_MODULE(VulkanManageCore)
            .waitIdle();
        std::filesystem::remove_all(
            temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan dependency-safe physical scope rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "runtime target planner executes aliased image lifetimes",
    "[headless][render][alias][lifetime]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "shaders" /
                "alias_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "alias_producer_a.frag",
            aliasProducerAFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "alias_producer_b.frag",
            aliasProducerBFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "alias_copy.frag",
            gpuArenaCopyFragmentShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(aliasLifetimeRenderingConfig(), temp_dir).dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "alias_runtime_main";
        project["schema"] = "pelican.project";
        project["version"] = 1;
        project["name"] =
            "alias-runtime";
        GET_MODULE(ProjectSource)
            .setProjectData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false, project.dump());

        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 2;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        auto &targets =
            GET_MODULE(RenderTargetContainer);
        const auto alias_a =
            targets.getRenderTargetIdByName(
                "alias_a");
        const auto alias_b =
            targets.getRenderTargetIdByName(
                "alias_b");
        const auto metadata_a =
            targets.getMetadata(alias_a);
        const auto metadata_b =
            targets.getMetadata(alias_b);
        REQUIRE(metadata_a.alias_group.has_value());
        REQUIRE(metadata_b.alias_group ==
                metadata_a.alias_group);
        REQUIRE(
            targets.aliasGroup(alias_a) ==
            targets.aliasGroup(alias_b));
        REQUIRE(
            targets.sharesAllocation(
                alias_a, alias_b));
        REQUIRE(
            targets.getImage(alias_a).image.get() !=
            targets.getImage(alias_b).image.get());

        const auto checkpoint =
            targets.checkpointRegistrations();
        targets.hideRegistrationName(
            "alias_a", alias_a);
        targets.hideRegistrationName(
            "alias_b", alias_b);
        auto candidate_a =
            RenderTargetDefinition{
                .name = "alias_a",
                .format_class = "data",
                .role = "data",
                .format =
                    vk::Format::eR8G8B8A8Unorm,
                .usage =
                    vk::ImageUsageFlagBits::
                            eColorAttachment |
                    vk::ImageUsageFlagBits::eSampled,
            };
        candidate_a.alias_group =
            metadata_a.alias_group;
        auto candidate_b = candidate_a;
        candidate_b.name = "alias_b";
        registerRenderTargetDefinitions(
            {candidate_a, candidate_b},
            {32, 32}, targets);
        const auto candidate_alias_a =
            targets.getRenderTargetIdByName(
                "alias_a");
        const auto candidate_alias_b =
            targets.getRenderTargetIdByName(
                "alias_b");
        REQUIRE(candidate_alias_a != alias_a);
        REQUIRE(candidate_alias_b != alias_b);
        REQUIRE(
            targets.aliasGroup(candidate_alias_a) ==
            targets.aliasGroup(candidate_alias_b));
        REQUIRE(
            targets.aliasGroup(candidate_alias_a) !=
            targets.aliasGroup(alias_a));
        REQUIRE(
            targets.sharesAllocation(
                candidate_alias_a,
                candidate_alias_b));
        REQUIRE_FALSE(
            targets.sharesAllocation(
                alias_a, candidate_alias_a));
        targets.rollbackRegistrations(checkpoint);
        REQUIRE(
            targets.getRenderTargetIdByName(
                "alias_a") == alias_a);
        REQUIRE(
            targets.getRenderTargetIdByName(
                "alias_b") == alias_b);
        REQUIRE(
            targets.sharesAllocation(
                alias_a, alias_b));

        const auto pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "alias_runtime_main");
        const auto program =
            GET_MODULE(FrameGraphRuntimeContainer)
                .findProgram(pass_id);
        REQUIRE(program != nullptr);
        REQUIRE(
            program->frame_graph.target_plan !=
            nullptr);
        REQUIRE(
            program->frame_graph.target_plan
                ->alias_groups.size() == 1);
        REQUIRE(
            program->frame_graph.target_plan
                ->alias_groups.front().resources ==
            std::vector<std::string>{
                "alias_a", "alias_b"});

        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        REQUIRE(
            RendererTestAccess::imageAliasDependencyCount(renderer) >=
            1);

        RendererTestAccess::recreateRenderTargetsAndRebind(renderer,
                {32, 32});
        REQUIRE(
            targets.sharesAllocation(
                alias_a, alias_b));
        REQUIRE(
            targets.getImage(alias_a).image.get() !=
            targets.getImage(alias_b).image.get());

        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        REQUIRE(
            RendererTestAccess::imageAliasDependencyCount(renderer) >=
            1);
        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            pixels.size() ==
            32u * 32u * 4u);
        std::size_t mismatches = 0;
        for (std::size_t offset = 0;
             offset < pixels.size();
             offset += 4) {
            if (pixels[offset] != 0 ||
                pixels[offset + 1] != 255 ||
                pixels[offset + 2] != 0 ||
                pixels[offset + 3] != 255) {
                ++mismatches;
            }
        }
        REQUIRE(mismatches == 0);

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan alias rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE("project-owned material variant renders a second opaque pass",
          "[headless][render][material-variant][wp206b]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(temp_dir / "features");
        std::filesystem::create_directories(temp_dir / "shaders");
        std::filesystem::create_directories(temp_dir / "materials");

        writeTextFile(
            temp_dir / "features" / "silhouette.json",
            nlohmann::json{
                {"schema", "pelican.render_feature"},
                {"version", 1},
                {"name", "silhouette"},
                {"passes",
                 nlohmann::json::array({
                     {
                         {"insert", "after:forward_opaque"},
                         {"pass",
                          {
                              {"name", "silhouette_overlay"},
                              {"type", "material"},
                              {"material_contract", "forward_opaque_v1"},
                              {"material_filter",
                               {{"include", {"outlined"}}}},
                              {"material_variant", "silhouette"},
                              {"output",
                               {{"color", {"lit_color"}},
                                {"depth", "scene_depth"}}},
                              {"color_load_op", "load"},
                              {"depth_load_op", "load"},
                              {"depth_store_op", "store"},
                          }},
                     },
                 })},
            }
                .dump(2));

        const auto base_surface_source = std::string{
            "//! pelican.surface v1\n"
            "//! language: glsl\n"
            "//! params:\n"
            "//!   - { name: tint, type: color, default: [0.02, 0.08, 1.0, 1.0], encoding: linear }\n\n"
            "void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, "
            "inout PelicanSurfaceV1 surface) {\n"
            "    surface.base_color = pelican_param_tint();\n"
            "    surface.emissive = pelican_param_tint().rgb;\n"
            "    surface.roughness = 0.8;\n"
            "}\n"};
        const auto variant_surface_source = std::string{
            "//! pelican.surface v1\n"
            "//! language: glsl\n"
            "//! params:\n"
            "//!   - { name: width, type: float, default: 0.12, min: 0.0, max: 0.5 }\n"
            "//!   - { name: color, type: color, default: [1.0, 0.01, 0.01, 1.0], encoding: linear }\n"
            "//! render_state: { blend: opaque, cull: front, depth: read_only, depth_compare: less_equal }\n\n"
            "void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {\n"
            "    vertex.position += vertex.normal * pelican_param_width();\n"
            "}\n"
            "vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, "
            "in PelicanSurfaceInputV1 input_data) {\n"
            "    return pelican_param_color().rgb;\n"
            "}\n"};
        writeTextFile(
            temp_dir / "shaders" / "base.surface",
            base_surface_source);
        writeTextFile(
            temp_dir / "shaders" / "silhouette.surface",
            variant_surface_source);

        const auto material_json = nlohmann::json{
            {"schema", "pelican.material"},
            {"version", 1},
            {"materials",
             nlohmann::json::array({
                 {
                     {"name", "outlined_cube"},
                     {"tags", {"outlined"}},
                     {"surface", "project://shaders/base.surface"},
                     {"values",
                      {{"tint", {0.02, 0.08, 1.0, 1.0}}}},
                     {"variants",
                      {
                          {"silhouette",
                           {
                               {"surface",
                                "project://shaders/silhouette.surface"},
                               {"render_path", "forward"},
                               {"values",
                                {
                                    {"width", 0.12},
                                    {"color",
                                     {1.0, 0.01, 0.01, 1.0}},
                                }},
                           }},
                      }},
                 },
             })},
        };
        writeTextFile(
            temp_dir / "materials" / "outlined.material.json",
            material_json.dump(2));
        writeTextFile(
            temp_dir / "hybrid.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array(
                     {"project://features/silhouette.json"})},
            }, temp_dir).dump(2));

        auto project =
            makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        project["basic_config"]["rendering_config_json"] =
            "hybrid.json";
        project["basic_config"]["default_rendering_pass"] =
            "main_render";
        GET_MODULE(ProjectSource).setSourceByData(
            project.dump());
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        GET_MODULE(EngineTime).setup(
            EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        const auto main_render_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName("main_render");
        const auto execution =
            GET_MODULE(FrameGraphRuntimeContainer)
                .find(main_render_id);
        REQUIRE(execution != nullptr);
        REQUIRE(execution->render_pipeline != nullptr);

        const auto base_reference =
            std::string{"project://shaders/base.surface"};
        const auto variant_reference =
            std::string{"project://shaders/silhouette.surface"};
        const auto base_surface = parseSurfaceFormat(
            base_surface_source, base_reference);
        const auto variant_surface = parseSurfaceFormat(
            variant_surface_source, variant_reference);
        const MaterialSurfaceCatalog surfaces{
            {base_reference, base_surface},
            {variant_reference, variant_surface},
        };
        const auto document = parseMaterialFormatJson(
            material_json, surfaces);
        REQUIRE(document.materials.size() == 1);
        const auto &definition = document.materials.front();
        const auto base_lowered =
            lowerMaterial(definition, base_surface);
        const auto variants =
            lowerMaterialVariants(definition, surfaces);
        REQUIRE(base_lowered.route ==
                MaterialRouteClass::deferred_geometry);
        REQUIRE(variants.size() == 1);
        REQUIRE(variants.front().material.route ==
                MaterialRouteClass::forward_opaque);

        const auto base_shaders =
            GET_MODULE(ShaderLibrary).loadFromSurfaceForMaterial(
                base_surface, base_reference, base_lowered,
                execution->render_pipeline->shader_defines);
        const auto variant_shaders =
            GET_MODULE(ShaderLibrary).loadFromSurfaceForMaterial(
                variant_surface, variant_reference,
                variants.front().material,
                execution->render_pipeline->shader_defines);
        const auto &standard =
            GET_MODULE(StandardMaterialResource);
        const auto make_material =
            [&](const LoweredMaterial &lowered,
                SurfaceShaderBundleIds shaders) {
                MaterialInfo info{
                    .vert_shader = shaders.vertex,
                    .frag_shader = shaders.fragment,
                    .base_color_texture =
                        standard.whiteTexture(),
                    .metallic_roughness_texture =
                        standard
                            .metallicRoughnessDefaultTexture(),
                    .normal_texture =
                        standard.normalDefaultTexture(),
                    .emissive_texture =
                        standard.emissiveDefaultTexture(),
                    .occlusion_texture =
                        standard.occlusionDefaultTexture(),
                };
                applyLoweredMaterialForRoute(info, lowered);
                return info;
            };
        auto &materials = GET_MODULE(MaterialContainer);
        const auto base = materials.registerMaterial(
            make_material(base_lowered, base_shaders));
        const auto base_record =
            MaterialContainerTestAccess::materialGpuRecord(materials, base);
        materials.registerMaterialVariants(
            base,
            {{
                variants.front().name,
                make_material(
                    variants.front().material,
                    variant_shaders),
            }});
        const auto variant =
            materials.materialVariantResource(
                base, "silhouette");
        REQUIRE(variant != base);
        const auto base_record_after =
            MaterialContainerTestAccess::materialGpuRecord(materials, base);
        REQUIRE(base_record_after.size() ==
                base_record.size());
        REQUIRE(std::equal(
            base_record_after.begin(),
            base_record_after.end(),
            base_record.begin()));

        const auto &compiled =
            GET_MODULE(RenderingPassContainer)
                .getCompiledRenderingPass(main_render_id);
        const auto overlay = std::find_if(
            compiled.passes.begin(), compiled.passes.end(),
            [](const auto &pass) {
                return pass.definition.name ==
                       "silhouette_overlay";
            });
        REQUIRE(overlay != compiled.passes.end());
        REQUIRE(
            overlay->definition.materialInfo()
                .material_variant ==
            "silhouette");
        REQUIRE(materials.resolveMaterialForPass(
                    overlay->definition, base) ==
                variant);
        REQUIRE(materials.isRenderRequired(
            overlay->definition, base));

        auto &geometry = GET_MODULE(VertBufContainer);
        ModelTemplate model;
        model.asset_id = ModelAssetId{206};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = base,
                .primitives = {
                    geometry.addPrimitiveEntry(
                        makeOutlineCube(0.55f))},
                .source_material_index = 0,
            },
        };
        const auto instance =
            GET_MODULE(PolygonInstanceContainer)
                .placeModelInstance(model);
        REQUIRE(
            GET_MODULE(PolygonInstanceContainer)
                .isModelInstanceAlive(instance));
        auto &camera = GET_MODULE(Camera);
        camera.setPos({0.0f, 0.0f, 2.0f});
        camera.setDir({0.0f, 0.0f, -1.0f});
        camera.setUp({0.0f, 1.0f, 0.0f});

        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(pixels.size() == 32u * 32u * 4u);
        std::size_t base_pixels = 0;
        std::size_t variant_pixels = 0;
        for (std::size_t offset = 0;
             offset < pixels.size(); offset += 4) {
            const auto red = pixels[offset];
            const auto green = pixels[offset + 1];
            const auto blue = pixels[offset + 2];
            if (blue > red + 20 && blue > green + 20) {
                ++base_pixels;
            }
            if (red > blue + 20 && red > green + 20) {
                ++variant_pixels;
            }
        }
        REQUIRE(base_pixels > 0);
        REQUIRE(variant_pixels > 0);

        const auto plan = renderer.currentFramePlanJson();
        const auto plan_overlay = std::find_if(
            plan.at("nodes").begin(),
            plan.at("nodes").end(),
            [](const auto &node) {
                return node.value("name", "") ==
                       "silhouette_overlay";
            });
        REQUIRE(plan_overlay !=
                plan.at("nodes").end());
        REQUIRE(plan_overlay->at("material_variant") ==
                "silhouette");

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan material variant rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "clustered lighting uploads and selects more than 32 lights for every hybrid consumer",
    "[headless][render][clustered][lighting-data][wp208]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        writeTextFile(
            temp_dir / "hybrid.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array(
                     {"engine://features/clustered_lighting.json"})},
                {"xr",
                 {{"view_execution", "sequential"}}},
            }, temp_dir).dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "hybrid.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "main_render";
        GET_MODULE(ProjectSource).setSourceByData(
            project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false);
        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        // This fixture exercises the precompiled XR graph without starting
        // an OpenXR runtime. Initialize ordinary Vulkan first, then request
        // both graph variants before Renderer construction.
        (void)GET_MODULE(VulkanManageCore);
        launch.xr_active = true;
        GET_MODULE(EngineTime).setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        const auto rendering_pass =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "main_render");
        const auto execution =
            GET_MODULE(FrameGraphRuntimeContainer)
                .find(rendering_pass);
        REQUIRE(execution != nullptr);
        REQUIRE(
            execution->render_pipeline != nullptr);
        REQUIRE(
            execution->render_pipeline
                ->lighting_data.has_value());
        REQUIRE(
            execution->render_pipeline
                ->lighting_data
                ->overflow ==
            LightingOverflowPolicy::
                deterministic_truncate);
        const auto &compiled_rendering_pass =
            GET_MODULE(RenderingPassContainer)
                .getCompiledRenderingPass(
                    rendering_pass);

        const auto require_selection_input =
            [&](std::string_view pass_name) {
                const auto pass = std::find_if(
                    compiled_rendering_pass
                        .passes.begin(),
                    compiled_rendering_pass
                        .passes.end(),
                    [&](const auto &candidate) {
                        return candidate.definition
                                   .name ==
                               pass_name;
                    });
                REQUIRE(
                    pass !=
                    compiled_rendering_pass
                        .passes.end());
                REQUIRE(
                    std::find(
                        pass->definition
                            .input_buffers.begin(),
                        pass->definition
                            .input_buffers.end(),
                        "clustered_light_selection") !=
                    pass->definition
                        .input_buffers.end());
            };
        require_selection_input(
            "deferred_lighting");
        require_selection_input(
            "forward_opaque");
        require_selection_input(
            "forward_transparent");

        std::vector<LightLoadEntry> lights;
        lights.reserve(70);
        for (std::uint32_t index = 0;
             index < 70; ++index) {
            lights.push_back(
                LightLoadEntry{
                    .name =
                        "Directional" +
                        std::to_string(index),
                    .component = LightCodecData{
                        .type = LightCodecType::Directional,
                        .direction = {0.0f, 0.0f, -1.0f},
                        .intensity = 1.0f,
                        .color = {1.0f, 1.0f, 1.0f}},
                });
        }
        GET_MODULE(LightContainer).load(lights);

        auto &resources =
            GET_MODULE(
                FrameGraphResourceContainer);
        REQUIRE(resources.hasBuffer(
            "clustered_light_inventory"));
        REQUIRE(resources.hasBuffer(
            "clustered_light_selection"));
        REQUIRE(
            resources.bufferSize(
                "clustered_light_selection") ==
            2u * (48u + 260u));

        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();

        const auto inventory_id =
            resources.getBufferIdByName(
                "clustered_light_inventory");
        const auto population =
            resources.hostBufferPopulation(
                inventory_id);
        REQUIRE(population.has_value());
        REQUIRE(
            population->source_records == 70);
        REQUIRE(
            population->written_records == 70);
        const auto inventory =
            GET_MODULE(VulkanManageCore).readBuf(
                resources.buffer(inventory_id), 32);
        REQUIRE(
            wordAt(inventory, 0) ==
            lightInventoryV2Magic);
        REQUIRE(
            wordAt(inventory, 1) ==
            lightInventoryV2Version);
        REQUIRE(wordAt(inventory, 2) == 70);
        REQUIRE(wordAt(inventory, 3) == 0);
        REQUIRE(wordAt(inventory, 4) == 70);

        const auto selection =
            readFrameGraphBuffer(
                "clustered_light_selection");
        REQUIRE(
            wordAt(selection, 0) ==
            0x504C5332u);
        REQUIRE(wordAt(selection, 1) == 2);
        REQUIRE(wordAt(selection, 2) == 1);
        REQUIRE(wordAt(selection, 3) == 1);
        REQUIRE(wordAt(selection, 4) == 32);
        REQUIRE(wordAt(selection, 5) == 32);
        REQUIRE(wordAt(selection, 6) == 64);
        REQUIRE(wordAt(selection, 7) == 70);
        REQUIRE(wordAt(selection, 8) == 0);
        REQUIRE(wordAt(selection, 9) == 1);
        const auto main_family_token =
            renderViewFamilyToken(
                mainRenderViewFamilyId);
        REQUIRE(
            wordAt(selection, 10) ==
            main_family_token[0]);
        REQUIRE(
            wordAt(selection, 11) ==
            main_family_token[1]);
        const auto encoded_count =
            wordAt(selection, 12);
        REQUIRE(
            (encoded_count & 0x7fffffffu) ==
            64);
        REQUIRE(
            (encoded_count & 0x80000000u) !=
            0);
        for (std::uint32_t index = 0;
             index < 64; ++index) {
            REQUIRE(
                wordAt(selection, 13 + index) ==
                index);
        }

        const auto order =
            RendererTestAccess::currentFramePlanOrder(renderer);
        const auto selector =
            std::find(
                order.begin(), order.end(),
                "clustered_light_select");
        REQUIRE(selector != order.end());
        for (const auto consumer :
             {"deferred_lighting",
              "forward_opaque",
              "forward_transparent"}) {
            const auto found =
                std::find(
                    order.begin(), order.end(),
                    consumer);
            REQUIRE(found != order.end());
            REQUIRE(selector < found);
        }
        const auto plan =
            renderer.currentFramePlanJson();
        REQUIRE(
            plan.at("lighting_data")
                .at("selection_contract") ==
            "project.clustered_selection_v2");
        REQUIRE(
            plan.at("lighting_data_runtime")
                .at("selected_path") ==
            "compute_clustered");
        REQUIRE(
            plan.at("lighting_data_runtime")
                .at("declared_vram_bytes") ==
            65568u + 616u);
        REQUIRE(
            plan.at("lighting_data_runtime")
                .at("inventory")
                .at("source_records") == 70);
        REQUIRE(
            plan.at("lighting_data_runtime")
                .at("inventory")
                .at("written_records") == 70);
        REQUIRE(
            plan.at("lighting_data_runtime")
                .at("storage_buffer_contract")
                .at("supported") == true);
        REQUIRE(
            plan.at("lighting_data_runtime")
                .at("overflow")
                .at("selection_signal") ==
            "tile_count_high_bit");
        REQUIRE(
            plan.at("lighting_data_runtime")
                .at("fallbacks")
                .at("tile_gpu")
                .at("path") ==
            "small_light_v1");
        REQUIRE_FALSE(
            plan.at("lighting_data_runtime")
                .at("fallbacks")
                .at("tile_gpu")
                .at("reason")
                .get<std::string>()
                .empty());
        for (const auto consumer :
             {"deferred_lighting",
              "forward_opaque",
              "forward_transparent"}) {
            REQUIRE(std::any_of(
                plan.at("barriers").begin(),
                plan.at("barriers").end(),
                [&](const auto &barrier) {
                    return barrier.at("from") ==
                               "clustered_light_select" &&
                           barrier.at("to") ==
                               consumer &&
                           barrier.at("resource") ==
                               "clustered_light_selection";
                }));
        }

        std::array<RenderViewParameters, 2>
            stereo_views;
        for (std::uint32_t view_index = 0;
             view_index < stereo_views.size();
             ++view_index) {
            const auto eye_x =
                view_index == 0 ? -0.04f : 0.04f;
            stereo_views[view_index].view =
                glm::lookAt(
                    glm::vec3{eye_x, 0.0f, 2.0f},
                    glm::vec3{eye_x, 0.0f, 0.0f},
                    glm::vec3{0.0f, 1.0f, 0.0f});
            stereo_views[view_index].projection =
                glm::perspective(
                    glm::radians(70.0f),
                    1.0f, 0.1f, 100.0f);
            stereo_views[view_index]
                .projection[1][1] *= -1.0f;
            stereo_views[view_index]
                .camera_position =
                {eye_x, 0.0f, 2.0f};
            stereo_views[view_index].view_id =
                "$xr/" +
                std::to_string(view_index);
        }
        const RenderViewFamily stereo_family{
            .family_id =
                std::string{
                    mainRenderViewFamilyId},
            .views = {
                stereo_views.begin(),
                stereo_views.end()},
        };
        renderer.selectGraphVariant(
            RenderGraphVariant::xr);
        REQUIRE(
            std::find(
                renderer.xrExcludedFeatures()
                    .begin(),
                renderer.xrExcludedFeatures()
                    .end(),
                "clustered_lighting") ==
            renderer.xrExcludedFeatures().end());
        RendererTestAccess::setExecutionTracing(renderer,
            true);
        GET_MODULE(EngineTime).advance();
        Test::VulkanSyntheticStereoTarget
            stereo_target{
                launch.headless_extent,
                GET_MODULE(RenderTarget)
                    .getSwapchainFormat()};
        renderer.renderLogicalFrame(
            stereo_target, stereo_family);
        REQUIRE(
            stereo_target.logicalBeginCount() ==
            1);
        REQUIRE(
            stereo_target.viewBeginCount() ==
            2);
        REQUIRE(
            stereo_target.viewEndCount() ==
            2);
        REQUIRE(
            stereo_target.submissionCount() == 1);

        const auto stereo_selection =
            readFrameGraphBuffer(
                "clustered_light_selection");
        constexpr std::size_t
            stereo_region_words =
                (2u * (48u + 260u)) /
                sizeof(std::uint32_t) / 2u;
        REQUIRE(stereo_region_words == 77);
        for (std::uint32_t view_index = 0;
             view_index < stereo_views.size();
             ++view_index) {
            const auto base =
                static_cast<std::size_t>(
                    view_index) *
                stereo_region_words;
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 0) ==
                0x504C5332u);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 1) == 2);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 2) == 1);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 3) == 1);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 7) == 70);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 8) ==
                view_index);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 9) == 2);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 10) ==
                main_family_token[0]);
            REQUIRE(
                wordAt(
                    stereo_selection,
                    base + 11) ==
                main_family_token[1]);
            const auto encoded =
                wordAt(
                    stereo_selection,
                    base + 12);
            REQUIRE(
                (encoded & 0x7fffffffu) ==
                64);
            REQUIRE(
                (encoded & 0x80000000u) !=
                0);
        }
        REQUIRE(
            renderer.currentFramePlanJson()
                .at("lighting_data_runtime")
                .at("selected_path") ==
            "compute_clustered");

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan clustered lighting unavailable");
        throw;
    }
#endif
}

TEST_CASE("hybrid_v1 preset registers and renders a headless frame",
          "[headless][render][hybrid]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;

    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        const auto scene_path = temp_dir / "scene.json";
        const auto asset_path = temp_dir / "assets.json";
        writeTextFile(
            scene_path,
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            asset_path,
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "features");
        writeTextFile(
            temp_dir / "features" /
                "shadow_directional.json",
            engineResourceOrThrow(
                "features/shadow_directional.json"));
        writeTextFile(
            temp_dir / "hybrid.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"},
                  {"settings",
                   {{"msaa",
                     {{"samples", 4},
                      {"fallback", "lower_supported"},
                       {"scope", "geometry"}}}}}}},
                {"features",
                 nlohmann::json::array(
                     {"project://features/shadow_directional.json",
                      "engine://features/sky_ambient.json"})},
                {"render_strategy",
                 {{"name",
                   "headless.hybrid_authored"}}},
                {"graph_transforms",
                 nlohmann::json::array(
                     {{{"name",
                        "headless.hybrid_identity"}}})},
            }, temp_dir).dump(2));

        auto project = makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] = "default_scene";
        project["basic_config"]["rendering_config_json"] = "hybrid.json";
        project["basic_config"]["default_rendering_pass"] = "main_render";
        GET_MODULE(ProjectSource).setSourceByData(project.dump());
        GET_MODULE(PathResolver).setup(temp_dir, false);

        auto &launch_config = GET_MODULE(EngineLaunchConfig);
        launch_config.headless = true;
        launch_config.headless_extent = vk::Extent2D{32, 32};
        launch_config.headless_frames = 1;
        GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        const auto lit_color =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName("lit_color");
        const auto scene_depth =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName("scene_depth");
        const auto lit_metadata =
            GET_MODULE(RenderTargetContainer).getMetadata(lit_color);
        const auto depth_metadata =
            GET_MODULE(RenderTargetContainer).getMetadata(scene_depth);
        REQUIRE(lit_metadata.samples == depth_metadata.samples);
        if (lit_metadata.samples == 1) {
            SKIP("device exposes no multisampled common hybrid attachment count");
        }
        REQUIRE(GET_MODULE(RenderTargetContainer)
                    .hasSeparateAttachment(lit_color));
        REQUIRE(GET_MODULE(RenderTargetContainer)
                    .hasSeparateAttachment(scene_depth));
        const auto main_render_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName("main_render");
        const auto execution =
            GET_MODULE(FrameGraphRuntimeContainer).find(main_render_id);
        REQUIRE(execution != nullptr);
        REQUIRE(execution->target_plan != nullptr);
        REQUIRE(execution->sample_count_plan != nullptr);
        REQUIRE(execution->render_pipeline != nullptr);
        REQUIRE(
            execution->render_pipeline
                ->surface_resource_contracts.size() == 1);
        REQUIRE(
            execution->render_pipeline
                ->surface_resource_contracts.front()
                .contract.name == "directional_shadow");
        REQUIRE(
            execution->render_pipeline
                ->surface_resource_contracts.front()
                .provider_reference ==
            "project://features/shadow_directional.json");
        REQUIRE(
            std::find(
                execution->render_pipeline
                    ->shader_defines.begin(),
                execution->render_pipeline
                    ->shader_defines.end(),
                "PELICAN_FEATURE_SHADOW") !=
            execution->render_pipeline
                ->shader_defines.end());
        REQUIRE(
            execution->render_pipeline
                ->render_strategy.has_value());
        REQUIRE(
            execution->target_plan
                ->render_strategy ==
            execution->render_pipeline
                ->render_strategy);
        REQUIRE(
            execution->render_pipeline
                ->render_strategy->name ==
            "headless.hybrid_authored");
        REQUIRE(
            execution->render_pipeline
                ->render_strategy->provider ==
            std::string{
                builtinAuthoredRenderStrategyProvider});
        REQUIRE(
            execution->render_pipeline
                ->graph_transforms.size() == 1);
        REQUIRE(
            execution->target_plan
                ->graph_transforms ==
            execution->render_pipeline
                ->graph_transforms);
        REQUIRE(
            execution->target_plan
                ->graph_transforms.front()
                .provider ==
            std::string{
                builtinLogicalGraphTransformProvider});
        const auto frame_plan_json =
            renderer.currentFramePlanJson();
        REQUIRE(
            frame_plan_json.at("render_strategy")
                .at("name") ==
            "headless.hybrid_authored");
        REQUIRE(
            frame_plan_json.at("physical_target_plan")
                .at("render_strategy")
                .at("name") ==
            "headless.hybrid_authored");
        const auto runtime_generation =
            GET_MODULE(FrameGraphRuntimeContainer)
                .activeGeneration();
        REQUIRE(runtime_generation > 0);
        REQUIRE(frame_plan_json.at("runtime_generation") ==
                runtime_generation);
        REQUIRE(frame_plan_json.at("sample_count_plan")
                    .at("request")
                    .at("samples") == 4);
        REQUIRE(frame_plan_json.at("physical_target_plan")
                    .at("backend_selection")
                    .at("selected_candidate") ==
                "pelican.vulkan.materialized_plan@1");
        const auto ejected_pins =
            vulkanTargetPlanPinPackageFromJson(
                frame_plan_json.at(
                    "physical_target_plan")
                    .at("ejectable_pin_package"));
        REQUIRE(
            ejected_pins ==
            ejectVulkanTargetPlanPinPackage(
                *execution->target_plan));
        REQUIRE(
            ejected_pins.logical_graph_fingerprint ==
            execution->target_plan
                ->logical_graph_fingerprint);
        const auto ejected_fragment =
            vulkanPhysicalFragmentPackageFromJson(
                frame_plan_json.at(
                    "physical_target_plan")
                    .at(
                        "ejectable_physical_fragment"));
        REQUIRE(
            ejected_fragment ==
            ejectVulkanPhysicalFragmentPackage(
                *execution->target_plan));
        REQUIRE(
            ejected_fragment
                .automatic_plan_fingerprint ==
            execution->target_plan
                ->automatic_plan_fingerprint);
        const auto physical_lit = std::find_if(
            execution->target_plan->resources.begin(),
            execution->target_plan->resources.end(),
            [](const auto &resource) {
                return resource.logical_resource == "lit_color";
            });
        REQUIRE(physical_lit !=
                execution->target_plan->resources.end());
        REQUIRE(physical_lit->representation ==
                VulkanResourceRepresentation::materialized_image);
        REQUIRE(physical_lit->rasterization_samples ==
                lit_metadata.samples);
        REQUIRE(std::any_of(
            execution->sample_count_plan->groups.begin(),
            execution->sample_count_plan->groups.end(),
            [&](const auto &group) {
                return group.selected_samples == lit_metadata.samples &&
                       group.selected_samples > 1;
            }));
        auto &standard = GET_MODULE(StandardMaterialResource);
        const auto surface_reference =
            std::string{"engine://surfaces/openpbr/opaque_single.surface"};
        const auto surface = parseSurfaceFormat(
            engineResourceOrThrow("surfaces/openpbr/opaque_single.surface"),
            surface_reference);
        MaterialSurfaceCatalog surfaces{{surface_reference, surface}};
        const auto material_document = parseMaterialFormatJson(
            nlohmann::json::parse(R"json({
              "schema":"pelican.material","version":1,"materials":[{
                "name":"hybrid_coat","surface":"engine://surfaces/openpbr/opaque_single.surface",
                "values":{"coat_weight":0.5},
                "routing":{"alpha_mode":"opaque","double_sided":false}
              }]
            })json"),
            surfaces);
        const auto lowered = lowerMaterial(material_document.materials.front(), surface);
        REQUIRE(lowered.route == MaterialRouteClass::forward_opaque);
        const auto shaders = GET_MODULE(ShaderLibrary).loadFromSurfaceForMaterial(
            surface, surface_reference, lowered,
            execution->render_pipeline->shader_defines);
        MaterialInfo material{
            .vert_shader = shaders.vertex,
            .frag_shader = shaders.fragment,
            .base_color_texture = standard.whiteTexture(),
            .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
            .normal_texture = standard.normalDefaultTexture(),
            .emissive_texture = standard.emissiveDefaultTexture(),
            .occlusion_texture = standard.occlusionDefaultTexture(),
        };
        applyLoweredMaterialForRoute(material, lowered);
        const auto material_id = GET_MODULE(MaterialContainer).registerMaterial(
            std::move(material));
        REQUIRE(isValidMaterialId(material_id));

        const auto refraction_source = std::string{
            "//! pelican.surface v1\n"
            "//! language: glsl\n"
            "//! screen_inputs: [opaque_color, linear_view_depth]\n"
            "//! render_state: { blend: blend, cull: none, depth: read_only, depth_compare: less_equal }\n\n"
            "void pelican_surface_v1(in PelicanSurfaceInputV1 input_data, "
            "inout PelicanSurfaceV1 surface) { "
            "vec2 bent_uv = input_data.uv + vec2(0.08, 0.0); "
            "vec3 behind = pelican_screen_opaque_color(bent_uv).rgb; "
            "float scene_depth = pelican_screen_linear_view_depth(input_data.uv).r; "
            "float fragment_depth = -(pelicanFrame.view * "
            "vec4(input_data.world_position, 1.0)).z; "
            "float fade = clamp((scene_depth - fragment_depth) * 2.0, 0.0, 1.0); "
            "surface.base_color = vec4(vec3(0.0), 0.25 + 0.5 * fade); "
            "surface.emissive = behind * vec3(0.25, 0.75, 1.25); }\n"};
        const auto refraction_surface = parseSurfaceFormat(
            refraction_source, "project://shaders/headless_refraction.surface");
        const auto refraction_lowered = lowerSurfaceDefaults(
            refraction_surface,
            "project://shaders/headless_refraction.surface");
        REQUIRE(refraction_lowered.route ==
                MaterialRouteClass::forward_transparent);
        const auto refraction_shaders =
            GET_MODULE(ShaderLibrary).loadFromSurfaceForMaterial(
                refraction_surface,
                "project://shaders/headless_refraction.surface",
                refraction_lowered,
                execution->render_pipeline->shader_defines);
        MaterialInfo refraction_material{
            .vert_shader = refraction_shaders.vertex,
            .frag_shader = refraction_shaders.fragment,
            .base_color_texture = standard.whiteTexture(),
            .metallic_roughness_texture =
                standard.metallicRoughnessDefaultTexture(),
            .normal_texture = standard.normalDefaultTexture(),
            .emissive_texture = standard.emissiveDefaultTexture(),
            .occlusion_texture = standard.occlusionDefaultTexture(),
        };
        applyLoweredMaterialForRoute(refraction_material,
                                     refraction_lowered);
        auto &materials = GET_MODULE(MaterialContainer);
        const auto refraction_id =
            materials.registerMaterial(std::move(refraction_material));
        REQUIRE(isValidMaterialId(refraction_id));

        const auto rendering_pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName("main_render");
        const auto &compiled =
            GET_MODULE(RenderingPassContainer)
                .getCompiledRenderingPass(rendering_pass_id);
        const auto transparent = std::find_if(
            compiled.passes.begin(), compiled.passes.end(),
            [](const auto &pass) {
                return pass.definition.name == "forward_transparent";
            });
        REQUIRE(transparent != compiled.passes.end());
        const auto opaque = std::find_if(
            compiled.passes.begin(), compiled.passes.end(),
            [](const auto &pass) {
                return pass.definition.name == "forward_opaque";
            });
        REQUIRE(opaque != compiled.passes.end());
        REQUIRE(
            opaque->definition.materialInfo()
                .surface_resources.size() == 1);
        REQUIRE(
            transparent->definition.materialInfo()
                .surface_resources.size() == 1);
        const auto first_revision = MaterialContainerTestAccess::screenInputBindingRevision(materials,
            refraction_id, transparent->definition);
        REQUIRE(first_revision != 0);
        const auto opaque_revision =
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                material_id, opaque->definition);
        REQUIRE(opaque_revision != 0);
        const auto opaque_color = GET_MODULE(RenderTargetContainer)
                                      .getRenderTargetIdByName("opaque_color");
        const auto opaque_depth = GET_MODULE(RenderTargetContainer)
                                      .getRenderTargetIdByName("opaque_depth");
        const auto shadow_map =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName("shadow_map");
        REQUIRE(isConcreteRenderTarget(shadow_map));
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                material_id, opaque->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getLayeredImageView(shadow_map)});
        const auto first_views = MaterialContainerTestAccess::boundScreenInputImageViews(materials,
            refraction_id, transparent->definition);
        REQUIRE(first_views ==
                std::vector<vk::ImageView>{
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_color),
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_depth),
                    GET_MODULE(RenderTargetContainer)
                        .getLayeredImageView(shadow_map)});
        RendererTestAccess::recreateRenderTargetsAndRebind(renderer, {32, 32});
        REQUIRE(MaterialContainerTestAccess::screenInputBindingRevision(materials,
                    refraction_id, transparent->definition) > first_revision);
        REQUIRE(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                material_id, opaque->definition) >
            opaque_revision);
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                material_id, opaque->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getLayeredImageView(shadow_map)});
        REQUIRE(MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                    refraction_id, transparent->definition) ==
                std::vector<vk::ImageView>{
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_color),
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_depth),
                    GET_MODULE(RenderTargetContainer)
                        .getLayeredImageView(shadow_map)});

        auto &geometry = GET_MODULE(VertBufContainer);
        ModelTemplate model;
        model.asset_id = ModelAssetId{187};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = material_id,
                .primitives = {geometry.addPrimitiveEntry(
                    makeScreenQuad(0.8f, 0.0f))},
                .source_material_index = 0},
            ModelTemplate::MaterialPrimitives{
                .material = refraction_id,
                .primitives = {geometry.addPrimitiveEntry(
                    makeScreenQuad(0.35f, 0.5f))},
                .source_material_index = 1},
        };
        const auto model_instance =
            GET_MODULE(PolygonInstanceContainer).placeModelInstance(model);
        REQUIRE(GET_MODULE(PolygonInstanceContainer)
                    .isModelInstanceAlive(model_instance));
        auto &camera = GET_MODULE(Camera);
        camera.setPos({0.0f, 0.0f, 2.0f});
        camera.setDir({0.0f, 0.0f, -1.0f});
        camera.setUp({0.0f, 1.0f, 0.0f});
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();

        const auto pixels = GET_MODULE(RenderTarget).readbackLastFrameRGBA8();
        REQUIRE(pixels.size() == 32u * 32u * 4u);
        const auto rgbDistance = [&](std::size_t left, std::size_t right) {
            return std::abs(static_cast<int>(pixels[left]) -
                            static_cast<int>(pixels[right])) +
                   std::abs(static_cast<int>(pixels[left + 1]) -
                            static_cast<int>(pixels[right + 1])) +
                   std::abs(static_cast<int>(pixels[left + 2]) -
                            static_cast<int>(pixels[right + 2]));
        };
        const auto center = (16u * 32u + 16u) * 4u;
        const auto opaque_only = (16u * 32u + 4u) * 4u;
        REQUIRE(pixels[center] + pixels[center + 1] + pixels[center + 2] > 0);
        REQUIRE(rgbDistance(center, opaque_only) > 8);
        const auto plan = renderer.currentFramePlanJson();
        REQUIRE(plan.dump().find("deferred_geometry") != std::string::npos);
        REQUIRE(plan.dump().find("forward_transparent") != std::string::npos);
        REQUIRE(plan.dump().find("__snapshot_opaque_color") !=
                std::string::npos);
        REQUIRE(plan.dump().find("__snapshot_opaque_depth") !=
                std::string::npos);
        REQUIRE(plan.dump().find("scene_present") != std::string::npos);
        REQUIRE(plan.contains("gpu_resource_arena"));
        const auto &gpu_arena =
            plan.at("gpu_resource_arena");
        REQUIRE(
            gpu_arena.at("runtime_generation") ==
            plan.at("runtime_generation"));
        REQUIRE(
            gpu_arena.at("resource_count")
                .get<std::size_t>() > 0);
        REQUIRE(
            std::any_of(
                gpu_arena.at("scopes").begin(),
                gpu_arena.at("scopes").end(),
                [](const auto &scope) {
                    return scope.at("owner_scope") ==
                           "render_pipeline/flat";
                }));

        auto &runtime =
            GET_MODULE(FrameGraphRuntimeContainer);
        auto previous_generation = runtime.snapshot();
        REQUIRE(previous_generation != nullptr);
        const auto previous_preview_pipeline =
            renderer.previewGraphProgram()
                .render_pipeline;
        REQUIRE(
            previous_preview_pipeline != nullptr);
        const auto previous_generation_number =
            previous_generation->generation;
        REQUIRE(previous_generation->find(
                    main_render_id) != nullptr);
        REQUIRE(previous_generation->gpu_arena !=
                nullptr);
        const auto previous_gpu_resource_count =
            previous_generation->gpu_arena
                ->resourceCount();
        REQUIRE(previous_gpu_resource_count > 0);
        const auto resized_opaque_revision =
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                material_id, opaque->definition);
        const auto previous_shadow_views =
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                material_id, opaque->definition);
        REQUIRE(previous_shadow_views ==
                std::vector<vk::ImageView>{
                    GET_MODULE(RenderTargetContainer)
                        .getLayeredImageView(
                            shadow_map)});

        auto reloaded_shadow_feature =
            nlohmann::json::parse(
                engineResourceOrThrow(
                    "features/shadow_directional.json"));
        // The feature name is the prefix of the scalar shader defines that
        // engine shaders spell out literally
        // (PELICAN_FEATURE_SHADOW_DIRECTIONAL_PCF_RADIUS and friends in
        // fullscreen.frag), so a reloaded copy has to keep its name. Mark the
        // recomposition with an extra define instead.
        reloaded_shadow_feature["shader_defines"].push_back(
            "PELICAN_FEATURE_SHADOW_DIRECTIONAL_RELOADED");
        writeTextFile(
            temp_dir / "features" /
                "shadow_directional.json",
            reloaded_shadow_feature.dump(2));
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "features/shadow_directional.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));

        auto reloaded_generation = runtime.snapshot();
        REQUIRE(reloaded_generation != nullptr);
        REQUIRE(reloaded_generation !=
                previous_generation);
        REQUIRE(reloaded_generation->generation ==
                previous_generation_number + 1);
        REQUIRE(previous_generation->generation ==
                previous_generation_number);
        REQUIRE(previous_generation->find(
                    main_render_id) != nullptr);
        REQUIRE(previous_generation->gpu_arena
                    ->resourceCount() ==
                previous_gpu_resource_count);
        const auto reloaded_preview_pipeline =
            renderer.previewGraphProgram()
                .render_pipeline;
        REQUIRE(
            reloaded_preview_pipeline != nullptr);
        REQUIRE(
            reloaded_preview_pipeline !=
            previous_preview_pipeline);
        REQUIRE(
            std::find(
                renderer.previewGraphProgram()
                    .render_pipeline->shader_defines.begin(),
                renderer.previewGraphProgram()
                    .render_pipeline->shader_defines.end(),
                "PELICAN_FEATURE_SHADOW_DIRECTIONAL_RELOADED") !=
            renderer.previewGraphProgram()
                .render_pipeline->shader_defines.end());

        const auto reloaded_main_id =
            reloaded_generation->name_to_id.at(
                "main_render");
        const auto *reloaded_program =
            reloaded_generation->find(
                reloaded_main_id);
        REQUIRE(reloaded_program != nullptr);
        REQUIRE(
            reloaded_program->frame_graph
                .render_pipeline != nullptr);
        REQUIRE(
            reloaded_preview_pipeline
                ->render_compiler_program ==
            reloaded_program->frame_graph
                .render_pipeline
                ->render_compiler_program);
        REQUIRE(
            reloaded_program->frame_graph
                .render_pipeline
                ->surface_resource_contracts
                .size() == 1);
        REQUIRE(
            reloaded_program->frame_graph
                .render_pipeline
                ->surface_resource_contracts.front()
                .provider_reference ==
            "project://features/shadow_directional.json");
        const auto reloaded_opaque = std::find_if(
            reloaded_program->rendering_pass.passes.begin(),
            reloaded_program->rendering_pass.passes.end(),
            [](const auto &pass) {
                return pass.definition.name ==
                       "forward_opaque";
            });
        REQUIRE(
            reloaded_opaque !=
            reloaded_program->rendering_pass.passes.end());
        const auto reloaded_shadow_map =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName("shadow_map");
        REQUIRE(isConcreteRenderTarget(
            reloaded_shadow_map));
        const auto reloaded_opaque_revision =
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                material_id,
                reloaded_opaque->definition);
        REQUIRE(reloaded_opaque_revision >
                resized_opaque_revision);
        const auto reloaded_shadow_views =
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                material_id,
                reloaded_opaque->definition);
        REQUIRE(reloaded_shadow_views ==
                std::vector<vk::ImageView>{
                    GET_MODULE(RenderTargetContainer)
                        .getLayeredImageView(
                            reloaded_shadow_map)});

        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto reloaded_pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(reloaded_pixels == pixels);

        auto invalid_shadow_feature =
            reloaded_shadow_feature;
        invalid_shadow_feature["surface_resources"]
                              [0]["resource"] =
            "missing_shadow_map";
        writeTextFile(
            temp_dir / "features" /
                "shadow_directional.json",
            invalid_shadow_feature.dump(2));
        REQUIRE_FALSE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "features/shadow_directional.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        REQUIRE(runtime.snapshot() ==
                reloaded_generation);
        REQUIRE(
            renderer.previewGraphProgram()
                .render_pipeline ==
            reloaded_preview_pipeline);
        REQUIRE(
            std::find(
                renderer.previewGraphProgram()
                    .render_pipeline->shader_defines.begin(),
                renderer.previewGraphProgram()
                    .render_pipeline->shader_defines.end(),
                "PELICAN_FEATURE_SHADOW_DIRECTIONAL_RELOADED") !=
            renderer.previewGraphProgram()
                .render_pipeline->shader_defines.end());
        REQUIRE(previous_generation->find(
                    main_render_id) != nullptr);
        REQUIRE(previous_generation->gpu_arena
                    ->resourceCount() ==
                previous_gpu_resource_count);
        REQUIRE(
            MaterialContainerTestAccess::screenInputBindingRevision(materials,
                material_id,
                reloaded_opaque->definition) ==
            reloaded_opaque_revision);
        REQUIRE(
            MaterialContainerTestAccess::boundScreenInputImageViews(materials,
                material_id,
                reloaded_opaque->definition) ==
            reloaded_shadow_views);

        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        REQUIRE(
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8() ==
            reloaded_pixels);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            ex, "Vulkan hybrid rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "sky ambient feature keeps deferred and forward metals visible with zero lights",
    "[headless][render][sky][ambient][wp240b]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;

    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        writeTextFile(
            temp_dir / "hybrid.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array(
                     {nlohmann::json{
                         {"ref",
                          "engine://features/sky_ambient.json"},
                         {"parameters",
                          {
                              {"color_r", 0.2},
                              {"color_g", 0.4},
                              {"color_b", 0.8},
                              {"ambient_intensity", 0.75},
                              {"sky_intensity", 0.1},
                          }},
                     }})},
            }, temp_dir).dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "hybrid.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "main_render";
        GET_MODULE(ProjectSource)
            .setSourceByData(project.dump());
        GET_MODULE(PathResolver)
            .setup(temp_dir, false);

        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        GET_MODULE(EngineTime).setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer =
            GET_MODULE(Renderer);
        const auto main_render_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "main_render");
        const auto execution =
            GET_MODULE(
                FrameGraphRuntimeContainer)
                .find(main_render_id);
        REQUIRE(execution != nullptr);
        REQUIRE(
            execution->render_pipeline !=
            nullptr);
        REQUIRE(
            std::find(
                execution->render_pipeline
                    ->feature_names.begin(),
                execution->render_pipeline
                    ->feature_names.end(),
                "sky_ambient") !=
            execution->render_pipeline
                ->feature_names.end());
        REQUIRE(
            std::find(
                execution->render_pipeline
                    ->shader_defines.begin(),
                execution->render_pipeline
                    ->shader_defines.end(),
                "PELICAN_FEATURE_SKY_AMBIENT") !=
            execution->render_pipeline
                ->shader_defines.end());

        constexpr std::string_view
            deferred_surface_source =
                R"surface(//! pelican.surface v1
//! language: glsl

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.base_color = vec4(0.9, 0.9, 0.9, 1.0);
    surface.roughness = 0.6;
    surface.metallic = 1.0;
    surface.emissive = vec3(0.0);
}
)surface";
        constexpr std::string_view
            deferred_surface_reference =
                "project://shaders/wp240b_deferred.surface";
        const auto deferred_surface =
            parseSurfaceFormat(
                deferred_surface_source,
                deferred_surface_reference);
        const auto deferred_lowered =
            lowerSurfaceDefaults(
                deferred_surface,
                deferred_surface_reference);
        REQUIRE(
            deferred_lowered.route ==
            MaterialRouteClass::
                deferred_geometry);
        const auto deferred_shaders =
            GET_MODULE(ShaderLibrary)
                .loadFromSurfaceForMaterial(
                    deferred_surface,
                    deferred_surface_reference,
                    deferred_lowered,
                    execution->render_pipeline
                        ->shader_defines);

        const auto openpbr_reference =
            std::string{
                "engine://surfaces/openpbr/opaque_single.surface"};
        const auto openpbr_surface =
            parseSurfaceFormat(
                engineResourceOrThrow(
                    "surfaces/openpbr/opaque_single.surface"),
                openpbr_reference);
        const MaterialSurfaceCatalog
            openpbr_catalog{
                {openpbr_reference,
                 openpbr_surface}};
        const auto openpbr_document =
            parseMaterialFormatJson(
                nlohmann::json::parse(
                    R"json({
                      "schema":"pelican.material",
                      "version":1,
                      "materials":[{
                        "name":"wp240b_forward_metal",
                        "surface":"engine://surfaces/openpbr/opaque_single.surface",
                        "values":{
                          "base_color":[0.9,0.9,0.9,1.0],
                          "base_metalness":1.0,
                          "coat_weight":0.5
                        },
                        "routing":{
                          "alpha_mode":"opaque",
                          "double_sided":false
                        }
                      }]
                    })json"),
                openpbr_catalog);
        const auto forward_lowered =
            lowerMaterial(
                openpbr_document
                    .materials.front(),
                openpbr_surface);
        REQUIRE(
            forward_lowered.route ==
            MaterialRouteClass::
                forward_opaque);
        const auto forward_shaders =
            GET_MODULE(ShaderLibrary)
                .loadFromSurfaceForMaterial(
                    openpbr_surface,
                    openpbr_reference,
                    forward_lowered,
                    execution->render_pipeline
                        ->shader_defines);

        const auto &standard =
            GET_MODULE(
                StandardMaterialResource);
        auto &materials =
            GET_MODULE(MaterialContainer);
        // glTF leaves metallicRoughness R undefined. R=0 reproduces the
        // DamagedHelmet failure; absent occlusion must still resolve to white.
        const std::array<std::uint8_t, 4>
            zero_red_metallic_roughness{
                0, 255, 255, 255};
        const auto zero_red_metallic_roughness_texture =
            materials.registerTexture(
                vk::Extent3D{1, 1, 1},
                zero_red_metallic_roughness.data());
        const auto make_material =
            [&](SurfaceShaderBundleIds shaders,
                const LoweredMaterial &lowered) {
                MaterialInfo material{
                    .vert_shader =
                        shaders.vertex,
                    .frag_shader =
                        shaders.fragment,
                    .base_color_texture =
                        standard.whiteTexture(),
                    .metallic_roughness_texture =
                        standard
                            .metallicRoughnessDefaultTexture(),
                    .normal_texture =
                        standard.normalDefaultTexture(),
                    .emissive_texture =
                        standard.emissiveDefaultTexture(),
                    .occlusion_texture =
                        standard.occlusionDefaultTexture(),
                };
                applyLoweredMaterialForRoute(
                    material, lowered);
                return material;
            };
        auto deferred_material_info =
            make_material(
                deferred_shaders,
                deferred_lowered);
        deferred_material_info
            .metallic_roughness_texture =
            zero_red_metallic_roughness_texture;
        const auto deferred_material =
            materials.registerMaterial(
                std::move(
                    deferred_material_info));
        const auto forward_material =
            materials.registerMaterial(
                make_material(
                    forward_shaders,
                    forward_lowered));
        REQUIRE(isValidMaterialId(
            deferred_material));
        REQUIRE(isValidMaterialId(
            forward_material));

        auto &geometry =
            GET_MODULE(VertBufContainer);
        ModelTemplate deferred_model;
        deferred_model.asset_id =
            ModelAssetId{2401};
        deferred_model.material_primitives = {
            ModelTemplate::
                MaterialPrimitives{
                    .material =
                        deferred_material,
                    .primitives =
                        {geometry
                             .addPrimitiveEntry(
                                 makeScreenQuad(
                                     0.3f,
                                     0.0f))},
                    .source_material_index =
                        0},
        };
        ModelTemplate forward_model;
        forward_model.asset_id =
            ModelAssetId{2402};
        forward_model.material_primitives = {
            ModelTemplate::
                MaterialPrimitives{
                    .material =
                        forward_material,
                    .primitives =
                        {geometry
                             .addPrimitiveEntry(
                                 makeScreenQuad(
                                     0.3f,
                                     0.0f))},
                    .source_material_index =
                        0},
        };
        auto &instances =
            GET_MODULE(
                PolygonInstanceContainer);
        const auto deferred_instance =
            instances.placeModelInstance(
                deferred_model);
        const auto forward_instance =
            instances.placeModelInstance(
                forward_model);
        instances.setTrs(
            deferred_instance,
            {-0.45f, 0.0f, 0.0f},
            glm::quat{
                1.0f, 0.0f, 0.0f, 0.0f},
            glm::vec3{1.0f});
        instances.setTrs(
            forward_instance,
            {0.45f, 0.0f, 0.0f},
            glm::quat{
                1.0f, 0.0f, 0.0f, 0.0f},
            glm::vec3{1.0f});

        auto &camera =
            GET_MODULE(Camera);
        camera.setPos(
            {0.0f, 0.0f, 2.0f});
        camera.setDir(
            {0.0f, 0.0f, -1.0f});
        camera.setUp(
            {0.0f, 1.0f, 0.0f});

        renderer.render();
        auto &vkcore =
            GET_MODULE(VulkanManageCore);
        vkcore.waitIdle();

        const auto light_bytes =
            vkcore.readBuf(
                GET_MODULE(LightContainer)
                    .lightBuffer(),
                sizeof(LightUBO));
        REQUIRE(light_bytes.size() ==
                sizeof(LightUBO));
        LightUBO light_data{};
        std::memcpy(
            &light_data,
            light_bytes.data(),
            sizeof(light_data));
        REQUIRE(
            light_data
                .directionalLightCount == 0);
        REQUIRE(
            light_data.pointLightCount == 0);
        REQUIRE(
            light_data.spotLightCount == 0);
        REQUIRE(
            std::abs(
                light_data
                    .environmentAmbientRadiance
                    .r -
                0.15f) < 0.00001f);
        REQUIRE(
            std::abs(
                light_data
                    .environmentAmbientRadiance
                    .g -
                0.3f) < 0.00001f);
        REQUIRE(
            std::abs(
                light_data
                    .environmentAmbientRadiance
                    .b -
                0.6f) < 0.00001f);
        REQUIRE(
            std::abs(
                light_data
                    .environmentSkyRadiance
                    .r -
                0.02f) < 0.00001f);
        REQUIRE(
            std::abs(
                light_data
                    .environmentSkyRadiance
                    .g -
                0.04f) < 0.00001f);
        REQUIRE(
            std::abs(
                light_data
                    .environmentSkyRadiance
                    .b -
                0.08f) < 0.00001f);

        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            pixels.size() ==
            32u * 32u * 4u);
        const auto corner_sum =
            static_cast<unsigned>(
                pixels[0]) +
            static_cast<unsigned>(
                pixels[1]) +
            static_cast<unsigned>(
                pixels[2]);
        REQUIRE(corner_sum > 0);
        std::array<
            unsigned, 2>
            half_maximum{
                0u, 0u};
        std::array<
            std::size_t, 2>
            visible_pixels{
                0u, 0u};
        std::vector<std::uint8_t>
            deferred_non_background;
        for (std::size_t y = 0;
             y < 32; ++y) {
            for (std::size_t x = 0;
                 x < 32; ++x) {
                const auto offset =
                    (y * 32 + x) * 4;
                const auto sum =
                    static_cast<unsigned>(
                        pixels[offset]) +
                    static_cast<unsigned>(
                        pixels[offset + 1]) +
                    static_cast<unsigned>(
                        pixels[offset + 2]);
                const auto half =
                    x < 16 ? 0u : 1u;
                half_maximum[half] =
                    std::max(
                        half_maximum[half],
                        sum);
                const auto distance =
                    std::abs(
                        static_cast<int>(
                            pixels[offset]) -
                        static_cast<int>(
                            pixels[0])) +
                    std::abs(
                        static_cast<int>(
                            pixels[offset + 1]) -
                        static_cast<int>(
                            pixels[1])) +
                    std::abs(
                        static_cast<int>(
                            pixels[offset + 2]) -
                        static_cast<int>(
                            pixels[2]));
                if (sum > corner_sum + 20 &&
                    distance > 20) {
                    ++visible_pixels[half];
                }
                if (half == 0 && distance > 20) {
                    deferred_non_background.push_back(
                        std::max({
                            pixels[offset],
                            pixels[offset + 1],
                            pixels[offset + 2],
                        }));
                }
            }
        }
        REQUIRE_FALSE(
            deferred_non_background.empty());
        std::ranges::sort(
            deferred_non_background);
        const auto deferred_median =
            deferred_non_background[
                deferred_non_background.size() /
                2];
        // The coupled MR.R path produces an 8-bit median at or below 2 here.
        INFO(
            "sky sum=" << corner_sum <<
            ", deferred max=" <<
            half_maximum[0] <<
            ", forward max=" <<
            half_maximum[1] <<
            ", deferred non-background median=" <<
            static_cast<unsigned>(
                deferred_median));
        REQUIRE(deferred_median > 16);
        REQUIRE(
            half_maximum[0] >
            corner_sum + 20);
        REQUIRE(
            half_maximum[1] >
            corner_sum + 20);
        REQUIRE(
            visible_pixels[0] > 8);
        REQUIRE(
            visible_pixels[1] > 8);

        const auto frame_plan =
            renderer.currentFramePlanJson();
        const auto sky_node =
            std::find_if(
                frame_plan.at("nodes").begin(),
                frame_plan.at("nodes").end(),
                [](const auto &node) {
                    return node.at("name") ==
                           "sky_background";
                });
        REQUIRE(
            sky_node !=
            frame_plan.at("nodes").end());
        REQUIRE(
            sky_node->at("reads") ==
            nlohmann::json::array(
                {"scene_depth", "lit_color"}));
        REQUIRE(
            sky_node->at("writes") ==
            nlohmann::json::array(
                {"lit_color"}));

        vkcore.waitIdle();
        std::filesystem::remove_all(
            temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan sky ambient rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "WP218 and WP219 project material writes typed G-buffer targets with independent attachment state",
    "[wp218][wp219][headless][render][gbuffer][mrt][attachment-state]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;

    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        writeTextFile(
            temp_dir / "shaders" /
                "object_id_present.frag",
            R"glsl(#version 450
layout(set = 1, binding = 0) uniform sampler2D albedo;
layout(set = 1, binding = 1) uniform usampler2D objectIds;
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() {
    uint objectId = texture(objectIds, inUV).r;
    outColor = objectId == 73u
        ? vec4(texture(albedo, inUV).rgb, 1.0)
        : objectId == 0xffffffffu
              ? vec4(1.0, 0.0, 0.0, 1.0)
              : vec4(0.0, 0.0, 0.0, 1.0);
}
)glsl");

        auto pipeline_document =
            nlohmann::json::parse(
                engineResourceOrThrow(
                    "render_pipelines/hybrid_v1.json"));
        auto config =
            pipeline_document.at("config");
        config["multisampling"] = {
            {"samples", 4},
            {"fallback", "lower_supported"},
            {"scope", "geometry"},
        };
        config["render_targets"].push_back({
            {"name", "gbuffer_object_id"},
            {"extent_scale", 1.0},
            {"format", "R32_UINT"},
            {"format_class", "data"},
            {"usage",
             nlohmann::json::array(
                 {"COLOR_ATTACHMENT", "SAMPLED"})},
        });

        auto &passes =
            config["rendering_passes"][0]["passes"];
        const auto deferred =
            std::find_if(
                passes.begin(), passes.end(),
                [](const auto &pass) {
                    return pass.at("name") ==
                           "deferred_geometry";
                });
        REQUIRE(deferred != passes.end());
        (*deferred)["output"]["color"].push_back(
            "gbuffer_object_id");
        (*deferred)["material_outputs"] = {
            {"schema", "pelican.material_outputs"},
            {"version", 1},
            {"name", "headless.extended_gbuffer"},
            {"outputs",
             nlohmann::json::array({
                 {{"name", "albedo"},
                  {"type", "vec4"},
                  {"source", "surface.base_color"}},
                 {{"name", "normal"},
                  {"type", "vec4"},
                  {"source", "surface.normal_encoded"}},
                 {{"name", "material"},
                  {"type", "vec4"},
                  {"source", "surface.material"}},
                 {{"name", "world_position"},
                  {"type", "vec4"},
                  {"source", "input.world_position"}},
                 {{"name", "emissive"},
                  {"type", "vec4"},
                  {"source", "surface.emissive"}},
                 {{"name", "object_id"},
                  {"type", "uint"},
                  {"source", "custom"}},
             })},
        };
        (*deferred)["material_output_states"] = {
            {"albedo",
             {
                 {"blend",
                  {
                      {"color",
                       {{"src", "one"},
                        {"dst", "one"},
                        {"op", "add"}}},
                      {"alpha",
                       {{"src", "one"},
                        {"dst", "one"},
                        {"op", "add"}}},
                  }},
                 {"write_mask", "r"},
             }},
            {"object_id",
             {
                 {"blend", "opaque"},
                 {"write_mask", "r"},
             }},
        };
        (*deferred)["clear_colors"] = {
            {"gbuffer_albedo",
             nlohmann::json::array(
                 {0.25, 0.75, 0.5, 0.0})},
            {"gbuffer_object_id",
             nlohmann::json::array(
                 {4294967295.0, 0, 0, 0})},
        };
        const auto present =
            std::find_if(
                passes.begin(), passes.end(),
                [](const auto &pass) {
                    return pass.at("name") ==
                           "scene_present";
                });
        REQUIRE(present != passes.end());
        (*present)["input"] =
            nlohmann::json::array(
                {"gbuffer_albedo",
                 "gbuffer_object_id"});
        (*present)["input_sampling"] =
            nlohmann::json::array({
                {{"filter", "nearest"},
                 {"address", "clamp_to_edge"}},
                {{"filter", "nearest"},
                 {"address", "clamp_to_edge"}},
            });
        (*present)["shader"]["fragment"] =
            "project://shaders/object_id_present";
        writeTextFile(
            temp_dir / "extended_gbuffer.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(config, temp_dir).dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "extended_gbuffer.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "main_render";
        GET_MODULE(ProjectSource).setSourceByData(
            project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false);

        const auto physical_device =
            GET_MODULE(VulkanManageCore)
                .getPhysDevice();
        const auto limits =
            physical_device.getProperties().limits;
        if (!GET_MODULE(VulkanManageCore)
                 .getRuntimeCapabilities()
                 .independent_blend) {
            std::filesystem::remove_all(temp_dir);
            temp_dir.clear();
            SKIP(
                "device does not expose independentBlend; "
                "per-attachment state is rejected before pipeline "
                "creation");
        }
        if (limits.maxColorAttachments < 6) {
            std::filesystem::remove_all(temp_dir);
            temp_dir.clear();
            SKIP(
                "device exposes fewer than six color attachments; "
                "the runtime correctly treats maxColorAttachments "
                "as the physical limit");
        }
        const auto object_id_features =
            physical_device
                .getFormatProperties(
                    vk::Format::eR32Uint)
                .optimalTilingFeatures;
        const auto required_features =
            vk::FormatFeatureFlagBits::eColorAttachment |
            vk::FormatFeatureFlagBits::eSampledImage;
        if ((object_id_features & required_features) !=
            required_features) {
            std::filesystem::remove_all(temp_dir);
            temp_dir.clear();
            SKIP(
                "device cannot use R32_UINT as both a color "
                "attachment and sampled image");
        }

        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 1;
        GET_MODULE(EngineTime).setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        const auto main_render_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "main_render");
        const auto execution =
            GET_MODULE(FrameGraphRuntimeContainer)
                .find(main_render_id);
        REQUIRE(execution != nullptr);
        REQUIRE(
            execution->render_pipeline != nullptr);

        const auto schema =
            GET_MODULE(RenderingPassContainer)
                .materialOutputSchema(
                    MaterialRouteClass::
                        deferred_geometry);
        REQUIRE(schema);
        REQUIRE(schema->name ==
                "headless.extended_gbuffer");
        REQUIRE(schema->outputs.size() == 6);
        REQUIRE(
            schema->outputs.back().type ==
            MaterialOutputType::
                unsigned_integer);

        constexpr std::string_view surface_source =
            R"surface(//! pelican.surface v1
//! language: glsl

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.base_color = vec4(0.25, 0.1, 0.1, 1.0);
}

void pelican_material_outputs_v1(
    in PelicanSurfaceInputV1 input_data,
    in PelicanSurfaceV1 surface,
    inout PelicanMaterialOutputsV1 outputs) {
    outputs.object_id = 73u;
}
)surface";
        constexpr std::string_view surface_reference =
            "project://shaders/extended_gbuffer.surface";
        writeTextFile(
            temp_dir / "shaders" /
                "extended_gbuffer.surface",
            std::string{surface_source});
        const auto surface = parseSurfaceFormat(
            surface_source, surface_reference);
        const auto lowered = lowerSurfaceDefaults(
            surface, surface_reference);
        REQUIRE(
            lowered.route ==
            MaterialRouteClass::
                deferred_geometry);
        const auto shaders =
            GET_MODULE(ShaderLibrary)
                .loadFromSurfaceForMaterial(
                    surface, surface_reference,
                    lowered,
                    execution->render_pipeline
                        ->shader_defines);
        const auto &fragment =
            GET_MODULE(ShaderLibrary)
                .get(shaders.fragment);
        REQUIRE(
            fragment.material_output_schema ==
            schema);
        REQUIRE(
            fragment.reflection
                .fragment_outputs.size() == 6);
        REQUIRE(
            fragment.reflection
                .fragment_outputs.back()
                .format ==
            vk::Format::eR32Uint);

        auto &standard =
            GET_MODULE(StandardMaterialResource);
        MaterialInfo material{
            .vert_shader = shaders.vertex,
            .frag_shader = shaders.fragment,
            .base_color_texture =
                standard.whiteTexture(),
            .metallic_roughness_texture =
                standard
                    .metallicRoughnessDefaultTexture(),
            .normal_texture =
                standard.normalDefaultTexture(),
            .emissive_texture =
                standard.emissiveDefaultTexture(),
            .occlusion_texture =
                standard.occlusionDefaultTexture(),
        };
        applyLoweredMaterialForRoute(
            material, lowered);
        const auto material_id =
            GET_MODULE(MaterialContainer)
                .registerMaterial(
                    std::move(material));
        REQUIRE(
            isValidMaterialId(material_id));

        const auto object_id_target =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "gbuffer_object_id");
        const auto object_id_metadata =
            GET_MODULE(RenderTargetContainer)
                .getMetadata(
                    object_id_target);
        REQUIRE(
            object_id_metadata.format ==
            vk::Format::eR32Uint);
        if (object_id_metadata.samples > 1) {
            REQUIRE(
                GET_MODULE(
                    RenderTargetContainer)
                    .hasSeparateAttachment(
                        object_id_target));
        }

        auto &geometry =
            GET_MODULE(VertBufContainer);
        ModelTemplate model;
        model.asset_id = ModelAssetId{218};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = material_id,
                .primitives = {
                    geometry.addPrimitiveEntry(
                        makeScreenQuad(
                            0.4f, 0.0f))},
                .source_material_index = 0},
        };
        const auto model_instance =
            GET_MODULE(
                PolygonInstanceContainer)
                .placeModelInstance(model);
        REQUIRE(
            GET_MODULE(
                PolygonInstanceContainer)
                .isModelInstanceAlive(
                    model_instance));
        auto &camera = GET_MODULE(Camera);
        camera.setPos(
            {0.0f, 0.0f, 2.0f});
        camera.setDir(
            {0.0f, 0.0f, -1.0f});
        camera.setUp(
            {0.0f, 1.0f, 0.0f});

        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            pixels.size() ==
            32u * 32u * 4u);
        const auto center =
            (16u * 32u + 16u) * 4u;
        const auto corner = 0u;
        REQUIRE(pixels[center] > 176);
        REQUIRE(pixels[center] < 200);
        REQUIRE(pixels[center + 1] > 216);
        REQUIRE(pixels[center + 1] < 236);
        REQUIRE(pixels[center + 2] > 176);
        REQUIRE(pixels[center + 2] < 200);
        REQUIRE(pixels[corner] > 224);
        REQUIRE(pixels[corner + 1] < 16);
        REQUIRE(pixels[corner + 2] < 16);

        const auto frame_plan =
            renderer.currentFramePlanJson();
        REQUIRE(
            frame_plan.dump().find(
                "gbuffer_object_id") !=
            std::string::npos);
        REQUIRE(
            frame_plan.dump().find(
                "headless.extended_gbuffer") !=
            std::string::npos);
        REQUIRE(
            frame_plan.dump().find(
                "pelican.material_output_states@1") !=
            std::string::npos);

        auto &runtime =
            GET_MODULE(
                FrameGraphRuntimeContainer);
        const auto published_generation =
            runtime.snapshot();
        REQUIRE(
            published_generation != nullptr);
        auto state_reload = config;
        auto &state_reload_passes =
            state_reload
                ["rendering_passes"][0]
                ["passes"];
        const auto state_reload_deferred =
            std::find_if(
                state_reload_passes.begin(),
                state_reload_passes.end(),
                [](const auto &pass) {
                    return pass.at("name") ==
                           "deferred_geometry";
                });
        REQUIRE(
            state_reload_deferred !=
            state_reload_passes.end());
        (*state_reload_deferred)
            ["material_output_states"]["albedo"]
            ["write_mask"] = "rg";
        writeTextFile(
            temp_dir / "extended_gbuffer.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(state_reload, temp_dir).dump(2));
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "extended_gbuffer.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        const auto state_generation =
            runtime.snapshot();
        REQUIRE(state_generation != nullptr);
        REQUIRE(
            state_generation !=
            published_generation);
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();
        const auto state_pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(state_pixels != pixels);
        REQUIRE(
            state_pixels[center + 1] >
            pixels[center + 1]);

        auto coordinated_reload = state_reload;
        auto &reload_passes =
            coordinated_reload
                ["rendering_passes"][0]
                ["passes"];
        const auto reload_deferred =
            std::find_if(
                reload_passes.begin(),
                reload_passes.end(),
                [](const auto &pass) {
                    return pass.at("name") ==
                           "deferred_geometry";
                });
        REQUIRE(
            reload_deferred !=
            reload_passes.end());
        (*reload_deferred)
            ["material_outputs"]["name"] =
            "headless.coordinated_reload";
        constexpr std::string_view
            coordinated_surface_source =
                R"surface(//! pelican.surface v1
//! language: glsl

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.base_color = vec4(0.05, 0.2, 0.1, 1.0);
}

void pelican_material_outputs_v1(
    in PelicanSurfaceInputV1 input_data,
    in PelicanSurfaceV1 surface,
    inout PelicanMaterialOutputsV1 outputs) {
    outputs.object_id = 73u;
}
)surface";
        writeTextFile(
            temp_dir / "extended_gbuffer.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(coordinated_reload, temp_dir).dump(2));
        writeTextFile(
            temp_dir / "shaders" /
                "extended_gbuffer.surface",
            std::string{
                coordinated_surface_source});
        const auto shader_version_before =
            GET_MODULE(ShaderLibrary)
                .get(shaders.fragment)
                .version;
        const std::array coordinated_requests{
            watch::ReloadRequest{
                watch::makeAssetKey(
                    "extended_gbuffer.json"),
                watch::ReloadKind::modified,
                {}, 2},
            watch::ReloadRequest{
                watch::makeAssetKey(
                    "shaders/extended_gbuffer.surface"),
                watch::ReloadKind::modified,
                {}, 2},
        };
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestsForTesting(
                    coordinated_requests) ==
            std::vector<bool>{true, true});
        const auto coordinated_generation =
            runtime.snapshot();
        REQUIRE(
            coordinated_generation != nullptr);
        REQUIRE(
            coordinated_generation !=
            state_generation);
        REQUIRE(
            GET_MODULE(ShaderLibrary)
                .get(shaders.fragment)
                .version ==
            shader_version_before + 1);
        const auto coordinated_schema =
            MaterialContainerTestAccess::materialOutputSchema(GET_MODULE(MaterialContainer),
                    material_id);
        REQUIRE(coordinated_schema);
        REQUIRE(
            coordinated_schema->name ==
            "headless.coordinated_reload");

        renderer.render();
        GET_MODULE(VulkanManageCore)
            .waitIdle();
        const auto coordinated_pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            coordinated_pixels !=
            state_pixels);
        REQUIRE(
            coordinated_pixels[center] <
            state_pixels[center]);
        REQUIRE(
            coordinated_pixels[center + 1] >
            state_pixels[center + 1]);

        auto failed_reload =
            coordinated_reload;
        auto &failed_passes =
            failed_reload
                ["rendering_passes"][0]
                ["passes"];
        const auto failed_deferred =
            std::find_if(
                failed_passes.begin(),
                failed_passes.end(),
                [](const auto &pass) {
                    return pass.at("name") ==
                           "deferred_geometry";
                });
        REQUIRE(
            failed_deferred !=
            failed_passes.end());
        (*failed_deferred)
            ["material_outputs"]["name"] =
            "headless.failed_reload";
        writeTextFile(
            temp_dir / "extended_gbuffer.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(failed_reload, temp_dir).dump(2));
        writeTextFile(
            temp_dir / "shaders" /
                "extended_gbuffer.surface",
            R"surface(//! pelican.surface v1
//! language: glsl
void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    this_is_not_valid_glsl
}
)surface");
        const auto failed_generation_before =
            runtime.snapshot();
        const auto failed_shader_version_before =
            GET_MODULE(ShaderLibrary)
                .get(shaders.fragment)
                .version;
        const std::array failed_requests{
            watch::ReloadRequest{
                watch::makeAssetKey(
                    "extended_gbuffer.json"),
                watch::ReloadKind::modified,
                {}, 3},
            watch::ReloadRequest{
                watch::makeAssetKey(
                    "shaders/extended_gbuffer.surface"),
                watch::ReloadKind::modified,
                {}, 3},
        };
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestsForTesting(
                    failed_requests) ==
            std::vector<bool>{false, false});
        REQUIRE(
            runtime.snapshot() ==
            failed_generation_before);
        REQUIRE(
            GET_MODULE(ShaderLibrary)
                .get(shaders.fragment)
                .version ==
            failed_shader_version_before);
        const auto rollback_schema =
            MaterialContainerTestAccess::materialOutputSchema(GET_MODULE(MaterialContainer),
                    material_id);
        REQUIRE(rollback_schema);
        REQUIRE(
            rollback_schema->name ==
            "headless.coordinated_reload");

        renderer.render();
        GET_MODULE(VulkanManageCore)
            .waitIdle();
        const auto rollback_pixels =
            GET_MODULE(RenderTarget)
                .readbackLastFrameRGBA8();
        REQUIRE(
            rollback_pixels ==
            coordinated_pixels);

        std::filesystem::remove_all(
            temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan extended G-buffer rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "WP196 pipeline watcher coalesces dependencies and preserves the active generation on failure",
    "[wp196][headless][render-pipeline][hot-reload][coalesce][rollback]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        std::filesystem::create_directories(
            temp_dir / "shaders");
        std::filesystem::create_directories(
            temp_dir / "features");
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(temp_dir / "assets.json",
                      R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        writeTextFile(
            temp_dir / "shaders" /
                "gpu_arena_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "pipeline_reload.frag",
            pipelineReloadFragmentShader());

        auto initial_config =
            pipelineReloadRenderingConfig();
        writeTextFile(
            temp_dir / "features" /
                "reload_marker.json",
            R"json({"schema":"pelican.render_feature","version":1,"name":"reload_marker"})json");
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(initial_config, temp_dir).dump());

        auto project =
            makeProjectConfig("scene.json",
                              "assets.json");
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "pipeline_reload_main";
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["schema"] =
            "pelican.project";
        project["version"] = 1;
        project["name"] =
            "wp196-pipeline-reload";
        GET_MODULE(ProjectSource)
            .setProjectData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false, project.dump());
        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{16, 16};

        auto &renderer = GET_MODULE(Renderer);
        (void)renderer;
        auto &runtime =
            GET_MODULE(FrameGraphRuntimeContainer);
        auto before = runtime.snapshot();
        REQUIRE(before != nullptr);
        const auto before_generation =
            before->generation;

        auto replacement = initial_config;
        replacement["shader_defines"][0] =
            "WP196_RELOADED";
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(replacement, temp_dir).dump());
        // A real watcher batch can contain both the root and one dependency.
        // The participant must compile and publish exactly once.
        const std::array requests{
            watch::ReloadRequest{
                watch::makeAssetKey(
                    "pipeline.json"),
                watch::ReloadKind::modified,
                {}, 1},
            watch::ReloadRequest{
                watch::makeAssetKey(
                    "features/reload_marker.json"),
                watch::ReloadKind::modified,
                {}, 1},
        };
        const auto applied =
            GET_MODULE(watch::ReloadService)
                .applyRequestsForTesting(requests);
        REQUIRE(applied ==
                std::vector<bool>{true, true});
        auto after = runtime.snapshot();
        REQUIRE(after != nullptr);
        REQUIRE(after->generation ==
                before_generation + 1);
        const auto *program =
            after->find(
                after->name_to_id.at(
                    "pipeline_reload_main"));
        REQUIRE(program != nullptr);
        REQUIRE(
            program->frame_graph.render_pipeline
                ->shader_defines ==
            std::vector<std::string>{
                "WP196_RELOADED"});

        writeTextFile(
            temp_dir / "features" /
                "reload_marker.json",
            "{ not valid json");
        REQUIRE_FALSE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "features/reload_marker.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        REQUIRE(runtime.snapshot() == after);

        writeTextFile(
            temp_dir / "features" /
                "reload_marker.json",
            R"json({"schema":"pelican.render_feature","version":1,"name":"reload_marker"})json");
        auto unsupported_feature =
            replacement;
        unsupported_feature.erase(
            "shader_defines");
        unsupported_feature["features"]
            .push_back(
                "engine://features/ui.json");
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(unsupported_feature, temp_dir).dump());
        FastModuleContainer::freezeCreation();
        REQUIRE_FALSE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "pipeline.json"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        REQUIRE(runtime.snapshot() == after);

        GET_MODULE(VulkanManageCore).waitIdle();
        before.reset();
        after.reset();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan pipeline reload unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "WP194 rollback and WP195 scope replacement preserve generation-owned GPU resources",
    "[wp194][wp195][headless][render-pipeline][gpu-arena][rollback][replacement]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        std::filesystem::create_directories(
            temp_dir / "shaders");
        const auto scene_path = temp_dir / "scene.json";
        const auto asset_path = temp_dir / "assets.json";
        writeTextFile(
            scene_path,
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            asset_path,
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        writeTextFile(
            temp_dir / "shaders" /
                "gpu_arena_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "gpu_arena_compute.comp",
            gpuArenaComputeShader());
        writeTextFile(
            temp_dir / "shaders" /
                "gpu_arena_buffer.frag",
            gpuArenaBufferFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "gpu_arena_copy.frag",
            gpuArenaCopyFragmentShader());

        auto project =
            makeProjectConfig("scene.json", "assets.json");
        project["basic_config"]["default_scene_id"] =
            "default_scene";
        GET_MODULE(ProjectSource).setSourceByData(
            project.dump());
        GET_MODULE(PathResolver).setup(temp_dir, false);
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{16, 16};
        (void)GET_MODULE(RenderTarget);

        const auto registries =
            gpuArenaRegistryDependencies();
        auto &debug_draw = GET_MODULE(DebugDraw);
        auto &debug_text = GET_MODULE(DebugText);
        auto &gizmo = GET_MODULE(Gizmo);
        const auto baseline =
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw, &debug_text,
                &gizmo);
        auto &runtime =
            GET_MODULE(FrameGraphRuntimeContainer);
        REQUIRE(runtime.snapshot() == nullptr);

        constexpr std::array fault_points{
            RenderPipelineGpuRegistrationFaultPoint::
                after_render_targets,
            RenderPipelineGpuRegistrationFaultPoint::
                after_frame_graph_buffers,
            RenderPipelineGpuRegistrationFaultPoint::
                after_compute_tasks,
            RenderPipelineGpuRegistrationFaultPoint::
                after_rendering_passes,
            RenderPipelineGpuRegistrationFaultPoint::
                after_runtime_prepare,
        };
        // MG-2b: the arena's buffers and tasks reach the composition through
        // the project's task feature.
        const auto config =
            Pelican::TestSupport::renderGraphRootWithTaskFeature(gpuArenaRenderingConfig(), temp_dir).dump();
        for (const auto fault_point : fault_points) {
            RenderingPassConfigRegistrationDependencies::
                Options options;
            options.fault_point = fault_point;
            REQUIRE_THROWS_WITH(
                registerRenderingPassConfigFromJsonData(
                    config, {16, 16},
                    gpuArenaRegistrationDependencies(
                        std::move(options))),
                "Injected render pipeline GPU registration failure");
            REQUIRE(
                inspectRenderPipelineGpuRegistryCounts(
                    registries, &debug_draw,
                    &debug_text, &gizmo) == baseline);
            REQUIRE(runtime.snapshot() == nullptr);
            REQUIRE_FALSE(
                isConcreteRenderTarget(
                    GET_MODULE(RenderTargetContainer)
                        .getRenderTargetIdByName(
                            "gpu_arena_scratch")));
            REQUIRE_FALSE(
                GET_MODULE(FrameGraphResourceContainer)
                    .hasBuffer("gpu_arena_buffer"));
            REQUIRE(
                GET_MODULE(ComputeTaskContainer)
                    .getComputeTaskIdByName(
                        "gpu_arena_compute")
                    .value < 0);
        }

        DelegatingVulkanCompilerProgram
            compiler_program;
        RenderingPassConfigRegistrationDependencies::
            Options compiler_options;
        compiler_options.render_compiler_program =
            &compiler_program;
        const auto registered =
            registerRenderingPassConfigFromJsonData(
                config, {16, 16},
                gpuArenaRegistrationDependencies(
                    std::move(
                        compiler_options)));
        REQUIRE(compiler_program.compile_calls == 1);
        REQUIRE(
            compiler_program.compiled_variants ==
            std::vector{
                RenderPipelineGraphVariant::flat});
        REQUIRE(
            compiler_program.compiled_artifacts ==
            std::vector{
                RenderCompilerProgramArtifact::
                    runtime_package});
        REQUIRE(registered.runtime_generation == 1);
        REQUIRE(registered.target_plans.size() == 1);
        REQUIRE(
            registered.target_plans.front()
                ->render_strategy.has_value());
        REQUIRE(
            registered.target_plans.front()
                ->render_strategy->name ==
            "headless.gpu_arena");
        REQUIRE(
            registered.target_plans.front()
                ->render_strategy->provider ==
            std::string{
                builtinAuthoredRenderStrategyProvider});
        REQUIRE(
            registered.target_plans.front()
                ->graph_transforms.size() == 1);
        REQUIRE(
            registered.target_plans.front()
                ->graph_transforms.front()
                .provider ==
            std::string{
                builtinLogicalGraphTransformProvider});
        REQUIRE(
            registered.target_plans.front()
                ->subgraph_replacements.size() == 1);
        REQUIRE(
            registered.target_plans.front()
                ->subgraph_replacements.front()
                .provider ==
            std::string{
                builtinTaggedSubgraphReplacementProvider});
        auto generation = runtime.snapshot();
        REQUIRE(generation != nullptr);
        REQUIRE(generation->generation == 1);
        const auto *compiled_program =
            generation->find(
                generation->name_to_id.at(
                    "gpu_arena_main"));
        REQUIRE(compiled_program != nullptr);
        REQUIRE(
            compiled_program->frame_graph
                .render_pipeline
                ->render_compiler_program
                .has_value());
        CHECK(
            compiled_program->frame_graph
                .render_pipeline
                ->render_compiler_program->name ==
            "test.delegating_vulkan");
        REQUIRE(generation->gpu_arena != nullptr);
        REQUIRE(
            generation->gpu_arena->runtime_generation ==
            generation->generation);
        const auto *scope =
            generation->gpu_arena->findScope(
                "render_pipeline/flat");
        REQUIRE(scope != nullptr);
        const auto has_kind =
            [scope](RenderPipelineGpuResourceKind kind) {
                return std::any_of(
                    scope->resources.begin(),
                    scope->resources.end(),
                    [kind](const auto &resource) {
                        return resource.kind == kind;
                    });
            };
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::render_target));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::
                frame_graph_buffer));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::compute_task));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::fullscreen_pass));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::shader_bundle));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::pipeline));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::debug_draw_pass));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::gizmo_pass));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::debug_text_pass));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::shadow_depth_pass));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::velocity_pass));
        const auto committed =
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw, &debug_text,
                &gizmo);
        REQUIRE(committed.render_targets >
                baseline.render_targets);
        REQUIRE(committed.frame_graph_buffers >
                baseline.frame_graph_buffers);
        REQUIRE(committed.compute_tasks >
                baseline.compute_tasks);
        REQUIRE(committed.fullscreen_passes >
                baseline.fullscreen_passes);
        REQUIRE(committed.shader_bundles >
                baseline.shader_bundles);
        REQUIRE(committed.pipelines >
                baseline.pipelines);
        REQUIRE(committed.debug_draw_passes >
                baseline.debug_draw_passes);
        REQUIRE(committed.gizmo_passes >
                baseline.gizmo_passes);
        REQUIRE(committed.debug_text_passes >
                baseline.debug_text_passes);
        REQUIRE(committed.shadow_depth_passes >
                baseline.shadow_depth_passes);
        REQUIRE(committed.velocity_passes >
                baseline.velocity_passes);

        const auto old_program_id =
            generation->name_to_id.at(
                "gpu_arena_main");
        const auto old_target =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "gpu_arena_scratch");
        const auto old_buffer =
            GET_MODULE(FrameGraphResourceContainer)
                .getBufferIdByName(
                    "gpu_arena_buffer");
        const auto old_task =
            GET_MODULE(ComputeTaskContainer)
                .getComputeTaskIdByName(
                    "gpu_arena_compute");
        REQUIRE(isConcreteRenderTarget(old_target));
        REQUIRE(isValidFrameGraphBufferId(old_buffer));
        REQUIRE(old_task.value >= 0);

        auto batch_config_json =
            gpuArenaRenderingConfig();
        auto &batch_targets =
            batch_config_json["render_targets"];
        batch_targets.erase(
            std::remove_if(
                batch_targets.begin(),
                batch_targets.end(),
                [](const auto &target) {
                    return target.value(
                               "name",
                               std::string{})
                           .find("velocity") !=
                           std::string::npos;
                }),
            batch_targets.end());
        auto &batch_passes =
            batch_config_json["rendering_passes"][0]
                             ["passes"];
        batch_passes.erase(
            std::remove_if(
                batch_passes.begin(),
                batch_passes.end(),
                [](const auto &pass) {
                    return pass.value(
                               "type",
                               std::string{}) ==
                           "velocity";
                }),
            batch_passes.end());
        const auto batch_config =
            Pelican::TestSupport::renderGraphRootWithTaskFeature(batch_config_json, temp_dir).dump();

        RenderingPassConfigRegistrationDependencies::
            Options batch_flat_options;
        batch_flat_options.gpu_owner_scope =
            "render_pipeline/flat";
        RenderingPassConfigRegistrationDependencies::
            Options batch_xr_options;
        batch_xr_options.graph_variant =
            RenderPipelineGraphVariant::xr;
        batch_xr_options.publish_enabled_features =
            false;
        batch_xr_options.gpu_owner_scope =
            "render_pipeline/flat";
        batch_xr_options
            .validate_prepared_generation =
            [](const RendererRuntimeGeneration &) {
                throw std::runtime_error(
                    "Injected variant-batch validation failure");
            };
        std::vector<
            RenderingPassConfigRegistrationDependencies>
            failed_variant_batch;
        failed_variant_batch.push_back(
            gpuArenaRegistrationDependencies(
                std::move(batch_flat_options)));
        failed_variant_batch.push_back(
            gpuArenaRegistrationDependencies(
                std::move(batch_xr_options)));
#if PELICAN_WITH_OPENXR
        constexpr auto variant_batch_failure =
            "Injected variant-batch validation failure";
#else
        constexpr auto variant_batch_failure =
            "XR graph variant is unavailable in this build";
#endif
        REQUIRE_THROWS_WITH(
            registerRenderingPassConfigVariantsFromJsonData(
                batch_config, {16, 16},
                std::move(failed_variant_batch)),
            variant_batch_failure);
        REQUIRE(runtime.snapshot() == generation);
        REQUIRE(
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw,
                &debug_text, &gizmo) == committed);
        REQUIRE(
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "gpu_arena_scratch") ==
            old_target);
        REQUIRE(
            GET_MODULE(FrameGraphResourceContainer)
                .getBufferIdByName(
                    "gpu_arena_buffer") ==
            old_buffer);
        REQUIRE(
            GET_MODULE(ComputeTaskContainer)
                .getComputeTaskIdByName(
                    "gpu_arena_compute") ==
            old_task);

        auto replacement_config =
            gpuArenaRenderingConfig();
        replacement_config["features"].erase(
            std::remove(
                replacement_config["features"].begin(),
                replacement_config["features"].end(),
                "engine://features/debug_text.json"),
            replacement_config["features"].end());
        replacement_config["features"].erase(
            std::remove(
                replacement_config["features"].begin(),
                replacement_config["features"].end(),
                "engine://features/gizmo.json"),
            replacement_config["features"].end());
        replacement_config["buffers"][0]["size"] = 32;
        auto &replacement_targets =
            replacement_config["render_targets"];
        replacement_targets.erase(
            std::remove_if(
                replacement_targets.begin(),
                replacement_targets.end(),
                [](const auto &target) {
                    const auto &name =
                        target.at("name");
                    return name == "gpu_arena_velocity" ||
                           name ==
                               "gpu_arena_velocity_depth";
                }),
            replacement_targets.end());
        auto &replacement_passes =
            replacement_config["rendering_passes"][0]
                              ["passes"];
        replacement_passes.erase(
            std::remove_if(
                replacement_passes.begin(),
                replacement_passes.end(),
                [](const auto &pass) {
                    return pass.at("name") ==
                           "gpu_arena_velocity_pass";
                }),
            replacement_passes.end());

        RenderingPassConfigRegistrationDependencies::
            Options failed_replacement_options;
        failed_replacement_options.fault_point =
            RenderPipelineGpuRegistrationFaultPoint::
                after_runtime_prepare;
        REQUIRE_THROWS_WITH(
            registerRenderingPassConfigFromJsonData(
                Pelican::TestSupport::renderGraphRootWithTaskFeature(replacement_config, temp_dir).dump(), {16, 16},
                gpuArenaRegistrationDependencies(
                    std::move(
                        failed_replacement_options))),
            "Injected render pipeline GPU registration failure");
        REQUIRE(runtime.snapshot() == generation);
        REQUIRE(
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw,
                &debug_text, &gizmo) == committed);
        REQUIRE(
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "gpu_arena_scratch") ==
            old_target);
        REQUIRE(
            GET_MODULE(FrameGraphResourceContainer)
                .getBufferIdByName(
                    "gpu_arena_buffer") ==
            old_buffer);
        REQUIRE(
            GET_MODULE(ComputeTaskContainer)
                .getComputeTaskIdByName(
                    "gpu_arena_compute") ==
            old_task);

        const auto replaced =
            registerRenderingPassConfigFromJsonData(
                Pelican::TestSupport::renderGraphRootWithTaskFeature(replacement_config, temp_dir).dump(), {16, 16},
                gpuArenaRegistrationDependencies({}));
        REQUIRE(replaced.runtime_generation == 2);
        auto replacement_generation =
            runtime.snapshot();
        REQUIRE(
            replacement_generation->name_to_id.at(
                "gpu_arena_main") == old_program_id);
        const auto new_target =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "gpu_arena_scratch");
        const auto new_buffer =
            GET_MODULE(FrameGraphResourceContainer)
                .getBufferIdByName(
                    "gpu_arena_buffer");
        const auto new_task =
            GET_MODULE(ComputeTaskContainer)
                .getComputeTaskIdByName(
                    "gpu_arena_compute");
        REQUIRE(new_target != old_target);
        REQUIRE(new_buffer != old_buffer);
        REQUIRE(new_task != old_task);
        REQUIRE(
            GET_MODULE(FrameGraphResourceContainer)
                .bufferSize(new_buffer) == 32);
        REQUIRE_FALSE(isConcreteRenderTarget(
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "gpu_arena_velocity")));

        const auto *old_program =
            generation->find(old_program_id);
        const auto *new_program =
            replacement_generation->find(
                old_program_id);
        REQUIRE(old_program != nullptr);
        REQUIRE(new_program != nullptr);
        REQUIRE(
            old_program->frame_graph
                .render_target_bindings.at(
                    "gpu_arena_scratch") ==
            old_target);
        REQUIRE(
            new_program->frame_graph
                .render_target_bindings.at(
                    "gpu_arena_scratch") ==
            new_target);
        REQUIRE(
            old_program->frame_graph.buffer_bindings.at(
                "gpu_arena_buffer") == old_buffer);
        REQUIRE(
            new_program->frame_graph.buffer_bindings.at(
                "gpu_arena_buffer") == new_buffer);
        REQUIRE(old_program->frame_graph
                    .render_target_bindings.contains(
                        "gpu_arena_velocity"));
        REQUIRE_FALSE(
            new_program->frame_graph
                .render_target_bindings.contains(
                    "gpu_arena_velocity"));

        const auto both_generations =
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw, &debug_text,
                &gizmo);
        REQUIRE(both_generations.render_targets >
                committed.render_targets);
        REQUIRE(both_generations.frame_graph_buffers >
                committed.frame_graph_buffers);
        REQUIRE(both_generations.compute_tasks >
                committed.compute_tasks);
        REQUIRE(
            GET_MODULE(RenderTargetContainer)
                .getMetadata(old_target)
                .name == "gpu_arena_scratch");
        generation.reset();
        const auto retired =
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw, &debug_text,
                &gizmo);
        REQUIRE(retired.render_targets <
                both_generations.render_targets);
        REQUIRE(retired.frame_graph_buffers <
                both_generations.frame_graph_buffers);
        REQUIRE(retired.compute_tasks <
                both_generations.compute_tasks);
        REQUIRE(retired.gizmo_passes ==
                baseline.gizmo_passes);
        REQUIRE_THROWS(
            GET_MODULE(RenderTargetContainer)
                .getMetadata(old_target));

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            ex, "Vulkan GPU arena transaction unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "typed image subresources execute a two-stage depth pyramid and "
    "rebind after resize",
    "[headless][render][compute][subresource][depth-pyramid][wp209b][wp215]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        std::filesystem::create_directories(
            temp_dir / "shaders");
        writeTextFile(
            temp_dir / "scene.json",
            R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":2,"models":[]})json");
        writeTextFile(
            temp_dir / "shaders" /
                "depth_pyramid_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "depth_pyramid_seed.comp",
            depthPyramidSeedComputeShader());
        writeTextFile(
            temp_dir / "shaders" /
                "depth_pyramid_initialize.frag",
            pipelineReloadFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "depth_pyramid_reduce.comp",
            depthPyramidReduceComputeShader());
        writeTextFile(
            temp_dir / "shaders" /
                "depth_pyramid_present.frag",
            depthPyramidPresentFragmentShader());
        writeTextFile(
            temp_dir / "pipeline.json",
            Pelican::TestSupport::renderGraphRootWithTaskFeature(depthPyramidRenderingConfig(), temp_dir).dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "pipeline.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "depth_pyramid_main";
        GET_MODULE(ProjectSource)
            .setSourceByData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false);
        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{32, 32};
        launch.headless_frames = 3;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        auto &render_targets =
            GET_MODULE(RenderTargetContainer);
        auto &compute_tasks =
            GET_MODULE(ComputeTaskContainer);
        auto &fullscreen_passes =
            GET_MODULE(FullscreenPassContainer);
        const auto target =
            render_targets.getRenderTargetIdByName(
                "depth_pyramid");
        REQUIRE(isConcreteRenderTarget(target));
        REQUIRE(
            render_targets.getMetadata(target)
                .mip_levels == 6);
        REQUIRE(
            render_targets.getMetadata(target)
                .array_layers == 2);

        const auto mip_zero =
            ImageSubresourceRange{
                .base_mip_level = 0,
                .base_array_layer = 1,
            };
        const auto mip_one =
            ImageSubresourceRange{
                .base_mip_level = 1,
                .base_array_layer = 1,
            };
        const auto initial_mip_zero_view =
            render_targets.getImageSubresourceView(
                target, mip_zero,
                ImageSubresourceViewDimension::two_d);
        const auto initial_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one,
                ImageSubresourceViewDimension::two_d);
        REQUIRE(
            initial_mip_zero_view !=
            initial_mip_one_view);

        const auto seed_task =
            compute_tasks.getComputeTaskIdByName(
                "depth_seed");
        const auto reduce_task =
            compute_tasks.getComputeTaskIdByName(
                "depth_reduce");
        REQUIRE(seed_task.value >= 0);
        REQUIRE(reduce_task.value >= 0);
        REQUIRE(
            compute_tasks
                .dispatchGroupsForTesting(
                    seed_task) ==
            (std::array<std::uint32_t, 3>{
                4, 4, 1}));
        REQUIRE(
            compute_tasks
                .dispatchGroupsForTesting(
                    reduce_task) ==
            (std::array<std::uint32_t, 3>{
                2, 2, 1}));
        REQUIRE_THROWS_WITH(
            compute_tasks.setDispatchGroups(
                reduce_task, 1, 1, 1),
            Catch::Matchers::ContainsSubstring(
                "extent-derived"));
        const auto initial_seed_views =
            compute_tasks.boundImageViewsForTesting(
                seed_task, 0);
        const auto initial_reduce_views =
            compute_tasks.boundImageViewsForTesting(
                reduce_task, 0);
        REQUIRE(
            initial_seed_views ==
            std::vector<vk::ImageView>{
                initial_mip_zero_view});
        REQUIRE(initial_reduce_views.size() == 2);
        REQUIRE(
            std::find(
                initial_reduce_views.begin(),
                initial_reduce_views.end(),
                initial_mip_zero_view) !=
            initial_reduce_views.end());
        REQUIRE(
            std::find(
                initial_reduce_views.begin(),
                initial_reduce_views.end(),
                initial_mip_one_view) !=
            initial_reduce_views.end());
        const auto initial_seed_revision =
            compute_tasks.bindingRevisionForTesting(
                seed_task);
        const auto initial_reduce_revision =
            compute_tasks.bindingRevisionForTesting(
                reduce_task);

        const auto generation =
            GET_MODULE(FrameGraphRuntimeContainer)
                .snapshot();
        REQUIRE(generation != nullptr);
        const auto rendering_pass_id =
            GET_MODULE(RenderingPassContainer)
                .getRenderingPassIdByName(
                    "depth_pyramid_main");
        const auto *program =
            generation->find(rendering_pass_id);
        REQUIRE(program != nullptr);
        const auto present =
            std::find_if(
                program->rendering_pass.passes.begin(),
                program->rendering_pass.passes.end(),
                [](const auto &pass) {
                    return pass.definition.name ==
                           "depth_present";
                });
        REQUIRE(
            present !=
            program->rendering_pass.passes.end());
        const auto initial_present_revision =
            fullscreen_passes
                .inputBindingRevisionForTesting(
                    present->pass_id);
        REQUIRE(
            fullscreen_passes
                .boundInputImageViewsForTesting(
                    present->pass_id) ==
            std::vector<vk::ImageView>{
                initial_mip_one_view});

        const auto render_and_require_green =
            [&] {
                engine_time.advance();
                renderer.render();
                GET_MODULE(VulkanManageCore)
                    .waitIdle();
                const auto pixels =
                    GET_MODULE(RenderTarget)
                        .readbackLastFrameRGBA8();
                REQUIRE(
                    pixels.size() ==
                    32u * 32u * 4u);
                const auto center =
                    (16u * 32u + 16u) * 4u;
                const auto red =
                    static_cast<unsigned>(
                        pixels[center]);
                const auto green =
                    static_cast<unsigned>(
                        pixels[center + 1]);
                const auto blue =
                    static_cast<unsigned>(
                        pixels[center + 2]);
                REQUIRE(green > 170);
                REQUIRE(green > red + 70);
                REQUIRE(green > blue + 30);
        };
        render_and_require_green();

        auto stale_candidate =
            render_targets.prepareForExtent(
                {48, 32});
        REQUIRE(stale_candidate.valid());
        REQUIRE(stale_candidate.extent() ==
                (vk::Extent2D{48, 32}));
        REQUIRE(stale_candidate.targetCount() > 0);
        REQUIRE(
            render_targets.getMetadata(target)
                .extent == (vk::Extent2D{32, 32}));
        REQUIRE(
            render_targets.getImageSubresourceView(
                target, mip_zero,
                ImageSubresourceViewDimension::two_d) ==
            initial_mip_zero_view);

        RendererTestAccess::recreateRenderTargetsAndRebind(renderer,
                {64, 32});
        REQUIRE_THROWS_WITH(
            render_targets.publishPreparedExtent(
                std::move(stale_candidate)),
            "render target extent candidate is stale");
        REQUIRE(
            compute_tasks
                .dispatchGroupsForTesting(
                    seed_task) ==
            (std::array<std::uint32_t, 3>{
                8, 4, 1}));
        REQUIRE(
            compute_tasks
                .dispatchGroupsForTesting(
                    reduce_task) ==
            (std::array<std::uint32_t, 3>{
                4, 2, 1}));
        REQUIRE(
            render_targets.getMetadata(target)
                .mip_levels == 7);
        REQUIRE(
            render_targets.getMetadata(target)
                .array_layers == 2);
        const auto resized_mip_zero_view =
            render_targets.getImageSubresourceView(
                target, mip_zero,
                ImageSubresourceViewDimension::two_d);
        const auto resized_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one,
                ImageSubresourceViewDimension::two_d);
        REQUIRE(
            resized_mip_zero_view !=
            initial_mip_zero_view);
        REQUIRE(
            resized_mip_one_view !=
            initial_mip_one_view);
        REQUIRE(
            compute_tasks.bindingRevisionForTesting(
                seed_task) >
            initial_seed_revision);
        REQUIRE(
            compute_tasks.bindingRevisionForTesting(
                reduce_task) >
            initial_reduce_revision);
        REQUIRE(
            compute_tasks.boundImageViewsForTesting(
                seed_task, 0) ==
            std::vector<vk::ImageView>{
                resized_mip_zero_view});
        const auto resized_reduce_views =
            compute_tasks.boundImageViewsForTesting(
                reduce_task, 0);
        REQUIRE(resized_reduce_views.size() == 2);
        REQUIRE(
            std::find(
                resized_reduce_views.begin(),
                resized_reduce_views.end(),
                resized_mip_zero_view) !=
            resized_reduce_views.end());
        REQUIRE(
            std::find(
                resized_reduce_views.begin(),
                resized_reduce_views.end(),
                resized_mip_one_view) !=
            resized_reduce_views.end());
        REQUIRE(
            fullscreen_passes
                .inputBindingRevisionForTesting(
                    present->pass_id) >
            initial_present_revision);
        REQUIRE(
            fullscreen_passes
                .boundInputImageViewsForTesting(
                    present->pass_id) ==
            std::vector<vk::ImageView>{
                resized_mip_one_view});
        render_and_require_green();

        const auto pre_reload_reduce_revision =
            compute_tasks.bindingRevisionForTesting(
                reduce_task);
        const auto pre_reload_reduce_views =
            compute_tasks.boundImageViewsForTesting(
                reduce_task, 0);
        const auto pre_reload_mip_zero_view =
            render_targets.getImageSubresourceView(
                target, mip_zero,
                ImageSubresourceViewDimension::two_d);
        const auto pre_reload_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one,
                ImageSubresourceViewDimension::two_d);
        REQUIRE(
            std::find(
                pre_reload_reduce_views.begin(),
                pre_reload_reduce_views.end(),
                pre_reload_mip_zero_view) !=
            pre_reload_reduce_views.end());
        REQUIRE(
            std::find(
                pre_reload_reduce_views.begin(),
                pre_reload_reduce_views.end(),
                pre_reload_mip_one_view) !=
            pre_reload_reduce_views.end());
        writeTextFile(
            temp_dir / "shaders" /
                "depth_pyramid_reduce.comp",
            std::string{
                depthPyramidReduceComputeShader()} +
                "\n// subresource reload probe\n");
        REQUIRE(
            GET_MODULE(watch::ReloadService)
                .applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "shaders/depth_pyramid_reduce.comp"),
                        watch::ReloadKind::modified,
                        {}, 1}));
        render_and_require_green();
        REQUIRE(
            compute_tasks.bindingRevisionForTesting(
                reduce_task) >
            pre_reload_reduce_revision);
        const auto reloaded_reduce_views =
            compute_tasks.boundImageViewsForTesting(
                reduce_task, 0);
        const auto reloaded_mip_zero_view =
            render_targets.getImageSubresourceView(
                target, mip_zero,
                ImageSubresourceViewDimension::two_d);
        const auto reloaded_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one,
                ImageSubresourceViewDimension::two_d);
        REQUIRE(reloaded_reduce_views.size() == 2);
        REQUIRE(
            std::find(
                reloaded_reduce_views.begin(),
                reloaded_reduce_views.end(),
                reloaded_mip_zero_view) !=
            reloaded_reduce_views.end());
        REQUIRE(
            std::find(
                reloaded_reduce_views.begin(),
                reloaded_reduce_views.end(),
                reloaded_mip_one_view) !=
            reloaded_reduce_views.end());
        REQUIRE(
            fullscreen_passes
                .boundInputImageViewsForTesting(
                    present->pass_id) ==
            std::vector<vk::ImageView>{
                reloaded_mip_one_view});

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan depth-pyramid subresource rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "ray-query acceleration structures remain unbuilt when not requested",
    "[headless][gpu][wp281][ray-query]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeRayQueryTestProject(temp_dir, false);
        configureRayQueryTestRuntime(temp_dir);

        auto &renderer = GET_MODULE(Renderer);
        auto &geometry = GET_MODULE(VertBufContainer);
        auto primitive = geometry.addPrimitiveEntry(
            makeScreenQuad(0.25F, 0.0F));
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        ModelTemplate model;
        model.asset_id = ModelAssetId{2801};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material =
                    GET_MODULE(StandardMaterialResource)
                        .standardTransparentMaterial(),
                .primitives = {primitive},
            },
        };
        GET_MODULE(PolygonInstanceContainer)
            .placeModelInstance(model);
        renderer.render();
        GET_MODULE(VulkanManageCore).waitIdle();

        const auto diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK_FALSE(diagnostics.requested);
        CHECK(diagnostics.blas_build_count == 0);
        CHECK(diagnostics.tlas_build_count == 0);
        CHECK(diagnostics.active_blas_count == 0);

        const auto plan = renderer.currentFramePlanJson();
        CHECK_FALSE(
            plan.at("ray_query_acceleration_structures")
                .at("requested")
                .get<bool>());
        bool registered = false;
        for (const auto &scope :
             plan.at("gpu_resource_arena").at("scopes")) {
            for (const auto &resource :
                 scope.at("resources")) {
                registered = registered ||
                             resource.at("kind") ==
                                 "acceleration_structure";
            }
        }
        CHECK_FALSE(registered);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan non-ray-query headless rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "ray-query static BLAS and frame TLAS invalidate on real pool growth and model rebuild",
    "[headless][gpu][wp281][ray-query]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeRayQueryTestProject(temp_dir, true);
        configureRayQueryTestRuntime(temp_dir);

        auto &renderer = GET_MODULE(Renderer);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        REQUIRE(vkcore.getRuntimeCapabilities().ray_query);
        auto &geometry = GET_MODULE(VertBufContainer);
        auto &standard =
            GET_MODULE(StandardMaterialResource);
        auto &materials = GET_MODULE(MaterialContainer);

        const auto skinned_material =
            materials.registerMaterial(MaterialInfo{
                .vert_shader = standard.skinnedVertShader(),
                .frag_shader = standard.standardFragShader(),
                .skinned = true,
                .base_color_texture =
                    standard.transparentTexture(),
                .metallic_roughness_texture =
                    standard.metallicRoughnessDefaultTexture(),
                .normal_texture =
                    standard.normalDefaultTexture(),
                .emissive_texture =
                    standard.emissiveDefaultTexture(),
                .occlusion_texture =
                    standard.occlusionDefaultTexture(),
            });

        auto static_primitive = geometry.addPrimitiveEntry(
            makeScreenQuad(0.2F, 0.0F));
        static_primitive.mesh_index = 0;
        static_primitive.primitive_index = 0;

        auto blas_ineligible_data =
            makeScreenQuad(0.2F, 0.0F);
        blas_ineligible_data.indices.clear();
        REQUIRE(blas_ineligible_data.pos.size() == 4);
        auto blas_ineligible_primitive =
            geometry.addPrimitiveEntry(
                std::move(blas_ineligible_data));
        REQUIRE(blas_ineligible_primitive.index_count == 4);
        blas_ineligible_primitive.mesh_index = 4;
        blas_ineligible_primitive.primitive_index = 0;

        auto morph_primitive = geometry.addPrimitiveEntry(
            makeScreenQuad(0.2F, 0.0F));
        morph_primitive.mesh_index = 1;
        morph_primitive.primitive_index = 0;
        morph_primitive.morph_deformed = true;

        auto vat_primitive = geometry.addPrimitiveEntry(
            makeScreenQuad(0.2F, 0.0F));
        vat_primitive.mesh_index = 2;
        vat_primitive.primitive_index = 0;
        vat_primitive.index_count = 1;

        auto skinned_data = makeScreenQuad(0.2F, 0.0F);
        skinned_data.joint.assign(
            skinned_data.pos.size(), glm::i16vec4{0});
        skinned_data.weight.assign(
            skinned_data.pos.size(),
            glm::vec4{1.0F, 0.0F, 0.0F, 0.0F});
        auto skinned_primitive =
            geometry.addSkinnedPrimitiveEntry(
                std::move(skinned_data));
        skinned_primitive.mesh_index = 3;
        skinned_primitive.primitive_index = 0;

        ModelTemplate model;
        model.asset_id = ModelAssetId{2811};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material =
                    standard.standardTransparentMaterial(),
                .primitives = {
                    static_primitive,
                    morph_primitive,
                    vat_primitive,
                    blas_ineligible_primitive,
                },
            },
            ModelTemplate::MaterialPrimitives{
                .material = skinned_material,
                .primitives = {skinned_primitive},
            },
        };
        auto &instances =
            GET_MODULE(PolygonInstanceContainer);
        const auto model_instance =
            instances.placeModelInstance(model);
        const std::array skin_palette{
            glm::mat4{1.0F}};
        instances.setSkinningPalette(
            model_instance, skin_palette);
        auto &camera = GET_MODULE(Camera);
        camera.setPos({0.0F, 0.0F, 2.0F});
        camera.setDir({0.0F, 0.0F, -1.0F});
        camera.setUp({0.0F, 1.0F, 0.0F});

        renderer.render();
        auto initial = RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        REQUIRE(initial.requested);
        CHECK(initial.blas_build_count == 3);
        CHECK(initial.tlas_build_count == 1);
        CHECK(initial.active_blas_count == 3);
        CHECK(initial.tlas_instance_count == 3);
        CHECK(initial.excluded.primitive_count == 2);
        CHECK(initial.excluded.instance_count == 2);
        CHECK(initial.excluded.skinned_primitive_count == 0);
        CHECK(initial.excluded.morph_primitive_count == 0);
        CHECK(initial.excluded.vat_primitive_count == 0);
        CHECK(initial.excluded.blas_ineligible_primitive_count == 2);
        REQUIRE(initial.excluded.skinned_names.empty());
        REQUIRE(initial.excluded.morph_names.empty());
        REQUIRE(initial.excluded.vat_names.empty());
        REQUIRE(initial.excluded.blas_ineligible_names.size() == 2);
        REQUIRE(initial.active_blas_inputs.size() == 3);
        const auto old_blas_input =
            initial.active_blas_inputs.front();

        // Static geometry is reused; the two deformed primitives refit each frame.
        instances.setTrs(
            model_instance, {0.1F, 0.0F, 0.0F},
            glm::quat{1.0F, 0.0F, 0.0F, 0.0F},
            {1.0F, 1.0F, 1.0F});
        renderer.render();
        const auto transformed =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK(transformed.blas_build_count == initial.blas_build_count);
        CHECK(transformed.blas_update_count == initial.blas_update_count + 2);
        CHECK(transformed.tlas_build_count ==
              initial.tlas_build_count + 1);
        CHECK(transformed.geometry_pool_invalidation_count == 0);
        CHECK(transformed.model_rebuild_invalidation_count == 0);

        const auto rebuild_generation =
            instances.rayQueryGeometryGeneration();
        instances.rebuildModelInstances(model.asset_id, model);
        instances.setSkinningPalette(
            model_instance, skin_palette);
        REQUIRE(instances.rayQueryGeometryGeneration() ==
                rebuild_generation + 1);
        renderer.render();
        const auto rebuilt =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK(rebuilt.blas_build_count ==
              transformed.blas_build_count + 3);
        CHECK(rebuilt.blas_update_count == transformed.blas_update_count);
        CHECK(rebuilt.model_rebuild_invalidation_count == 1);

        const auto old_index_capacity =
            geometry.indexCapacityForTesting();
        const auto old_vertex_capacity =
            geometry.vertexCapacityForTesting(false);
        const auto allocated_indices =
            geometry.allocatedIndexCountForTesting();
        const auto allocated_vertices =
            geometry.allocatedVertexCountForTesting(false);
        REQUIRE(allocated_indices < old_index_capacity);
        REQUIRE(allocated_vertices < old_vertex_capacity);

        // One model allocation deliberately crosses both global pool
        // capacities. vertex_count fills the remaining static-vertex range
        // plus one; index_count does the same and is rounded up to a complete
        // triangle. This calls ensureVertexCapacity and ensureIndexCapacity,
        // rather than simulating their notification in a unit test.
        const auto crossing_vertex_count =
            static_cast<std::uint32_t>(
                old_vertex_capacity - allocated_vertices + 1);
        auto crossing_index_count =
            static_cast<std::uint32_t>(
                old_index_capacity - allocated_indices + 1);
        crossing_index_count +=
            (3 - crossing_index_count % 3) % 3;
        CommonPolygonVertData crossing_data;
        crossing_data.pos.resize(crossing_vertex_count);
        for (std::uint32_t index = 0;
             index < crossing_vertex_count; ++index) {
            crossing_data.pos[index] = {
                static_cast<float>(index % 3),
                static_cast<float>((index / 3) % 3),
                0.0F,
            };
        }
        crossing_data.indices.resize(crossing_index_count);
        for (std::uint32_t index = 0;
             index < crossing_index_count; index += 3) {
            crossing_data.indices[index] = 0;
            crossing_data.indices[index + 1] = 1;
            crossing_data.indices[index + 2] = 2;
        }
        const auto old_pool_generation =
            geometry.geometryAddressGeneration();
        ModelTemplate capacity_crossing_model;
        capacity_crossing_model.asset_id = ModelAssetId{2812};
        capacity_crossing_model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material =
                    standard.standardTransparentMaterial(),
                .primitives = {geometry.addPrimitiveEntry(
                    std::move(crossing_data))},
            },
        };
        REQUIRE(geometry.indexCapacityForTesting() >
                old_index_capacity);
        REQUIRE(geometry.vertexCapacityForTesting(false) >
                old_vertex_capacity);
        REQUIRE(geometry.geometryAddressGeneration() >=
                old_pool_generation + 2);

        renderer.render();
        const auto reallocated =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK(reallocated.geometry_pool_invalidation_count == 1);
        CHECK(reallocated.blas_build_count ==
              rebuilt.blas_build_count + 3);
        REQUIRE(reallocated.active_blas_inputs.size() == 3);
        const auto &new_blas_input =
            reallocated.active_blas_inputs.front();
        CHECK(new_blas_input.index_device_address !=
              old_blas_input.index_device_address);
        CHECK(new_blas_input.vertex_device_address !=
              old_blas_input.vertex_device_address);
        const auto current_index_address =
            vkcore.getDevice().getBufferAddress(
                vk::BufferDeviceAddressInfo{
                    geometry.indexBuffer().buffer.get()});
        const auto current_vertex_address =
            vkcore.getDevice().getBufferAddress(
                vk::BufferDeviceAddressInfo{
                    geometry.vertexBuffer(false).buffer.get()});
        CHECK(new_blas_input.index_device_address ==
              current_index_address +
                  sizeof(std::uint32_t) *
                      static_primitive.index_offset);
        CHECK(new_blas_input.vertex_device_address ==
              current_vertex_address +
                  sizeof(CommonVertStruct) *
                      static_cast<std::uint32_t>(
                          static_primitive.vert_offset));

        const auto plan = renderer.currentFramePlanJson();
        CHECK(plan.at("ray_query_acceleration_structures")
                  .at("excluded")
                  .at("primitive_count") == 2);
        CHECK(plan.at("ray_query_acceleration_structures")
                  .at("excluded")
                  .at("blas_ineligible_primitive_count") == 2);
        bool registered = false;
        for (const auto &scope :
             plan.at("gpu_resource_arena").at("scopes")) {
            for (const auto &resource :
                 scope.at("resources")) {
                registered = registered ||
                             resource.at("kind") ==
                                 "acceleration_structure";
            }
        }
        CHECK(registered);

        vkcore.waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan ray-query acceleration-structure rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "ray query and RT pipeline shadow masks include skin and morph but exclude invalid ranges",
    "[headless][gpu][wp283][wp284][wp285][ray-query][ray-tracing][pixel]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        temp_dir = makeTempProjectDir();
        enum class BlockerKind {
            none,
            static_geometry,
            skinned,
            morph,
            invalid_geometry,
        };
        struct RenderResult {
            R8RenderTargetReadback mask;
            R8RenderTargetReadback pipeline_mask;
            std::vector<std::uint8_t> color;
            RayQueryAccelerationStructureDiagnostics diagnostics;
        };
        const auto render = [&](BlockerKind blocker,
                                bool raster_shadow,
                                float world_x = 0.0F) {
            FastModuleContainer modules;
            writeRtShadowMaskTestProject(
                temp_dir, raster_shadow, true);
            configureRayQueryTestRuntime(temp_dir);

            auto &renderer = GET_MODULE(Renderer);
            auto &vkcore = GET_MODULE(VulkanManageCore);
            REQUIRE(vkcore.getRuntimeCapabilities().ray_query);
            REQUIRE(
                vkcore.getRuntimeCapabilities().ray_tracing_pipeline);
            auto &standard =
                GET_MODULE(StandardMaterialResource);
            auto &materials = GET_MODULE(MaterialContainer);
            const auto make_material = [&](bool skinned) {
                return materials.registerMaterial(MaterialInfo{
                    .vert_shader =
                        skinned ? standard.skinnedVertShader()
                                : standard.standardVertShader(),
                    .frag_shader = standard.standardFragShader(),
                    .skinned = skinned,
                    .base_color_texture = standard.whiteTexture(),
                    .metallic_roughness_texture =
                        standard.metallicRoughnessDefaultTexture(),
                    .normal_texture = standard.normalDefaultTexture(),
                    .emissive_texture =
                        standard.emissiveDefaultTexture(),
                    .occlusion_texture =
                        standard.occlusionDefaultTexture(),
                });
            };
            const auto opaque_material = make_material(false);
            const auto skinned_material = make_material(true);

            auto &geometry = GET_MODULE(VertBufContainer);
            auto receiver = geometry.addPrimitiveEntry(
                makeScreenQuad(0.95F, 0.0F));
            receiver.mesh_index = 0;
            receiver.primitive_index = 0;
            ModelTemplate model;
            model.asset_id = ModelAssetId{2831};
            ModelTemplate::MaterialPrimitives opaque{
                .material = opaque_material,
                .primitives = {receiver},
            };
            if (blocker == BlockerKind::static_geometry ||
                blocker == BlockerKind::morph ||
                blocker == BlockerKind::invalid_geometry) {
                auto blocker_data = makeScreenQuad(0.18F, 0.5F);
                for (auto &position : blocker_data.pos) {
                    position.x -= 0.25F;
                }
                auto candidate = geometry.addPrimitiveEntry(
                    std::move(blocker_data));
                candidate.mesh_index = 1;
                candidate.primitive_index = 0;
                candidate.morph_deformed =
                    blocker == BlockerKind::morph;
                if (blocker == BlockerKind::invalid_geometry) candidate.index_count = 1;
                opaque.primitives.push_back(candidate);
            }
            model.material_primitives.push_back(
                std::move(opaque));
            if (blocker == BlockerKind::skinned) {
                auto data = makeScreenQuad(0.18F, 0.5F);
                for (auto &position : data.pos) {
                    position.x -= 0.25F;
                }
                data.joint.assign(
                    data.pos.size(), glm::i16vec4{0});
                data.weight.assign(
                    data.pos.size(),
                    glm::vec4{1.0F, 0.0F, 0.0F, 0.0F});
                auto candidate =
                    geometry.addSkinnedPrimitiveEntry(
                        std::move(data));
                candidate.mesh_index = 1;
                candidate.primitive_index = 0;
                model.material_primitives.push_back(
                    ModelTemplate::MaterialPrimitives{
                        .material = skinned_material,
                        .primitives = {candidate},
                    });
            }

            auto &instances =
                GET_MODULE(PolygonInstanceContainer);
            const auto instance =
                instances.placeModelInstance(model);
            if (world_x != 0.0F) {
                instances.setTrs(
                    instance, {world_x, 0.0F, 0.0F},
                    glm::quat{1.0F, 0.0F, 0.0F, 0.0F},
                    {1.0F, 1.0F, 1.0F});
            }
            if (blocker == BlockerKind::skinned) {
                const std::array palette{glm::mat4{1.0F}};
                instances.setSkinningPalette(instance, palette);
            }
            GET_MODULE(LightContainer).load({
                LightLoadEntry{
                    .name = "WP283 directional",
                    .component = LightCodecData{
                        .type = LightCodecType::Directional,
                        // A background sample at the cleared world origin
                        // intersects the x=-0.25 blocker along this ray.
                        // The corner assertions therefore exercise coverage,
                        // rather than relying on an incidental miss.
                        .direction = {0.5f, 0.0f, -1.0f},
                        .intensity = 4.0f,
                        .color = {1.0f, 1.0f, 1.0f}},
                },
            });
            auto &camera = GET_MODULE(Camera);
            camera.setPos({world_x, 0.0F, 2.0F});
            camera.setDir({0.0F, 0.0F, -1.0F});
            camera.setUp({0.0F, 1.0F, 0.0F});

            renderer.render();
            vkcore.waitIdle();
            return RenderResult{
                .mask =
                    RendererTestAccess::readR8RenderTarget(renderer,
                        "rt_shadow_mask"),
                .pipeline_mask =
                    RendererTestAccess::readR8RenderTarget(renderer,
                        "rt_shadow_mask_pipeline"),
                .color = GET_MODULE(RenderTarget)
                             .readbackLastFrameRGBA8(),
                .diagnostics =
                    RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer),
            };
        };
        const auto dark_count = [](const auto &pixels) -> std::size_t {
            return static_cast<std::size_t>(std::count_if(
                pixels.begin(), pixels.end(),
                [](const auto value) { return value < 64; }));
        };

        const auto baseline =
            render(BlockerKind::none, false);
        const auto static_only =
            render(BlockerKind::static_geometry, false);
        const auto far_baseline =
            render(BlockerKind::none, false, 128.0F);
        const auto far_static =
            render(BlockerKind::static_geometry, false, 128.0F);
        const auto skinned =
            render(BlockerKind::skinned, false);
        const auto morph =
            render(BlockerKind::morph, false);
        const auto invalid =
            render(BlockerKind::invalid_geometry, false);
        const auto require_pipeline_match =
            [](const RenderResult &result) {
                REQUIRE(result.pipeline_mask.extent ==
                        result.mask.extent);
                REQUIRE(result.pipeline_mask.pixels ==
                        result.mask.pixels);
            };
        require_pipeline_match(baseline);
        require_pipeline_match(static_only);
        require_pipeline_match(far_baseline);
        require_pipeline_match(far_static);
        require_pipeline_match(skinned);
        require_pipeline_match(morph);
        require_pipeline_match(invalid);
        REQUIRE(baseline.mask.extent.width == 32);
        REQUIRE(baseline.mask.extent.height == 32);
        REQUIRE(dark_count(baseline.mask.pixels) == 0);
        REQUIRE(dark_count(far_baseline.mask.pixels) == 0);
        // With no coverage guard these background pixels cast from the
        // cleared origin and hit the deliberately aligned blocker.
        REQUIRE(r8PixelAt(static_only.mask, 0, 0) > 192);
        REQUIRE(r8PixelAt(static_only.mask, 31, 31) > 192);
        REQUIRE(r8PixelAt(static_only.mask, 16, 16) < 64);
        REQUIRE(r8PixelAt(static_only.mask, 5, 16) > 192);
        REQUIRE(r8PixelAt(far_static.mask, 16, 16) < 64);
        REQUIRE(r8PixelAt(far_static.mask, 5, 16) > 192);
        // Skin and morph blockers now participate in both ray paths.
        REQUIRE(r8PixelAt(skinned.mask, 16, 16) < 64);
        REQUIRE(r8PixelAt(morph.mask, 16, 16) < 64);
        REQUIRE(dark_count(invalid.mask.pixels) == 0);
        REQUIRE(skinned.diagnostics.excluded.skinned_primitive_count == 0);
        REQUIRE(morph.diagnostics.excluded.morph_primitive_count == 0);
        REQUIRE(invalid.diagnostics.excluded.blas_ineligible_primitive_count == 1);

        const auto raster =
            render(BlockerKind::static_geometry, true);
        require_pipeline_match(raster);
        REQUIRE(raster.color.size() == static_only.color.size());
        REQUIRE(raster.mask.pixels.size() ==
                static_only.mask.pixels.size());
        std::size_t raster_shadow_pixels = 0;
        std::size_t overlapping_shadow_pixels = 0;
        std::size_t differing_mask_pixels = 0;
        for (std::size_t pixel = 0;
             pixel < raster.mask.pixels.size(); ++pixel) {
            const auto rgba = pixel * 4;
            const auto without_shadow =
                static_cast<int>(static_only.color[rgba]) +
                static_cast<int>(static_only.color[rgba + 1]) +
                static_cast<int>(static_only.color[rgba + 2]);
            const auto with_shadow =
                static_cast<int>(raster.color[rgba]) +
                static_cast<int>(raster.color[rgba + 1]) +
                static_cast<int>(raster.color[rgba + 2]);
            const bool raster_shadowed =
                without_shadow > with_shadow + 12;
            const bool ray_shadowed =
                raster.mask.pixels[pixel] < 64;
            raster_shadow_pixels += raster_shadowed;
            overlapping_shadow_pixels +=
                raster_shadowed && ray_shadowed;
            differing_mask_pixels +=
                raster_shadowed != ray_shadowed;
        }
        REQUIRE(raster_shadow_pixels >= 4);
        REQUIRE(overlapping_shadow_pixels >= 1);
        REQUIRE(differing_mask_pixels >= 1);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan ray-query shadow-mask pixel rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "ray query and RT pipeline shadow masks match for an ineligible geometry scene",
    "[headless][gpu][wp284][wp285][ray-query][ray-tracing][empty]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeRtShadowMaskTestProject(temp_dir, false, true);
        configureRayQueryTestRuntime(temp_dir);

        auto &renderer = GET_MODULE(Renderer);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        REQUIRE(vkcore.getRuntimeCapabilities().ray_query);
        REQUIRE(
            vkcore.getRuntimeCapabilities().ray_tracing_pipeline);
        auto &standard =
            GET_MODULE(StandardMaterialResource);
        auto &materials = GET_MODULE(MaterialContainer);
        const auto skinned_material =
            materials.registerMaterial(MaterialInfo{
                .vert_shader = standard.skinnedVertShader(),
                .frag_shader = standard.standardFragShader(),
                .skinned = true,
                .base_color_texture = standard.whiteTexture(),
                .metallic_roughness_texture =
                    standard.metallicRoughnessDefaultTexture(),
                .normal_texture = standard.normalDefaultTexture(),
                .emissive_texture =
                    standard.emissiveDefaultTexture(),
                .occlusion_texture =
                    standard.occlusionDefaultTexture(),
            });
        auto data = makeScreenQuad(0.95F, 0.0F);
        data.joint.assign(data.pos.size(), glm::i16vec4{0});
        data.weight.assign(
            data.pos.size(),
            glm::vec4{1.0F, 0.0F, 0.0F, 0.0F});
        auto primitive =
            GET_MODULE(VertBufContainer)
                .addSkinnedPrimitiveEntry(std::move(data));
        primitive.index_count = 1;
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        ModelTemplate model;
        model.asset_id = ModelAssetId{2841};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = skinned_material,
                .primitives = {primitive},
            },
        };
        auto &instances =
            GET_MODULE(PolygonInstanceContainer);
        const auto instance = instances.placeModelInstance(model);
        const std::array palette{glm::mat4{1.0F}};
        instances.setSkinningPalette(instance, palette);
        GET_MODULE(LightContainer).load({
            LightLoadEntry{
                .name = "WP284 empty-TLAS directional",
                .component = LightCodecData{
                    .type = LightCodecType::Directional,
                    .direction = {0.0f, 0.0f, -1.0f},
                    .intensity = 1.0f,
                    .color = {1.0f, 1.0f, 1.0f}},
            },
        });
        auto &camera = GET_MODULE(Camera);
        camera.setPos({0.0F, 0.0F, 2.0F});
        camera.setDir({0.0F, 0.0F, -1.0F});
        camera.setUp({0.0F, 1.0F, 0.0F});

        renderer.render();
        vkcore.waitIdle();
        const auto mask =
            RendererTestAccess::readR8RenderTarget(renderer,
                "rt_shadow_mask");
        const auto pipeline_mask =
            RendererTestAccess::readR8RenderTarget(renderer,
                "rt_shadow_mask_pipeline");
        REQUIRE(mask.extent == vk::Extent2D{32, 32});
        REQUIRE(pipeline_mask.extent == mask.extent);
        REQUIRE(pipeline_mask.pixels == mask.pixels);
        REQUIRE(std::all_of(
            mask.pixels.begin(), mask.pixels.end(),
            [](const auto value) { return value > 192; }));
        const auto diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        REQUIRE(diagnostics.requested);
        REQUIRE(diagnostics.tlas_build_count == 1);
        REQUIRE(diagnostics.active_blas_count == 0);
        REQUIRE(diagnostics.tlas_instance_count == 0);
        REQUIRE(diagnostics.excluded.instance_count == 1);
        REQUIRE(
            diagnostics.excluded.blas_ineligible_primitive_count == 1);
        const auto plan = renderer.currentFramePlanJson();
        REQUIRE(
            plan.at("ray_query_acceleration_structures")
                .at("excluded")
                .at("instance_count") == 1);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan empty ray-query scene rendering unavailable");
        throw;
    }
#endif
}

TEST_CASE(
    "embedded ray query and RT pipeline shaders render matching real shadows",
    "[headless][gpu][wp283][wp284][wp285][ray-query][ray-tracing][embedded]") {
    setupLogger();
    std::filesystem::path temp_dir;
    try {
        FastModuleContainer modules;
        temp_dir = makeTempProjectDir();
        writeEmbeddedRtShadowMaskTestProject(temp_dir);
        configureRayQueryTestRuntime(temp_dir);

        auto &renderer = GET_MODULE(Renderer);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        REQUIRE(vkcore.getRuntimeCapabilities().ray_query);
        REQUIRE(
            vkcore.getRuntimeCapabilities().ray_tracing_pipeline);
        auto &geometry = GET_MODULE(VertBufContainer);
        auto primitive = geometry.addPrimitiveEntry(
            makeScreenQuad(0.55F, 1.0F));
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        ModelTemplate model;
        model.asset_id = ModelAssetId{2852};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material =
                    GET_MODULE(StandardMaterialResource)
                        .standardTransparentMaterial(),
                .primitives = {primitive},
            },
        };
        GET_MODULE(PolygonInstanceContainer)
            .placeModelInstance(model);
        GET_MODULE(LightContainer).load({
            LightLoadEntry{
                .name = "WP285 embedded directional",
                .component = LightCodecData{
                    .type = LightCodecType::Directional,
                    .direction = {0.0f, 0.0f, -1.0f},
                    .intensity = 1.0f,
                    .color = {1.0f, 1.0f, 1.0f}},
            },
        });

        renderer.render();
        vkcore.waitIdle();
        const auto mask =
            RendererTestAccess::readR8RenderTarget(renderer,
                "rt_shadow_mask");
        const auto pipeline_mask =
            RendererTestAccess::readR8RenderTarget(renderer,
                "rt_shadow_mask_pipeline");
        REQUIRE(mask.extent == vk::Extent2D{32, 32});
        REQUIRE(mask.pixels.size() == 32 * 32);
        REQUIRE(pipeline_mask.extent == mask.extent);
        REQUIRE(pipeline_mask.pixels == mask.pixels);
        for (std::uint32_t y = 4; y <= 10; ++y) {
            for (std::uint32_t x = 4; x <= 10; ++x) {
                REQUIRE(r8PixelAt(mask, x, y) < 64);
            }
        }
        for (std::uint32_t y = 0; y < 32; ++y) {
            for (std::uint32_t x = 22; x < 32; ++x) {
                REQUIRE(r8PixelAt(mask, x, y) > 192);
            }
        }
        const auto diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        REQUIRE(diagnostics.requested);
        REQUIRE(diagnostics.tlas_build_count == 1);
        REQUIRE(diagnostics.tlas_instance_count == 1);
        const auto plan = renderer.currentFramePlanJson();
        bool acceleration_structure_registered = false;
        for (const auto &scope :
             plan.at("gpu_resource_arena").at("scopes")) {
            for (const auto &resource : scope.at("resources")) {
                acceleration_structure_registered =
                    acceleration_structure_registered ||
                    resource.at("kind") ==
                        "acceleration_structure";
            }
        }
        REQUIRE(acceleration_structure_registered);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan embedded ray-query shadow-mask rendering unavailable");
        throw;
    }
}

CommonPolygonVertData makeRt1aOracleTriangles() {
    CommonPolygonVertData data;
    data.indices = {0, 1, 2, 3, 4, 5};
    data.pos = {
        {-1.0f, -1.0f, 8.0f},
        {1.0f, -1.0f, 8.0f},
        {0.0f, 1.0f, 8.0f},
        {-1.0f, -1.0f, 16.0f},
        {1.0f, -1.0f, 16.0f},
        {0.0f, 1.0f, 16.0f},
    };
    data.normal.assign(6, glm::vec3{0.0f, 0.0f, 1.0f});
    data.texcoord.assign(6, glm::vec2{0.0f});
    data.color.assign(6, glm::vec4{1.0f});
    return data;
}

TEST_CASE(
    "RT1a production observers see omitted explicit recursion reload and all extent wires",
    "[headless][gpu][ray-tracing][rt1a][observer][reload]") {
    setupLogger();
    struct RunResult {
        RayTracingRuntimeFacts facts;
        std::vector<RayTracingPipelineCreateObservation> creates;
        std::vector<RayTracingValidationObservation> validations;
        std::size_t rebuilt_pipelines = 0;
    };
    const auto run = [](std::optional<std::uint32_t> max_recursion,
                        bool exercise_extent_wires,
                        bool exercise_reload) {
        RunResult result;
        const auto temp_dir = makeTempProjectDir();
        try {
            {
                FastModuleContainer modules;
                writeEmbeddedRtShadowMaskTestProject(
                    temp_dir, max_recursion, exercise_reload);
                configureRayQueryTestRuntime(temp_dir);
                auto &vkcore = GET_MODULE(VulkanManageCore);
                if (!vkcore.getRuntimeCapabilities()
                         .ray_tracing_pipeline) {
                    SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
                }
                result.facts = vkcore.getRuntimeCapabilities()
                                   .ray_tracing_facts;
                const ScopedRayTracingValidationObserver validation_observer{
                    [&](const auto &observation) {
                        result.validations.push_back(observation);
                    }};
                const ScopedRayTracingPipelineCreateObserver create_observer{
                    [&](const auto &observation) {
                        result.creates.push_back(observation);
                    }};

                auto &renderer = GET_MODULE(Renderer);
                if (exercise_extent_wires) {
                    GET_MODULE(ComputeTaskContainer)
                        .rebindRenderTargets(
                            GET_MODULE(RenderTargetContainer));
                    renderer.render();
                }
                if (exercise_reload) {
                    auto prepared =
                        GET_MODULE(ShaderLibrary).prepareReloadAll();
                    REQUIRE_FALSE(prepared.empty());
                    const auto rebuilt =
                        GET_MODULE(PipelineFactory)
                            .rebuildPrepared(std::move(prepared));
                    REQUIRE(rebuilt.committed);
                    REQUIRE(rebuilt.failed_pipelines == 0);
                    result.rebuilt_pipelines =
                        rebuilt.rebuilt_pipelines;
                }
                vkcore.waitIdle();
            }
            std::filesystem::remove_all(temp_dir);
            return result;
        } catch (const std::exception &error) {
            std::filesystem::remove_all(temp_dir);
            TestSupport::skipIfVulkanDeviceUnavailable(
                error, "Vulkan RT1a observer fixture unavailable");
            throw;
        }
    };

#if PELICAN_RUNTIME_SHADER_COMPILER
    constexpr std::string_view engine_digest =
        "rt1a-fnv1a64:d18212b9cb401962";
#else
    constexpr std::string_view engine_digest =
        "rt1a-fnv1a64:22899212d139d671";
#endif
    constexpr std::string_view reload_digest =
        "rt1a-fnv1a64:6cba33c01f3e8dae";
    const auto check_creates = [](
                                   const RunResult &result,
                                   std::span<const std::uint32_t> depths,
                                   std::string_view expected_digest) {
        const std::string expected_digest_string{expected_digest};
        REQUIRE(result.creates.size() == depths.size());
        for (std::size_t index = 0; index < depths.size(); ++index) {
            CHECK(result.creates[index]
                      .max_pipeline_ray_recursion_depth ==
                  depths[index]);
            CHECK(result.creates[index].validator_digest ==
                  expected_digest_string);
        }
    };

    const auto omitted = run(std::nullopt, true, false);
    const std::array omitted_depths{1u};
    check_creates(omitted, omitted_depths, engine_digest);
    const auto explicit_one = run(1, false, false);
    const std::array explicit_one_depths{1u};
    check_creates(
        explicit_one, explicit_one_depths, engine_digest);
    std::array<bool, 4> observed_sites{};
    for (const auto &observation : omitted.validations) {
        CHECK(observation.facts == omitted.facts);
        observed_sites[static_cast<std::size_t>(observation.site)] =
            true;
        if (observation.extent) {
            CHECK(*observation.extent ==
                  std::array<std::uint32_t, 3>{32, 32, 1});
        }
    }
    CHECK(observed_sites == std::array{true, true, true, true});

    if (omitted.facts.max_ray_recursion_depth < 2) {
        SKIP("RT1a explicit depth-2 reload requires maxRayRecursionDepth >= 2; actual=" +
             std::to_string(
                 omitted.facts.max_ray_recursion_depth));
    }
    const auto explicit_two = run(2, false, true);
    const std::array explicit_two_depths{2u, 2u};
    check_creates(
        explicit_two, explicit_two_depths, reload_digest);
    CHECK(explicit_two.rebuilt_pipelines == 1);
}

TEST_CASE(
    "RT1a policy negatives stop the real headless load before Vulkan create",
    "[headless][gpu][ray-tracing][rt1a][negative][load]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    struct Case {
        Rt1aNegativeLoadKind kind;
        std::string_view code;
    };
    for (const auto test_case : {
             Case{Rt1aNegativeLoadKind::payload_mismatch,
                  "pelican.ray_tracing.payload_mismatch@1"},
             Case{Rt1aNegativeLoadKind::forbidden_hit_attribute,
                  "pelican.ray_tracing.hit_attribute_forbidden@1"},
             Case{Rt1aNegativeLoadKind::dynamic_trace_operand,
                  "pelican.ray_tracing.trace_operand_dynamic@1"},
         }) {
        const auto temp_dir = makeTempProjectDir();
        try {
            FastModuleContainer modules;
            writeRt1aNegativeLoadProject(temp_dir, test_case.kind);
            configureRayQueryTestRuntime(temp_dir);
            auto &vkcore = GET_MODULE(VulkanManageCore);
            if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
                SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
            }

            std::size_t create_calls = 0;
            const ScopedRayTracingPipelineCreateObserver create_observer{
                [&](const auto &) { ++create_calls; }};
            std::string error_message;
            try {
                (void)GET_MODULE(Renderer);
            } catch (const std::exception &error) {
                error_message = error.what();
            }
            const std::string expected_code{test_case.code};
            CAPTURE(expected_code, error_message);
            CHECK(error_message.find(expected_code) != std::string::npos);
            CHECK(error_message.find("project://shaders/negative") !=
                  std::string::npos);
            CHECK(create_calls == 0);
            vkcore.waitIdle();
            std::filesystem::remove_all(temp_dir);
        } catch (const std::exception &error) {
            std::filesystem::remove_all(temp_dir);
            TestSupport::skipIfVulkanDeviceUnavailable(
                error, "Vulkan RT1a negative-load fixture unavailable");
            throw;
        }
    }
#else
    SUCCEED(
        "RT1a source negative loads require the runtime compiler; direct SPIR-V rejection is covered by the CPU load test");
#endif
}

TEST_CASE(
    "RT1a GPU sentinels match the CPU interval oracle and expose a neighboring hit group",
    "[headless][gpu][ray-tracing][rt1a][oracle][sentinel]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeRt1aOracleProject(temp_dir);
        configureRayQueryTestRuntime(temp_dir);

        auto &renderer = GET_MODULE(Renderer);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        const auto actual_depth =
            vkcore.getRuntimeCapabilities()
                .ray_tracing_facts.max_ray_recursion_depth;
        if (actual_depth < 2) {
            SKIP("RT1a recursion sentinel requires maxRayRecursionDepth >= 2; actual=" +
                 std::to_string(actual_depth));
        }

        auto primitive = GET_MODULE(VertBufContainer)
                             .addPrimitiveEntry(
                                 makeRt1aOracleTriangles());
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        ModelTemplate model;
        model.asset_id = ModelAssetId{3640};
        model.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material =
                    GET_MODULE(StandardMaterialResource)
                        .standardTransparentMaterial(),
                .primitives = {primitive},
            },
        };
        GET_MODULE(PolygonInstanceContainer)
            .placeModelInstance(model);

        renderer.render();
        vkcore.waitIdle();
        const auto primary =
            RendererTestAccess::readRenderTarget(renderer,
                "wp364_oracle", ImageSubresourceRange{});
        const auto shifted =
            RendererTestAccess::readRenderTarget(renderer,
                "wp364_oracle_shifted",
                ImageSubresourceRange{});
        REQUIRE(primary.format ==
                vk::Format::eR32G32B32A32Uint);
        REQUIRE(shifted.format == primary.format);
        REQUIRE(primary.extent.width >= 2);
        REQUIRE(primary.bytes.size() >= 2 * 4 * sizeof(std::uint32_t));

        const auto pixel = [](const RenderTargetReadback &readback,
                              std::uint32_t x) {
            std::array<std::uint32_t, 4> result{};
            const auto offset =
                static_cast<std::size_t>(x) * sizeof(result);
            if (offset + sizeof(result) > readback.bytes.size()) {
                throw std::out_of_range(
                    "RT1a oracle pixel is outside readback");
            }
            std::memcpy(result.data(),
                        readback.bytes.data() + offset,
                        sizeof(result));
            return result;
        };
        const auto gpu_hit = pixel(primary, 0);
        const auto gpu_miss = pixel(primary, 1);
        const auto gpu_shifted_hit = pixel(shifted, 0);

        auto cpu_fixture =
            makeConstructiveRayTracingOracleFixture(
                RayTracingSentinelKind::hit, 0x364u, 8.0f);
        auto second = cpu_fixture.triangles.front();
        for (auto &vertex : second.vertices) vertex.z = 16.0;
        cpu_fixture.triangles.push_back(second);
        const auto cpu_hit = traceRayTracingOracle(
            cpu_fixture.ray, cpu_fixture.triangles);
        REQUIRE(cpu_hit.hit.has_value());
        const auto gpu_t = std::bit_cast<float>(gpu_hit[1]);
        CHECK(gpu_hit[3] == 1u);
        CHECK(rayTracingOracleContainsGpuHit(
            *cpu_hit.hit, gpu_t, gpu_hit[0]));

        const auto miss_fixture =
            makeConstructiveRayTracingOracleFixture(
                RayTracingSentinelKind::miss, 0x777u, 8.0f);
        CHECK_FALSE(traceRayTracingOracle(
                        miss_fixture.ray,
                        miss_fixture.triangles)
                        .hit.has_value());
        CHECK(gpu_miss[0] == 0x777u);
        CHECK(gpu_miss[3] == 0u);

        // Offset 1 selects the neighboring, same-kind hit-group handle.
        // The geometry and t stay fixed, so only the record identity moves.
        CHECK(gpu_shifted_hit[0] == 0x365u);
        CHECK_FALSE(rayTracingOracleContainsGpuHit(
            *cpu_hit.hit,
            std::bit_cast<float>(gpu_shifted_hit[1]),
            gpu_shifted_hit[0]));

        const auto recursion =
            rayTracingRecursionSentinel(2, 2, 0);
        CHECK(gpu_hit[2] == recursion.executed_depth);
        CHECK(gpu_hit[2] == recursion.payload);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan RT1a oracle sentinel fixture unavailable");
        throw;
    }
#else
    SUCCEED(
        "RT1a GPU oracle shaders require the runtime compiler; compiler-OFF parent SPIR-V hashes are covered by the CPU gate");
#endif
}

TEST_CASE("common graph state survives staged ray pipeline publication", "[headless][gpu][graph-transfer-rt]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeRt1bLayoutProject(temp_dir);
        writeTextFile(temp_dir / "ray_bindings.json", R"({"schema":"pelican.ray_bindings","version":2,"bindings":[]})");
        const auto config_path = temp_dir / "hybrid.json";
        auto document = nlohmann::json::parse(R"({"schema":"pelican.graph","version":1,
          "nodes":[{"id":"time","op":"value.input","parameters":{"name":"time"},"outputs":["value"]},
            {"id":"commands","op":"event.playback","outputs":["events"]},
            {"id":"clock","op":"value.event_clock","inputs":{"time":{"node":"time","port":"value"},
              "events":{"node":"commands","port":"events"}},"outputs":["value"]},
            {"id":"clock_buffer","op":"render.buffer","parameters":{"element":"float"},
              "inputs":{"value":{"node":"clock","port":"value"}},"outputs":["resource"]}],
          "outputs":{"elapsed":{"node":"clock","port":"value"}}})");
        document["rendering"] =
            nlohmann::json::parse(readTextFile(config_path)).at("rendering");
        // Existing RT tasks remain a feature, while the common graph derives
        // its own CPU-to-GPU buffers from connections. MG-2b: the project was
        // written as a graph root, so its tasks are already in its task
        // feature; this test carries them in a feature of its own.
        auto &rendering = document["rendering"];
        const auto authored_tasks = nlohmann::json::parse(
            readTextFile(temp_dir / "features" / "authored_tasks.json"));
        const nlohmann::json rt_feature{{"schema", "pelican.render_feature"}, {"version", 1},
            {"name", "rt_probe"}, {"compute_tasks", authored_tasks.at("compute_tasks")}};
        writeTextFile(temp_dir / "rt_probe.json", rt_feature.dump());
        rendering["features"] = {"project://rt_probe.json"};
#if PELICAN_WITH_AUDIO
        const unsigned char wav[]{'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
            1,0,1,0,64,31,0,0,128,62,0,0,2,0,16,0,'d','a','t','a',8,0,0,0,0,0,0,0,0,0,0,0};
        writeTextFile(temp_dir / "tone.wav", std::string(reinterpret_cast<const char *>(wav), sizeof(wav)));
        document["nodes"].push_back({{"id", "voice"}, {"op", "audio.playback"},
            {"parameters", {{"asset", "project://tone.wav"}}},
            {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}},
                        {"events", {{"node", "commands"}, {"port", "events"}}}}}, {"outputs", {"voice"}}});
#endif
        writeTextFile(config_path, document.dump());
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) SKIP("ray tracing pipeline unavailable");
        writeTextFile(temp_dir / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
            {"name":"GraphOwner","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}
        ]}}})");
        GET_MODULE(ECSPredefinedRegistration).reg();
        auto &scene = GET_MODULE(SceneLoader);
        test_support::loadScene(scene, "default_scene");
        const auto owner = scene.objectId("GraphOwner");
        REQUIRE(owner);
        REQUIRE_FALSE(scene.runtimeObjectBindings().empty());
        document["definitions"]["owned_buffer"] = {{"nodes", nlohmann::json::array({
            {{"id", "gain"}, {"op", "graph.input"}, {"default", 7}},
            {{"id", "gpu"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}}},
                {"inputs", {{"value", {{"node", "gain"}, {"port", "value"}}}}}, {"outputs", {"resource"}}}
        })}, {"outputs", nlohmann::json::object()}};
        document["nodes"].push_back({{"id", "owned"}, {"op", "graph.instance"}, {"definition", "owned_buffer"},
            {"owner", {{"scene", "default_scene"}, {"object", scene.runtimeObjectBindings().front().authoring_object_id.value}}}});
        writeTextFile(config_path, document.dump());
        GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
        auto &renderer = GET_MODULE(Renderer);
        GET_MODULE(PolygonInstanceContainer).placeModelInstance(GET_MODULE(ModelAssetContainer).getModelTemplateByName("tree"));
        auto &time = GET_MODULE(EngineTime);
        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        time.setTime(10);
        renderer.render();
        vkcore.waitIdle();
        REQUIRE(runtime.snapshot());
        const auto graph = runtime.snapshot()->programs.begin()->second.frame_graph.value_graph;
        REQUIRE(graph);
        REQUIRE(graph->emitEvent(scene.graphEvaluationState(graph), "commands", Animation::CurvePlaybackCommand::start, 10));
        time.setTime(10.25);
        renderer.render();
        vkcore.waitIdle();
        const auto read_clock = [&] {
            const auto bytes = vkcore.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer("clock_buffer"), sizeof(float));
            float value{};
            std::memcpy(&value, bytes.data(), sizeof(value));
            return value;
        };
        REQUIRE(read_clock() == 0.25f);
#if PELICAN_WITH_AUDIO
        const auto voice = scene.graphSound(graph, "voice");
        REQUIRE(voice);
        REQUIRE(GameContext{}.audio().isPlaying(*voice));
#endif
        document["nodes"].push_back({{"id", "extra"}, {"op", "value.constant"},
            {"parameters", {{"value", 1}}}, {"outputs", {"value"}}});
        const auto apply = [&](bool fail) {
            const auto reference = GET_MODULE(ProjectBasicConfig).renderingConfigReference();
            const auto key = GET_MODULE(PathResolver).normalizedReference(reference);
            const auto bytes = document.dump();
            return renderer.applyRenderPipelineAuthoringCandidate(RenderConfigCandidateDocumentSet{key,
                {{.reference = reference, .normalized_reference = key, .path = config_path,
                  .operation = RenderConfigDocumentOperation::replace,
                  .expected = {RenderConfigDocumentExistence::present, renderConfigSourceDigest(readTextFile(config_path))},
                  .bytes = bytes}}}, [config_path, bytes, fail] {
                    if (fail) throw std::runtime_error("injected staged graph source failure");
                    writeTextFile(config_path, bytes);
                });
        };
        const auto generation = runtime.activeGeneration();
        const auto source = readTextFile(config_path);
        REQUIRE(apply(true).staged);
        // The event arrives after staging, before the candidate frame starts.
        REQUIRE(graph->emitEvent(scene.graphEvaluationState(graph), "commands", Animation::CurvePlaybackCommand::pause, 10.5));
        time.setTime(10.75);
        REQUIRE_THROWS_WITH(renderer.render(), Catch::Matchers::ContainsSubstring("injected staged graph source failure"));
        REQUIRE(runtime.activeGeneration() == generation);
        REQUIRE(runtime.stagedGeneration() == 0);
        REQUIRE(readTextFile(config_path) == source);
#if PELICAN_WITH_AUDIO
        REQUIRE(GameContext{}.audio().soundState(*voice));
        REQUIRE_FALSE(GameContext{}.audio().soundState(*voice)->paused);
#endif
        REQUIRE(apply(false).staged);
        renderer.render();
        vkcore.waitIdle();
        REQUIRE(runtime.activeGeneration() > generation);
        REQUIRE(read_clock() == 0.5f);
        const auto next = runtime.snapshot()->programs.begin()->second.frame_graph.value_graph;
        REQUIRE(next != graph);
#if PELICAN_WITH_AUDIO
        REQUIRE(scene.graphSound(next, "voice") == voice);
        REQUIRE(GameContext{}.audio().soundState(*voice));
        REQUIRE(GameContext{}.audio().soundState(*voice)->paused);
#endif
        REQUIRE(next->emitEvent(scene.graphEvaluationState(next), "commands", Animation::CurvePlaybackCommand::resume, 10.75));
        time.setTime(11);
        renderer.render();
        vkcore.waitIdle();
        REQUIRE(read_clock() == 0.75f);
        // Owner-only recompilation uses the same staged RT publication path.
        auto &buffers = GET_MODULE(FrameGraphResourceContainer);
        auto held = runtime.snapshot();
        const auto owned_buffer = buffers.getBufferIdByName("owned/gpu");
        REQUIRE(buffers.hasBuffer(owned_buffer));
        const auto before_owner_removal = readTextFile(config_path);
        const auto feature_path = temp_dir / "rt_probe.json";
        const auto published_feature = readTextFile(feature_path);
        writeTextFile(feature_path, "uncommitted invalid feature JSON");
        GameObjects{}.remove(*owner);
        const auto before_failure = runtime.activeGeneration();
        {
            const ScopedRayTracingJointTransactionInjector injector{
                [](RayTracingJointTransactionPoint point) {
                    if (point == RayTracingJointTransactionPoint::submit)
                        throw std::runtime_error("injected owner GPU publication failure");
                }};
            REQUIRE_THROWS_WITH(renderer.render(), Catch::Matchers::ContainsSubstring("injected owner GPU publication failure"));
        }
        REQUIRE(runtime.activeGeneration() == before_failure);
        REQUIRE(buffers.hasBuffer("owned/gpu"));
        REQUIRE(readTextFile(config_path) == before_owner_removal);
        REQUIRE(readTextFile(feature_path) == "uncommitted invalid feature JSON");
        writeTextFile(feature_path, published_feature);
        // Explicit authored retry is available after a failed automatic attempt.
        REQUIRE(apply(false).staged);
        renderer.render();
        vkcore.waitIdle();
        REQUIRE_FALSE(buffers.hasBuffer("owned/gpu"));
        REQUIRE(buffers.hasBuffer(owned_buffer));
        REQUIRE(read_clock() == 0.75f);
#if PELICAN_WITH_AUDIO
        REQUIRE(scene.graphSound(runtime.snapshot()->programs.begin()->second.frame_graph.value_graph, "voice") == voice);
#endif
        held.reset();
        for (int frame = 0; frame < 3; ++frame) renderer.render();
        vkcore.waitIdle();
        GET_MODULE(DeletionQueue).flushAll();
        REQUIRE_FALSE(buffers.hasBuffer(owned_buffer));
        REQUIRE(GET_MODULE(DeletionQueue).pendingCountForTesting() == 0);
        const auto owner_free_generation = runtime.activeGeneration();
        renderer.render();
        REQUIRE(runtime.activeGeneration() == owner_free_generation);
        test_support::loadScene(scene, "default_scene");
        GET_MODULE(PolygonInstanceContainer).placeModelInstance(GET_MODULE(ModelAssetContainer).getModelTemplateByName("tree"));
        renderer.render();
        vkcore.waitIdle();
        REQUIRE(buffers.hasBuffer("owned/gpu"));
        const auto restored = vkcore.readBuf(buffers.buffer("owned/gpu"), sizeof(float));
        float restored_value{};
        std::memcpy(&restored_value, restored.data(), sizeof(float));
        REQUIRE(restored_value == 7);
        REQUIRE(readTextFile(config_path) == before_owner_removal);
        // MG-2b: "no graph" is a graph root with no nodes, not the older
        // root.
        document = Pelican::TestSupport::renderGraphRoot(document.at("rendering"));
        REQUIRE(apply(false).staged);
        renderer.render();
        vkcore.waitIdle();
        // The graph root always carries a value graph now; without nodes it
        // drives nothing.
        const auto &emptied_graph =
            runtime.snapshot()->programs.begin()->second.frame_graph.value_graph;
        REQUIRE((!emptied_graph || emptied_graph->nodeIds().empty()));
#if PELICAN_WITH_AUDIO
        REQUIRE_FALSE(GameContext{}.audio().soundState(*voice));
#endif
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }
    std::filesystem::remove_all(temp_dir);
#else
    SKIP("runtime shader compiler unavailable");
#endif
}

TEST_CASE("a frame skipped for want of an output image keeps the staged ray-tracing startup generation",
          "[headless][gpu][ray-tracing][staged-startup]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        RuntimeTeardownGuard teardown{
            RuntimeTeardownMode::terminal_shutdown};
        writeRt1bLayoutProject(temp_dir);
        writeTextFile(temp_dir / "ray_bindings.json",
                      R"({"schema":"pelican.ray_bindings","version":2,"bindings":[]})");
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        auto &renderer = GET_MODULE(Renderer);
        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        GET_MODULE(PolygonInstanceContainer).placeModelInstance(
            GET_MODULE(ModelAssetContainer).getModelTemplateByName("tree"));

        // A ray-tracing startup generation is staged, not published, until
        // its first submitted frame.
        REQUIRE(runtime.snapshot() == nullptr);
        const auto staged = runtime.stagedGeneration();
        REQUIRE(staged != 0);
        REQUIRE(renderer.publishedLogicalFrameCount() == 0);

        // The first frames arrive before the output has an image, as they do
        // for a player embedded in Studio with a warm shader cache.
        test_support::RenderTargetTestAccess::reportUnavailableFrames(
            GET_MODULE(RenderTarget), 2);
        renderer.render();
        renderer.render();
        CHECK(runtime.stagedGeneration() == staged);
        CHECK(runtime.snapshot() == nullptr);
        CHECK(renderer.publishedLogicalFrameCount() == 0);

        // The first frame with an image publishes that same generation.
        renderer.render();
        vkcore.waitIdle();
        CHECK(runtime.stagedGeneration() == 0);
        CHECK(runtime.activeGeneration() == staged);
        CHECK(renderer.publishedLogicalFrameCount() == 1);
        renderer.render();
        vkcore.waitIdle();
        CHECK(renderer.publishedLogicalFrameCount() == 2);
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }
    std::filesystem::remove_all(temp_dir);
#else
    SKIP("runtime shader compiler unavailable");
#endif
}

TEST_CASE(
    "WP366 renderer A B recovers q r group miss and WP384 preserves frame publication",
    "[headless][gpu][ray-tracing][rt1b][renderer][sentinel][transaction]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        // Retired descriptor sets must drain while their container-owned
        // pools are alive, including when an assertion unwinds this fixture.
        RuntimeTeardownGuard teardown{
            RuntimeTeardownMode::terminal_shutdown};
        writeRt1bLayoutProject(temp_dir);
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }

        // Both material-reference branches come from production assets: a
        // whole-model primitive binding selects one project material while a
        // second primitive retains its embedded glTF material identity.
        auto &model_assets = GET_MODULE(ModelAssetContainer);
        auto &material_assets =
            GET_MODULE(ProjectMaterialAssetContainer);
        auto &materials = GET_MODULE(MaterialContainer);
        auto &reload_service =
            GET_MODULE(watch::ReloadService);
        auto &deletion_queue = GET_MODULE(DeletionQueue);
        const auto project_material =
            material_assets.materialByName("ProjectLeaf");
        const auto embedded_materials = [&]() {
            const auto &model =
                model_assets.getModelTemplateByName("tree");
            std::vector<GlobalMaterialId> result;
            for (const auto &named : model.named_materials) {
                if ((named.name == "MatA" || named.name == "MatB") &&
                    named.material != project_material &&
                    materials.materialDocumentReference(
                        named.material).empty()) {
                    result.push_back(named.material);
                }
            }
            if (result.empty()) {
                throw std::runtime_error(
                    "WP366 production glTF lost embedded materials");
            }
            std::ranges::sort(
                result, {}, &GlobalMaterialId::value);
            return result;
        };
        {
            const auto &loaded =
                model_assets.getModelTemplateByName("tree");
            CHECK(loaded.authored_asset_reference ==
                  "models/tree.glb");
            REQUIRE(loaded.material_primitives.size() == 2);
            const auto project_group = std::find_if(
                loaded.material_primitives.begin(),
                loaded.material_primitives.end(),
                [project_material](const auto &group) {
                    return group.material == project_material;
                });
            REQUIRE(project_group !=
                    loaded.material_primitives.end());
            CHECK(project_group->source_material_index == 0);
            CHECK(materials.materialDocumentReference(
                      project_group->material) ==
                  "materials/project.material.json");
            const auto embedded_group = std::find_if(
                loaded.material_primitives.begin(),
                loaded.material_primitives.end(),
                [project_material](const auto &group) {
                    return group.material != project_material &&
                           group.source_material_index == 1;
                });
            REQUIRE(embedded_group !=
                    loaded.material_primitives.end());
            CHECK(materials.materialDocumentReference(
                      embedded_group->material)
                      .empty());

            auto &production_instances =
                GET_MODULE(PolygonInstanceContainer);
            const auto production_instance =
                production_instances.placeModelInstance(loaded);
            const auto snapshots =
                production_instances.rayQueryGeometryInstances();
            REQUIRE(snapshots.size() == 2);
            CHECK(std::ranges::any_of(
                snapshots, [](const auto &snapshot) {
                    return snapshot.material_document_reference ==
                           "materials/project.material.json";
                }));
            CHECK(std::ranges::any_of(
                snapshots, [](const auto &snapshot) {
                    return snapshot.material_document_reference.empty() &&
                           snapshot.material_asset_reference ==
                               "models/tree.glb" &&
                           snapshot.source_material_index == 1;
                }));

            RayTracingProjectConfig production_project{
                .layout = parseRayGroupLayout(
                    readTextFile(temp_dir / "ray_layout.json"),
                    "ray_layout.json"),
            };
            production_project.bindings = parseRayBindings(
                R"json({"schema":"pelican.ray_bindings","version":2,"bindings":[{"material":{"document":"materials/project.material.json"},"group":"opaque","record":{"tint":[1.0,0.0,1.0,1.0],"flags":303}},{"material":{"asset":"models/tree.glb","source_material_index":1},"group":"foliage","record":{"tint":[0.0,1.0,1.0,1.0],"flags":404}}]})json",
                production_project.layout,
                "production-loader-bindings.json");
            const auto production_rows =
                resolveRayTracingSceneRows(
                    snapshots, production_project);
            REQUIRE(production_rows.rows.size() == 2);
            std::array<std::uint32_t, 2> production_flags{};
            for (std::size_t index = 0;
                 index < production_rows.rows.size(); ++index) {
                REQUIRE(production_rows.rows[index]
                            .record_data.size() >= 20);
                std::memcpy(
                    &production_flags[index],
                    production_rows.rows[index]
                        .record_data.data() + 16,
                    sizeof(production_flags[index]));
            }
            std::ranges::sort(production_flags);
            CHECK(production_flags ==
                  std::array<std::uint32_t, 2>{303, 404});
            REQUIRE(production_instances.removeModelInstance(
                production_instance));
        }

        const auto model_path =
            temp_dir / "models" / "tree.glb";
        const auto first_embedded = embedded_materials();
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                watch::makeAssetKey("models/tree.glb"),
                watch::ReloadKind::modified, {}, 1}));
        const auto second_embedded = embedded_materials();
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                watch::makeAssetKey("models/tree.glb"),
                watch::ReloadKind::modified, {}, 2}));
        const auto third_embedded = embedded_materials();
        const auto disjoint = [](const auto &left,
                                 const auto &right) {
            return std::ranges::none_of(
                left, [&](const auto value) {
                    return std::ranges::find(right, value) !=
                           right.end();
                });
        };
        CHECK(disjoint(first_embedded, second_embedded));
        CHECK(disjoint(first_embedded, third_embedded));
        CHECK(disjoint(second_embedded, third_embedded));
        REQUIRE(deletion_queue.pendingCountForTesting() > 0);
        vkcore.waitIdle();
        deletion_queue.flushAll();
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                watch::makeAssetKey("models/tree.glb"),
                watch::ReloadKind::modified, {}, 3}));
        const auto reused_embedded = embedded_materials();
        CHECK(std::ranges::any_of(
            reused_embedded, [&](const auto value) {
                return std::ranges::find(
                           first_embedded, value) !=
                           first_embedded.end() ||
                       std::ranges::find(
                           second_embedded, value) !=
                           second_embedded.end() ||
                       std::ranges::find(
                           third_embedded, value) !=
                           third_embedded.end();
            }));
        CHECK(material_assets.materialByName("ProjectLeaf") ==
              project_material);

        TestGltfFragmentFixture::writeGlb(
            model_path, false, false, true);
        const auto stable_model_revision =
            model_assets.contentRevisionForTesting("tree");
        CHECK_FALSE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                watch::makeAssetKey("models/tree.glb"),
                watch::ReloadKind::modified, {}, 4}));
        CHECK(model_assets.contentRevisionForTesting("tree") ==
              stable_model_revision);
        TestGltfFragmentFixture::writeGlb(model_path);
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                watch::makeAssetKey("models/tree.glb"),
                watch::ReloadKind::modified, {}, 5}));

        std::vector<RayTracingSbtWriteObservation> writes;
        const ScopedRayTracingSbtWriteObserver observer{
            [&](const auto &write) { writes.push_back(write); }};
        auto &renderer = GET_MODULE(Renderer);
        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        REQUIRE(runtime.snapshot() == nullptr);
        const auto private_generation = runtime.stagedGeneration();
        REQUIRE(private_generation != 0);
        CHECK_THROWS_WITH(
            renderer.currentFramePlanJson(),
            Catch::Matchers::ContainsSubstring(
                "Current frame plan is not registered"));
        CHECK_THROWS_WITH(
            RendererTestAccess::currentFramePlanOrder(renderer),
            Catch::Matchers::ContainsSubstring(
                "Current frame plan is not registered"));

        CommonPolygonVertData geometry;
        geometry.indices = {0, 1, 2};
        geometry.pos = {{-0.75f, -0.75f, 8.0f},
                        {0.75f, -0.75f, 8.0f},
                        {0.0f, 0.75f, 8.0f}};
        geometry.normal.assign(3, glm::vec3{0.0f, 0.0f, 1.0f});
        geometry.texcoord.assign(3, glm::vec2{0.0f});
        geometry.color.assign(3, glm::vec4{1.0f});
        const auto primitive =
            GET_MODULE(VertBufContainer).addPrimitiveEntry(
                std::move(geometry));
        const auto material =
            GET_MODULE(StandardMaterialResource)
                .standardTransparentMaterial();
        ModelTemplate model_a;
        model_a.asset_id = ModelAssetId{3660};
        model_a.authored_asset_reference = "models/tree.glb";
        model_a.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = material,
                .primitives = {primitive},
                .source_material_index = 2,
            }};
        ModelTemplate model_b;
        model_b.asset_id = ModelAssetId{3661};
        model_b.authored_asset_reference = "models/tree.glb";
        model_b.material_primitives = {
            ModelTemplate::MaterialPrimitives{
                .material = material,
                .primitives = {primitive},
                .source_material_index = 3,
            }};
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto a = instances.placeModelInstance(model_a);
        const auto b = instances.placeModelInstance(model_b);
        instances.setTrs(a, {-2.0f, 0.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                         glm::vec3{1.0f});
        instances.setTrs(b, {2.0f, 0.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                         glm::vec3{1.0f});

        renderer.render();
        vkcore.waitIdle();
        REQUIRE(runtime.stagedGeneration() == 0);
        REQUIRE(runtime.activeGeneration() == private_generation);

        const auto output_a = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        const auto output_b = RendererTestAccess::readRenderTarget(renderer,
            "wp366_b", ImageSubresourceRange{});
        REQUIRE(output_a.format == vk::Format::eR32G32B32A32Uint);
        REQUIRE(output_b.format == output_a.format);
        const auto pixel = [](const RenderTargetReadback &readback,
                              std::uint32_t x) {
            std::array<std::uint32_t, 4> result{};
            const auto offset = static_cast<std::size_t>(x) * sizeof(result);
            if (offset + sizeof(result) > readback.bytes.size()) {
                throw std::out_of_range("WP366 sentinel pixel is outside readback");
            }
            std::memcpy(result.data(), readback.bytes.data() + offset,
                        sizeof(result));
            return result;
        };
        // MG-2a: the second channel is the instance custom index. Layout v1
        // wrote each row's q there; a version 3 row takes the layout default
        // (0), so q is read from the SBT the rows were written to below.
        constexpr auto custom_index =
            rayAccelerationStructureCanonicalDefaults.custom_index;
        CHECK(pixel(output_a, 0) ==
              std::array{0xa0u, custom_index, 101u, std::bit_cast<std::uint32_t>(1.0f)});
        CHECK(pixel(output_a, 1) ==
              std::array{0xb0u, custom_index, 202u, std::bit_cast<std::uint32_t>(0.0f)});
        CHECK(pixel(output_a, 2) ==
              std::array{1280u, 0xffffffffu, 0u, 0u});
        CHECK(pixel(output_b, 0) ==
              std::array{0xa1u, custom_index, 101u, std::bit_cast<std::uint32_t>(1.0f)});
        CHECK(pixel(output_b, 1) ==
              std::array{0xb1u, custom_index, 202u, std::bit_cast<std::uint32_t>(0.0f)});
        CHECK(pixel(output_b, 2) ==
              std::array{1281u, 0xffffffffu, 1u, 0u});

        REQUIRE(writes.size() == 2);
        CHECK(writes[0].row_digest_sha256 == writes[1].row_digest_sha256);
        CHECK(writes[0].pipeline_handle != writes[1].pipeline_handle);
        const auto &capabilities = vkcore.getRuntimeCapabilities();
        const auto alignment = capabilities.shader_group_handle_alignment;
        REQUIRE(alignment != 0);
        const auto data_start =
            (capabilities.shader_group_handle_size + alignment - 1u) /
            alignment * alignment;
        // The row pixel 0 hit (flags 101) is the q whose records carry 101.
        const auto q_a = [&] {
            std::uint32_t flags = 0;
            std::memcpy(&flags,
                        writes[0].hit_bytes.data() + data_start +
                            sizeof(std::array<float, 4>),
                        sizeof(flags));
            return flags == 101u ? 0u : 1u;
        }();
        for (const auto &write : writes) {
            REQUIRE(write.record_index_stride == 2);
            REQUIRE(write.record_count == 4);
            REQUIRE(write.hit_bytes.size() ==
                    static_cast<std::size_t>(write.record_byte_stride) * 4u);
            for (std::uint32_t q = 0; q < 2; ++q) {
                for (std::uint32_t r = 0; r < 2; ++r) {
                    const auto at =
                        static_cast<std::size_t>(q * 2u + r) *
                            write.record_byte_stride +
                        data_start;
                    std::array<float, 4> tint{};
                    std::uint32_t flags = 0;
                    std::memcpy(tint.data(), write.hit_bytes.data() + at,
                                sizeof(tint));
                    std::memcpy(&flags,
                                write.hit_bytes.data() + at + sizeof(tint),
                                sizeof(flags));
                    CHECK(flags == (q == q_a ? 101u : 202u));
                    CHECK(tint[0] == (q == q_a ? 1.0f : 0.0f));
                    CHECK(tint[1] == (q == q_a ? 0.0f : 1.0f));
                }
            }
        }

        // Authoring a layout project is staged: no Renderer-side metadata or
        // source bytes become visible until the submitted frame publishes
        // the runtime generation, and the delayed hooks own every value they
        // use after this call returns.
        const auto root_reference =
            GET_MODULE(ProjectBasicConfig)
                .renderingConfigReference();
        const auto config_path = temp_dir / "hybrid.json";
        const auto old_config = readTextFile(config_path);
        const auto authored_config = old_config + "\n";
        const auto root_key =
            GET_MODULE(PathResolver)
                .normalizedReference(root_reference);
        const auto before_authoring_status =
            reload_service.statusJson()
                .at("runtime")
                .at(std::string{
                    watch::renderPipelineReloadParticipantName})
                .at("details");
        const auto before_authoring_plan =
            renderer.currentFramePlanJson();
        auto source_commit_count =
            std::make_shared<std::size_t>(0);
        auto authoring_completion = std::make_shared<
            std::optional<
                RenderPipelineAuthoringApplyResult>>();
        writes.clear();
        const auto authored =
            renderer.applyRenderPipelineAuthoringCandidate(
                RenderConfigCandidateDocumentSet{
                    root_key,
                    {{.reference = root_reference,
                      .normalized_reference = root_key,
                      .path = config_path,
                      .operation =
                          RenderConfigDocumentOperation::replace,
                      .expected =
                          {.existence =
                               RenderConfigDocumentExistence::present,
                           .digest =
                               renderConfigSourceDigest(old_config)},
                      .bytes = authored_config}}},
                [config_path, authored_config,
                 source_commit_count] {
                    writeTextFile(config_path,
                                  authored_config);
                    ++*source_commit_count;
                },
                [authoring_completion](
                    RenderPipelineAuthoringApplyResult completed) {
                    *authoring_completion =
                        std::move(completed);
                });
        CHECK_FALSE(authored.committed);
        REQUIRE(authored.staged);
        REQUIRE(authored.staged_generation != 0);
        CHECK_FALSE(authoring_completion->has_value());
        CHECK(*source_commit_count == 0);
        CHECK(readTextFile(config_path) == old_config);
        CHECK(GET_MODULE(ProjectBasicConfig)
                  .renderingConfigJson() == old_config);
        CHECK(renderer.currentFramePlanJson() ==
              before_authoring_plan);
        const auto staged_authoring_status =
            reload_service.statusJson()
                .at("runtime")
                .at(std::string{
                    watch::renderPipelineReloadParticipantName})
                .at("details");
        CHECK(staged_authoring_status.at("applied") ==
              before_authoring_status.at("applied"));
        CHECK(staged_authoring_status.at("generation") ==
              before_authoring_status.at("generation"));
        CHECK(staged_authoring_status.at("watched_sources") ==
              before_authoring_status.at("watched_sources"));

        renderer.render();
        vkcore.waitIdle();
        REQUIRE(authoring_completion->has_value());
        CHECK(authoring_completion->value().committed);
        CHECK_FALSE(authoring_completion->value().staged);
        CHECK(authoring_completion->value().published_generation ==
              authored.staged_generation);
        CHECK(*source_commit_count == 1);
        CHECK(readTextFile(config_path) == authored_config);
        CHECK(GET_MODULE(ProjectBasicConfig)
                  .renderingConfigJson() == authored_config);
        CHECK(runtime.activeGeneration() ==
              authored.staged_generation);
        REQUIRE(writes.size() == 2);
        // The submit above publishes the new generation after executing the
        // old one. Render once with the newly published target ownership
        // before reading it back.
        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_a", ImageSubresourceRange{}).bytes ==
              output_a.bytes);
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_b", ImageSubresourceRange{}).bytes ==
              output_b.bytes);
        REQUIRE(writes.empty());

        // Re-present the same authored scene in reverse publication order.
        // Its canonical row digest is unchanged, so the published TLAS/SBT
        // bundle is reused and the renderer output remains byte-identical.
        // Replace the production TLAS base q*R with the tempting wrong q.
        // One geometry is enough to hit neighboring records, so both actual
        // pipelines must expose the addressing error in their GPU payloads.
        writes.clear();
        {
            const ScopedRayTracingTlasRecordOffsetOverrideForTesting
                wrong_tlas_base{[](
                                    std::uint32_t q,
                                    std::uint32_t) { return q; }};
            renderer.render();
            vkcore.waitIdle();
        }
        const auto wrong_tlas_output_a =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto wrong_tlas_output_b =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_b", ImageSubresourceRange{});
        // The bad base affects q=1, whichever authored object owns it.
        const auto q_one_pixel = q_a == 1u ? 0u : 1u;
        const auto q_zero_pixel = 1u - q_one_pixel;
        auto wrong_a_expected = pixel(output_b, q_zero_pixel);
        wrong_a_expected[1] = custom_index; // from the hit instance
        CHECK(pixel(wrong_tlas_output_a, q_one_pixel) == wrong_a_expected);
        CHECK(pixel(wrong_tlas_output_b, q_one_pixel) ==
              pixel(output_a, q_one_pixel));
        CHECK(wrong_tlas_output_a.bytes != output_a.bytes);
        CHECK(wrong_tlas_output_b.bytes != output_b.bytes);

        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_a", ImageSubresourceRange{}).bytes ==
              output_a.bytes);
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_b", ImageSubresourceRange{}).bytes ==
              output_b.bytes);

        REQUIRE(instances.removeModelInstance(a));
        REQUIRE(instances.removeModelInstance(b));
        const auto reverse_b = instances.placeModelInstance(model_b);
        const auto reverse_a = instances.placeModelInstance(model_a);
        instances.setTrs(reverse_b, {2.0f, 0.0f, 0.0f},
                         glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});
        instances.setTrs(reverse_a, {-2.0f, 0.0f, 0.0f},
                         glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});
        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        const auto reverse_output_a = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        const auto reverse_output_b = RendererTestAccess::readRenderTarget(renderer,
            "wp366_b", ImageSubresourceRange{});
        CHECK(reverse_output_a.bytes == output_a.bytes);
        CHECK(reverse_output_b.bytes == output_b.bytes);
        REQUIRE(writes.empty());

        // Removing an authored row and its selector must shrink Q*R in the
        // same staged publication.  A stale foliage row would keep the
        // right-hand ray live or retain four SBT records, both of which are
        // observed through production GPU state.
        REQUIRE(instances.removeModelInstance(reverse_b));
        const auto bindings_path = temp_dir / "ray_bindings.json";
        const auto complete_bindings = readTextFile(bindings_path);
        auto reduced_bindings = nlohmann::json::parse(complete_bindings);
        auto &binding_entries = reduced_bindings.at("bindings");
        REQUIRE(binding_entries.size() == 2);
        binding_entries.erase(binding_entries.begin() + 1);
        writeTextFile(bindings_path, reduced_bindings.dump(2));
        REQUIRE(GET_MODULE(watch::ReloadService)
                    .applyRequestForTesting(
                        watch::ReloadRequest{
                            watch::makeAssetKey("ray_bindings.json"),
                            watch::ReloadKind::modified, {}, 1}));
        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        REQUIRE(writes.size() == 2);
        const auto removed_writes = writes;
        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        const auto removed_output_a =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto removed_output_b =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_b", ImageSubresourceRange{});
        CHECK(pixel(removed_output_a, 0) ==
              std::array{0xa0u, 0u, 101u,
                         std::bit_cast<std::uint32_t>(1.0f)});
        CHECK(pixel(removed_output_a, 1) ==
              std::array{1280u, 0xffffffffu, 0u, 0u});
        CHECK(pixel(removed_output_b, 0) ==
              std::array{0xa1u, 0u, 101u,
                         std::bit_cast<std::uint32_t>(1.0f)});
        CHECK(pixel(removed_output_b, 1) ==
              std::array{1281u, 0xffffffffu, 1u, 0u});
        REQUIRE(writes.empty());
        for (const auto &write : removed_writes) {
            CHECK(write.record_count == 2);
            CHECK(write.hit_bytes.size() ==
                  static_cast<std::size_t>(
                      write.record_byte_stride) * 2u);
        }

        const auto restored_b =
            instances.placeModelInstance(model_b);
        instances.setTrs(restored_b, {2.0f, 0.0f, 0.0f},
                         glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                         glm::vec3{1.0f});
        writeTextFile(bindings_path, complete_bindings);
        REQUIRE(GET_MODULE(watch::ReloadService)
                    .applyRequestForTesting(
                        watch::ReloadRequest{
                            watch::makeAssetKey("ray_bindings.json"),
                            watch::ReloadKind::modified, {}, 2}));
        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        REQUIRE(writes.size() == 2);
        const auto restored_writes = writes;
        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_a", ImageSubresourceRange{}).bytes ==
              output_a.bytes);
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_b", ImageSubresourceRange{}).bytes ==
              output_b.bytes);
        REQUIRE(writes.empty());
        for (const auto &write : restored_writes) {
            CHECK(write.record_count == 4);
        }

        // Group order and ray-type order each alter the generated virtual
        // include salt without changing what the shaders write (MG-2a:
        // whitespace no longer does -- only v1 hashed raw bytes). Observe the production disk keys plus request/miss
        // counts through ReloadService, then remove the ComputeTask hookup in
        // the same path and require that the authored include no longer loads.
        const auto layout_path = temp_dir / "ray_layout.json";
        const auto cache_keys = [&] {
            std::set<std::string> result;
            const auto directory =
                temp_dir / ".pelican" / "shader_cache";
            std::error_code error;
            if (!std::filesystem::is_directory(directory, error) ||
                error) {
                return result;
            }
            for (const auto &entry :
                 std::filesystem::directory_iterator(directory)) {
                if (entry.is_regular_file() &&
                    entry.path().extension() == ".spv-cache") {
                    result.insert(entry.path().stem().string());
                }
            }
            return result;
        };
        const auto apply_layout_change =
            [&](const std::string &bytes,
                std::uint64_t sequence) {
                const auto before_root = runtime.snapshot();
                REQUIRE(before_root != nullptr);
                const auto before_metrics =
                    GET_MODULE(StartupMetrics).snapshot();
                const auto before_keys = cache_keys();
                writeTextFile(layout_path, bytes);
                writes.clear();
                REQUIRE(reload_service.applyRequestForTesting(
                    watch::ReloadRequest{
                        watch::makeAssetKey("ray_layout.json"),
                        watch::ReloadKind::modified, {}, sequence}));
                CHECK(runtime.snapshot() == before_root);
                REQUIRE(runtime.stagedGeneration() != 0);
                renderer.render();
                vkcore.waitIdle();
                REQUIRE(runtime.stagedGeneration() == 0);
                REQUIRE(runtime.activeGeneration() ==
                        before_root->generation + 1);
                REQUIRE(writes.size() == 2);
                const auto generation_writes = writes;
                writes.clear();
                renderer.render();
                vkcore.waitIdle();
                const auto after_metrics =
                    GET_MODULE(StartupMetrics).snapshot();
                const auto requests =
                    after_metrics.shader_cache_requests -
                    before_metrics.shader_cache_requests;
                const auto hits =
                    after_metrics.shader_cache_hits -
                    before_metrics.shader_cache_hits;
                CHECK(requests > 0);
                CHECK(requests > hits);
                const auto after_keys = cache_keys();
                CHECK(std::ranges::any_of(
                    after_keys, [&](const auto &key) {
                        return !before_keys.contains(key);
                    }));
                CHECK(RendererTestAccess::readRenderTarget(renderer,
                          "wp366_a", ImageSubresourceRange{}).bytes ==
                      output_a.bytes);
                CHECK(RendererTestAccess::readRenderTarget(renderer,
                          "wp366_b", ImageSubresourceRange{}).bytes ==
                      output_b.bytes);
                REQUIRE(writes.empty());
                for (const auto &write : generation_writes) {
                    CHECK(write.record_count == 4);
                }
            };

        auto reordered_layout = nlohmann::json::parse(
            readTextFile(layout_path));
        std::swap(reordered_layout["groups"][0],
                  reordered_layout["groups"][1]);
        auto layout_bytes = reordered_layout.dump(2);
        apply_layout_change(layout_bytes, 1);
        const auto reordered_keys = cache_keys();

        std::swap(reordered_layout["ray_types"][0],
                  reordered_layout["ray_types"][1]);
        layout_bytes = reordered_layout.dump(2);
        apply_layout_change(layout_bytes, 2);
        const auto ray_type_order_keys = cache_keys();
        CHECK(ray_type_order_keys != reordered_keys);

        const auto disabled_layout = layout_bytes + "\n\t";
        writeTextFile(layout_path, disabled_layout);
        {
            const ScopedRayGroupLayoutVirtualIncludeHookupOverrideForTesting
                disabled_hookup{false};
            CHECK_FALSE(reload_service.applyRequestForTesting(
                watch::ReloadRequest{
                    watch::makeAssetKey("ray_layout.json"),
                    watch::ReloadKind::modified, {}, 3}));
        }
        CHECK(runtime.stagedGeneration() == 0);
        writeTextFile(layout_path, layout_bytes);

        // The companion span handed to Renderer originates in a local vector
        // inside ReloadService. Force allocator reuse before submit and
        // observe the exact shader key at the delayed production hook.
        const auto shader_path =
            temp_dir / "shaders" / "wp366_a.rgen";
        auto shader_bytes = readTextFile(shader_path);
        shader_bytes += "\n";
        writeTextFile(shader_path, shader_bytes);
        layout_bytes += "\n";
        writeTextFile(layout_path, layout_bytes);
        std::vector<watch::AssetKey> observed_companions;
        {
            const watch::ScopedRenderPipelineCompanionReloadObserver
                companion_observer{
                    [&](std::span<const watch::ReloadRequest> requests) {
                        for (const auto &request : requests) {
                            observed_companions.push_back(request.key);
                        }
                    }};
            const std::array requests{
                watch::ReloadRequest{
                    watch::makeAssetKey("ray_layout.json"),
                    watch::ReloadKind::modified, {}, 4},
                watch::ReloadRequest{
                    watch::makeAssetKey(
                        "shaders/wp366_a.rgen"),
                    watch::ReloadKind::modified, {}, 4},
            };
            const auto accepted =
                reload_service.applyRequestsForTesting(requests);
            REQUIRE((accepted ==
                     std::vector<bool>{true, true}));
            REQUIRE(runtime.stagedGeneration() != 0);
            for (std::size_t index = 0; index < 4096;
                 ++index) {
                std::vector<watch::ReloadRequest> overwrite(
                    1,
                    watch::ReloadRequest{
                        watch::makeAssetKey(
                            "poison/" + std::to_string(index)),
                        watch::ReloadKind::modified, {}, index});
                CHECK(overwrite.size() == 1);
            }
            renderer.render();
            vkcore.waitIdle();
        }
        REQUIRE(observed_companions.size() == 1);
        CHECK(observed_companions[0] ==
              watch::makeAssetKey("shaders/wp366_a.rgen"));
        renderer.render();
        vkcore.waitIdle();
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_a", ImageSubresourceRange{}).bytes ==
              output_a.bytes);
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_b", ImageSubresourceRange{}).bytes ==
              output_b.bytes);

        const auto stable_root = runtime.snapshot();
        REQUIRE(stable_root != nullptr);
        const auto stable_scope = std::find_if(
            stable_root->gpu_arena->scopes.begin(),
            stable_root->gpu_arena->scopes.end(),
            [](const auto &scope) {
                return scope.ray_tracing_project != nullptr;
            });
        REQUIRE(stable_scope != stable_root->gpu_arena->scopes.end());
        REQUIRE(stable_scope->acceleration_structures != nullptr);
        const auto stable_tlas =
            stable_scope->acceleration_structures
                ->topLevelDeviceAddress();
        REQUIRE(stable_tlas != 0);
        auto &frame_resources = GET_MODULE(FrameResources);
        const auto stable_sbt =
            frame_resources
                .rayTracingSceneShaderBindingTablesLeaseForTesting();
        const auto stable_descriptor_bundle =
            frame_resources.activeRtDescriptorBundleForTesting();
        REQUIRE(stable_sbt != nullptr);
        REQUIRE(stable_descriptor_bundle != nullptr);
        REQUIRE(stable_descriptor_bundle->scene_tables == stable_sbt);
        REQUIRE(stable_descriptor_bundle->top_level ==
                stable_scope->acceleration_structures->topLevel());
        const auto stable_diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        REQUIRE(stable_diagnostics.descriptor_bundle_generation != 0);
        // WP384 decision 11 seals the retirement batch at the first successful
        // real GPU submit, not at logical-frame completion. The seal counter
        // and the identity of the batch `defer` is currently filling are the
        // two production values that pin that point down.
        auto &frame_deletion_queue = GET_MODULE(DeletionQueue);
        constexpr std::array transaction_points{
            RayTracingJointTransactionPoint::after_compile,
            RayTracingJointTransactionPoint::after_pipeline,
            RayTracingJointTransactionPoint::after_sbt,
            RayTracingJointTransactionPoint::after_as_record,
            RayTracingJointTransactionPoint::submit,
        };
        for (std::size_t index = 0;
             index < transaction_points.size(); ++index) {
            const auto point = transaction_points[index];
            const auto before_tlas =
                stable_scope->acceleration_structures
                    ->topLevelDeviceAddress();
            REQUIRE(before_tlas != 0);
            layout_bytes += index % 2 == 0 ? " " : "\n";
            writeTextFile(layout_path, layout_bytes);
            const auto before_status =
                reload_service.statusJson()
                    .at("runtime")
                    .at(std::string{
                        watch::renderPipelineReloadParticipantName})
                    .at("details");
            const auto before_plan =
                renderer.currentFramePlanJson();
            const auto stable_config =
                GET_MODULE(ProjectBasicConfig)
                    .renderingConfigJson();
            const auto before_seal_count =
                frame_deletion_queue.currentFrameForTesting();
            const auto before_batch =
                frame_deletion_queue
                    .currentRetirementBatchForTesting();
            REQUIRE_FALSE(before_batch.expired());
            bool accepted = false;
            {
                const ScopedRayTracingJointTransactionInjector injector{
                    [point](RayTracingJointTransactionPoint observed) {
                        if (observed == point) {
                            throw std::runtime_error(
                                "injected WP366 joint transaction failure");
                        }
                    }};
                accepted =
                    reload_service.applyRequestForTesting(
                        watch::ReloadRequest{
                            watch::makeAssetKey("ray_layout.json"),
                            watch::ReloadKind::modified, {},
                            10 + index});
                if (point ==
                        RayTracingJointTransactionPoint::after_compile ||
                    point ==
                        RayTracingJointTransactionPoint::after_pipeline) {
                    CHECK_FALSE(accepted);
                    CHECK(runtime.stagedGeneration() == 0);
                } else {
                    REQUIRE(accepted);
                    REQUIRE(runtime.snapshot() == stable_root);
                    REQUIRE(runtime.stagedGeneration() != 0);
                    CHECK_THROWS_WITH(
                        renderer.render(),
                        Catch::Matchers::ContainsSubstring(
                            "injected WP366 joint transaction failure"));
                }
            }
            CHECK(runtime.snapshot() == stable_root);
            CHECK(runtime.activeGeneration() ==
                  stable_root->generation);
            CHECK(runtime.stagedGeneration() == 0);
            CHECK(stable_scope->acceleration_structures
                      ->topLevelDeviceAddress() == before_tlas);
            CHECK(frame_resources
                      .rayTracingSceneShaderBindingTablesLeaseForTesting() ==
                  stable_sbt);
            const auto after_fault_bundle =
                frame_resources.activeRtDescriptorBundleForTesting();
            REQUIRE(after_fault_bundle != nullptr);
            CHECK(after_fault_bundle == stable_descriptor_bundle);
            CHECK(after_fault_bundle->top_level ==
                  stable_descriptor_bundle->top_level);
            const auto after_status =
                reload_service.statusJson()
                    .at("runtime")
                    .at(std::string{
                        watch::renderPipelineReloadParticipantName})
                    .at("details");
            CHECK(after_status.at("attempted") ==
                  before_status.at("attempted").get<std::uint64_t>() +
                      1);
            CHECK(after_status.at("applied") ==
                  before_status.at("applied"));
            CHECK(after_status.at("failed") ==
                  before_status.at("failed").get<std::uint64_t>() + 1);
            CHECK(after_status.at("generation") ==
                  before_status.at("generation"));
            CHECK(after_status.at("watched_sources") ==
                  before_status.at("watched_sources"));
            CHECK(GET_MODULE(ProjectBasicConfig)
                      .renderingConfigJson() == stable_config);
            CHECK(renderer.currentFramePlanJson() == before_plan);
            // Every one of these five points faults before the frame's first
            // real GPU submit, so nothing may be sealed: the rejected
            // candidate stays in the still-current batch and retires with the
            // next frame's fence rather than being orphaned or freed early.
            CHECK(frame_deletion_queue.currentFrameForTesting() ==
                  before_seal_count);
            CHECK_FALSE(before_batch.expired());

            // Render once more after each rejected point. A candidate pass
            // id, config, watch set, temporal reset, or applied counter leak
            // makes either this frame or its byte comparison diverge.
            renderer.render();
            vkcore.waitIdle();
            CHECK(RendererTestAccess::readRenderTarget(renderer,
                      "wp366_a", ImageSubresourceRange{}).bytes ==
                  output_a.bytes);
            CHECK(RendererTestAccess::readRenderTarget(renderer,
                      "wp366_b", ImageSubresourceRange{}).bytes ==
                  output_b.bytes);
            const auto reused =
                RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
            CHECK(reused.tlas_build_count ==
                  stable_diagnostics.tlas_build_count);
            CHECK(reused.descriptor_bundle_generation ==
                  stable_diagnostics.descriptor_bundle_generation);
            // The recovery frame is the first real submit after the rejection.
            // It seals the batch the rejected frame was still filling, and the
            // offscreen target has already waited on that submission's fence,
            // so the rejected candidate is gone by the time render() returns.
            CHECK(frame_deletion_queue.currentFrameForTesting() ==
                  before_seal_count + 1);
            CHECK(before_batch.expired());
        }

        // Feed malformed trace operands through the real config loader,
        // ShaderLibrary, and PipelineFactory. The scoped mutation seam wraps
        // the final RT1b routing hookup itself, so the exact same compiled
        // shader must stage only while that hookup is disabled.
        const auto raygen_path =
            temp_dir / "shaders" / "wp366_a.rgen";
        const auto valid_raygen = readTextFile(raygen_path);
        struct InvalidAddressingCase {
            std::string_view token;
            std::string_view replacement;
            std::string_view error;
        };
        constexpr std::array invalid_addressing{
            InvalidAddressingCase{
                "pelican_ray_type_count", "1u",
                "pelican.ray_tracing.record_index_stride_invalid@1"},
            InvalidAddressingCase{
                "PELICAN_RAY_TYPE_primary",
                "PELICAN_RAY_TYPE_shadow",
                "pelican.ray_tracing.ray_type_offset_invalid@1"},
            InvalidAddressingCase{
                "PELICAN_MISS_sky", "2u",
                "pelican.ray_tracing.miss_index_invalid@1"},
        };
        std::size_t invalid_addressing_index = 0;
        for (const auto &invalid : invalid_addressing) {
            const auto progress_path =
                temp_dir / "wp366_invalid_addressing_progress.txt";
            const auto progress = [&](std::string_view stage) {
                writeTextFile(
                    progress_path,
                    std::to_string(invalid_addressing_index) + ":" +
                        std::string{stage});
            };
            progress("source");
            auto source = valid_raygen;
            const auto at = source.find(invalid.token);
            REQUIRE(at != std::string::npos);
            source.replace(at, invalid.token.size(),
                           invalid.replacement);
            writeTextFile(raygen_path, source);
            auto invalid_config = readTextFile(config_path);
            // MG-2b: the project is a graph root; its tasks are in the task
            // feature, which the direct loads below read from disk.
            const auto task_feature_path =
                temp_dir / "features" / "authored_tasks.json";
            const auto valid_task_feature = readTextFile(task_feature_path);
            if (invalid.error ==
                "pelican.ray_tracing.ray_type_offset_invalid@1") {
                auto config = nlohmann::json::parse(valid_task_feature);
                auto &tasks = config.at("compute_tasks");
                const auto task = std::find_if(
                    tasks.begin(), tasks.end(), [](const auto &candidate) {
                        return candidate.at("name") == "wp366_a";
                    });
                REQUIRE(task != tasks.end());
                auto &pipeline = task->at("ray_tracing");
                pipeline["traced_ray_types"] = {"primary"};
                REQUIRE(pipeline.at("hit_shaders").erase("shadow") == 1);
                writeTextFile(task_feature_path, config.dump(2));
            }
            progress("direct-load");
            CHECK_THROWS_WITH(
                loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                    invalid_config),
                Catch::Matchers::ContainsSubstring(
                    std::string{invalid.error}));
            progress("direct-rejected");
            CHECK(runtime.snapshot() == stable_root);
            CHECK(runtime.stagedGeneration() == 0);
            {
                const ScopedRayTracingRecordAddressingValidationOverrideForTesting
                    disabled_addressing{false};
                progress("disabled-load");
                const auto accepted =
                    loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                        invalid_config);
                CHECK(isValidRenderingPassId(accepted.flat));
                progress("disabled-staged");
                const auto invalid_generation =
                    runtime.stagedGeneration();
                REQUIRE(invalid_generation != 0);
                progress("rollback");
                REQUIRE(runtime.rollbackStagedGeneration(
                    invalid_generation));
                progress("rolled-back");
            }
            CHECK(runtime.snapshot() == stable_root);
            CHECK(runtime.stagedGeneration() == 0);
            writeTextFile(raygen_path, valid_raygen);
            writeTextFile(task_feature_path, valid_task_feature);
            progress("restored");
            ++invalid_addressing_index;
        }

        // Change a shader-record declaration behind the production config
        // loader. The real ShaderLibrary reflection and PipelineFactory gate
        // must reject it by name without exposing a candidate generation.
        const auto wrong_hit_path =
            temp_dir / "shaders" /
            "wp366_opaque_primary.rchit";
        auto wrong_hit = readTextFile(wrong_hit_path);
        const auto valid_wrong_hit = wrong_hit;
        const auto flags_declaration =
            wrong_hit.find("    uint flags;\n");
        REQUIRE(flags_declaration != std::string::npos);
        wrong_hit.insert(flags_declaration,
                         "    float unexpected;\n");
        writeTextFile(wrong_hit_path, wrong_hit);
        const auto before_wrong_hit_tlas =
            stable_scope->acceleration_structures
                ->topLevelDeviceAddress();
        REQUIRE(before_wrong_hit_tlas != 0);
        CHECK_THROWS_WITH(
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                readTextFile(temp_dir / "hybrid.json")),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.shader_record_abi_mismatch@1"));
        CHECK(runtime.snapshot() == stable_root);
        CHECK(runtime.stagedGeneration() == 0);
        CHECK(stable_scope->acceleration_structures
                  ->topLevelDeviceAddress() == before_wrong_hit_tlas);
        writeTextFile(wrong_hit_path, valid_wrong_hit);

        // The two authored material selectors deliberately share one asset
        // and geometry.  Making their persistent scene identity coincide must
        // be rejected by the production renderer before a divergent row can
        // reach TLAS/SBT publication.
        instances.setTrs(restored_b, {-2.0f, 0.0f, 0.0f},
                         glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});
        const auto before_ambiguous_tlas =
            stable_scope->acceleration_structures
                ->topLevelDeviceAddress();
        REQUIRE(before_ambiguous_tlas != 0);
        CHECK_THROWS_WITH(
            renderer.render(),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_row_binding_ambiguous@1"));
        CHECK(runtime.snapshot() == stable_root);
        CHECK(stable_scope->acceleration_structures
                  ->topLevelDeviceAddress() == before_ambiguous_tlas);

        const auto pre_b_output_a =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto pre_b_output_b =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_b", ImageSubresourceRange{});
        instances.setTrs(restored_b, {20.0f, 0.0f, 0.0f},
                         glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.0f});
        // A fault after the no-fail publication callback is an output fault,
        // not a generation rollback.  The next frame must reuse exactly the
        // newly published TLAS/SBT/descriptor bundle.
        layout_bytes += " ";
        writeTextFile(layout_path, layout_bytes);
        const auto seal_before_publication_fault =
            frame_deletion_queue.currentFrameForTesting();
        const auto publication_fault_batch =
            frame_deletion_queue.currentRetirementBatchForTesting();
        REQUIRE_FALSE(publication_fault_batch.expired());
        {
            const ScopedRayTracingJointTransactionInjector injector{
                [](RayTracingJointTransactionPoint observed) {
                    if (observed ==
                        RayTracingJointTransactionPoint::after_publication) {
                        throw std::runtime_error(
                            "injected WP384 output publication failure");
                    }
                }};
            REQUIRE(reload_service.applyRequestForTesting(
                watch::ReloadRequest{
                    watch::makeAssetKey("ray_layout.json"),
                    watch::ReloadKind::modified, {}, 100}));
            CHECK_THROWS_WITH(
                renderer.render(),
                Catch::Matchers::ContainsSubstring(
                    "injected WP384 output publication failure"));
        }
        // WP384 decision 11 observed at the production seal point. This frame's
        // real GPU submit succeeded and only the output publication faulted, so
        // the batch must have been sealed by that submit rather than by
        // logical-frame completion, which this frame never reached. The
        // offscreen target already waited on that submission's fence, so the
        // sealed batch, and everything the aborted frame deferred into it,
        // is retired by the time render() returns. Moving the seal back after
        // `frame_abort_guard.complete()` leaves the counter unmoved and keeps
        // the rejected candidate resident in the still-current batch.
        CHECK(frame_deletion_queue.currentFrameForTesting() ==
              seal_before_publication_fault + 1);
        CHECK(publication_fault_batch.expired());
        CHECK_FALSE(frame_deletion_queue
                        .currentRetirementBatchForTesting()
                        .expired());
        const auto published_b = runtime.snapshot();
        REQUIRE(published_b != nullptr);
        CHECK(published_b != stable_root);
        CHECK(runtime.stagedGeneration() == 0);
        const auto published_b_bundle =
            frame_resources.activeRtDescriptorBundleForTesting();
        const auto published_b_sbt =
            frame_resources
                .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(published_b_bundle != nullptr);
        REQUIRE(published_b_sbt != nullptr);
        CHECK(published_b_bundle != stable_descriptor_bundle);
        CHECK(published_b_sbt != stable_sbt);
        const auto published_b_diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        renderer.render();
        vkcore.waitIdle();
        const auto reused_b_diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK(reused_b_diagnostics.tlas_build_count ==
              published_b_diagnostics.tlas_build_count);
        CHECK(reused_b_diagnostics.descriptor_bundle_generation ==
              published_b_diagnostics.descriptor_bundle_generation);
        CHECK(frame_resources.activeRtDescriptorBundleForTesting() ==
              published_b_bundle);
        CHECK(frame_resources
                  .rayTracingSceneShaderBindingTablesLeaseForTesting() ==
              published_b_sbt);
        const auto reused_b_output_a =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto reused_b_output_b =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_b", ImageSubresourceRange{});
        CHECK(pixel(reused_b_output_a, 1) != pixel(pre_b_output_a, 1));
        CHECK(pixel(reused_b_output_b, 1) != pixel(pre_b_output_b, 1));

        // RT -> non-RT is the same publication transaction with an empty
        // optional bundle. A pre-submit fault must leave B selected; the
        // following successful frame publishes the bundle-less generation.
        auto non_rt_config = nlohmann::json::parse(
            readTextFile(temp_dir / "hybrid.json"));
        // MG-2b: the project is a graph root. Its authored tasks are in the
        // task feature, so dropping that reference drops every task.
        auto &non_rt = renderingSectionOf(non_rt_config);
        non_rt.erase("ray_group_layout");
        non_rt.erase("ray_bindings");
        auto &non_rt_features = non_rt["features"];
        non_rt_features.erase(
            std::remove(non_rt_features.begin(), non_rt_features.end(),
                        nlohmann::json(
                            "project://features/authored_tasks.json")),
            non_rt_features.end());
        non_rt["target_planning"]["graphs"]["main_render"]
              ["required_capabilities"] = nlohmann::json::array();
        writeTextFile(temp_dir / "hybrid.json", non_rt_config.dump(2));
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                watch::makeAssetKey("hybrid.json"),
                watch::ReloadKind::modified, {}, 101}));
        REQUIRE(runtime.stagedGeneration() != 0);
        const auto seal_before_clear_fault =
            frame_deletion_queue.currentFrameForTesting();
        const auto clear_fault_batch =
            frame_deletion_queue.currentRetirementBatchForTesting();
        REQUIRE_FALSE(clear_fault_batch.expired());
        {
            const ScopedRayTracingJointTransactionInjector injector{
                [](RayTracingJointTransactionPoint observed) {
                    if (observed ==
                        RayTracingJointTransactionPoint::submit) {
                        throw std::runtime_error(
                            "injected WP384 RT clear submit failure");
                    }
                }};
            CHECK_THROWS_WITH(
                renderer.render(),
                Catch::Matchers::ContainsSubstring(
                    "injected WP384 RT clear submit failure"));
        }
        // The mirrored control: this fault precedes the frame's only submit,
        // so no seal may happen and the batch stays current.
        CHECK(frame_deletion_queue.currentFrameForTesting() ==
              seal_before_clear_fault);
        CHECK_FALSE(clear_fault_batch.expired());
        CHECK(runtime.snapshot() == published_b);
        CHECK(frame_resources.activeRtDescriptorBundleForTesting() ==
              published_b_bundle);
        CHECK(frame_resources
                  .rayTracingSceneShaderBindingTablesLeaseForTesting() ==
              published_b_sbt);
        CHECK(runtime.stagedGeneration() == 0);
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                watch::makeAssetKey("hybrid.json"),
                watch::ReloadKind::modified, {}, 102}));
        REQUIRE(runtime.stagedGeneration() != 0);
        renderer.render();
        vkcore.waitIdle();
        CHECK(runtime.stagedGeneration() == 0);
        CHECK(frame_resources.activeRtDescriptorBundleForTesting() ==
              nullptr);
        CHECK(frame_resources
                  .rayTracingSceneShaderBindingTablesLeaseForTesting() ==
              nullptr);

        CHECK(teardown.run().succeeded());
        CHECK(deletion_queue.pendingCountForTesting() == 0);
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan WP366 renderer A/B fixture unavailable");
        throw;
    }
#else
    SUCCEED("WP366 layout projects are rejected before GPU work when the runtime compiler is disabled");
#endif
}

TEST_CASE(
    "WP369 production material alpha profile matches mip zero oracle and next frame overrides",
    "[WP369][headless][gpu][ray-tracing][rt2b-2][profile][oracle][override][identity][production][mutation]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeWp369MaterialAlphaProject(temp_dir);
        const auto rendering_config =
            readTextFile(temp_dir / "hybrid.json");
        setWp369PortableProfile(temp_dir, false);
        configureRayQueryTestRuntime(temp_dir);

        auto &vkcore = GET_MODULE(VulkanManageCore);
        const auto &capabilities = vkcore.getRuntimeCapabilities();
        if (!capabilities.ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        if (!capabilities.ray_material_bindless) {
            SKIP("pelican.vulkan.ray_material_bindless@1 is unavailable");
        }

        auto &shaders = GET_MODULE(ShaderLibrary);
        const auto profile_absent_shader_checkpoint =
            shaders.checkpointRegistrations();
        auto &model_assets = GET_MODULE(ModelAssetContainer);
        const auto &loaded = model_assets.getModelTemplateByName("tree");
        REQUIRE(loaded.authored_asset_reference == "models/tree.glb");
        REQUIRE(loaded.material_primitives.size() == 8);
        std::array<GlobalMaterialId, 8> material_by_source{
            invalidMaterialId(), invalidMaterialId(), invalidMaterialId(),
            invalidMaterialId(), invalidMaterialId(), invalidMaterialId(),
            invalidMaterialId(), invalidMaterialId()};
        std::array<std::size_t, 8> primitive_count{};
        for (const auto &group : loaded.material_primitives) {
            REQUIRE(group.source_material_index.has_value());
            REQUIRE(*group.source_material_index < material_by_source.size());
            material_by_source[*group.source_material_index] = group.material;
            primitive_count[*group.source_material_index] =
                group.primitives.size();
        }
        REQUIRE(primitive_count ==
                std::array<std::size_t, 8>{1, 1, 1, 1, 1, 1, 1, 1});

        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto model_instance = instances.placeModelInstance(loaded);
        auto &camera = GET_MODULE(Camera);
        camera.setPos({0.0F, 0.0F, 0.0F});
        camera.setDir({0.0F, 0.0F, 1.0F});
        camera.setUp({0.0F, 1.0F, 0.0F});
        auto &renderer = GET_MODULE(Renderer);
        renderer.render();
        vkcore.waitIdle();

        const auto generated_registration_count =
            [&](const ShaderLibrary::RegistrationCheckpoint &checkpoint) {
                return std::ranges::count_if(
                    shaders.registrationsSince(checkpoint),
                    [&](ShaderBundleId id) {
                        return shaders.get(id).engine_generated;
                    });
            };
        // This is a real compile/registration observation, not a parse-only
        // stand-in: an otherwise identical RT2b layout has no generated
        // shader registration when portable_profile is absent.
        CHECK(generated_registration_count(
                  profile_absent_shader_checkpoint) == 0);

        const auto marker_at = [](const RenderTargetReadback &readback,
                                  std::uint32_t x) {
            std::uint32_t marker = 0;
            const auto offset = static_cast<std::size_t>(x) * 16;
            if (offset + sizeof(marker) > readback.bytes.size()) {
                throw std::out_of_range(
                    "WP369 sentinel pixel is outside readback");
            }
            std::memcpy(&marker, readback.bytes.data() + offset,
                        sizeof(marker));
            return marker;
        };
        const auto profile_absent =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        for (std::uint32_t lane = 0; lane < 8; ++lane) {
            CAPTURE(lane);
            CHECK(marker_at(profile_absent, lane) == 0x3690u);
        }
        const auto absent_plan = renderer.currentFramePlanJson();
        const auto absent_task = std::ranges::find_if(
            absent_plan.at("nodes"), [](const auto &node) {
                return node.at("name") == "wp366_a";
            });
        REQUIRE(absent_task != absent_plan.at("nodes").end());
        CHECK_FALSE(absent_task->at("shader_resolution")
                        .contains("anyhit_slots"));

        // Load the same authored asset identity from another physical path.
        // The original came through ModelAssetContainer; only the mirror is
        // committed directly, so the comparison crosses the production
        // authored/physical separation rather than constructing IR by hand.
        const auto mirror_path = temp_dir / "mirror" / "tree.glb";
        std::filesystem::create_directories(mirror_path.parent_path());
        std::filesystem::copy_file(
            temp_dir / "models" / "tree.glb", mirror_path,
            std::filesystem::copy_options::overwrite_existing);
        auto &gltf = GET_MODULE(GltfLoader);
        auto mirror = gltf.commit(
            gltf.prepareGltfBinary(mirror_path.string()),
            GltfAuthoredAssetReference{"models/tree.glb"});
        REQUIRE(mirror.authored_asset_reference == "models/tree.glb");
        const auto mirror_group = std::ranges::find(
            mirror.material_primitives, 0u,
            &ModelTemplate::MaterialPrimitives::source_material_index);
        REQUIRE(mirror_group != mirror.material_primitives.end());

        auto &materials = GET_MODULE(MaterialContainer);
        const auto source_for = [&](GlobalMaterialId material,
                                    std::string canonical_key) {
            return materials.rayMaterialTableSource(
                material, std::move(canonical_key),
                materials.rayMaterialGeneration(material),
                materials.rayMaterialLease(material));
        };
        const auto original_source = source_for(
            material_by_source[0], "asset:models/tree.glb#0");
        const auto mirror_source = source_for(
            mirror_group->material, "asset:models/tree.glb#0");
        REQUIRE(original_source.has_normalized_alpha_ir);
        REQUIRE(mirror_source.has_normalized_alpha_ir);
        CHECK(original_source.alpha_material_identity ==
              mirror_source.alpha_material_identity);
        CHECK(original_source.alpha_texture_identity ==
              mirror_source.alpha_texture_identity);
        CHECK(original_source.normalized_alpha_digest_sha256 ==
              mirror_source.normalized_alpha_digest_sha256);
        CHECK(original_source.material_handle !=
              mirror_source.material_handle);

        const auto active_layout = parseRayGroupLayout(
            [&] {
                setWp369PortableProfile(temp_dir, true);
                return readTextFile(temp_dir / "ray_layout.json");
            }(),
            "ray_layout.json");
        const std::array geometry_input{
            RayQueryGeometryInstanceSnapshot{
                .geometry_allocation_id = 1,
                .index_count = 3,
                .vertex_count = 3,
                .attribute_presence = rayGeometryAttributeUv0,
            }};
        const auto identity_geometry =
            makeRayTracingGeometrySnapshot(
                geometry_input, 0x1000, 0x2000);
        const auto build_identity_table =
            [&](const RayMaterialTableSource &source,
                std::string material_asset = "models/tree.glb") {
                const std::array row_input{
                    RaySceneRowInput{
                        .scene_identity = "identity-scene",
                        .geometry_identity = "identity-geometry",
                        .material_asset = std::move(material_asset),
                        .source_material_index = 0,
                        .resolved_material_handle =
                            source.material_handle,
                        .resolved_material_generation =
                            source.material_generation,
                        .resolved_material_lease =
                            source.material_lease,
                        .geometry_allocation_id = 1,
                    }};
                const auto rows = resolveRaySceneRows(
                    row_input, active_layout, nullptr);
                const std::array sources{source};
                return buildRayTracingTableSnapshot(
                    rows, sources, identity_geometry,
                    &active_layout);
            };
        const auto original_table =
            build_identity_table(original_source);
        const auto mirror_table = build_identity_table(mirror_source);
        CHECK((original_table.tables.material_bytes ==
               mirror_table.tables.material_bytes));
        CHECK(original_table.tables.material_digest_sha256 ==
              mirror_table.tables.material_digest_sha256);

        // A relative physical path is still only a physical path. The legacy
        // API must not infer authored provenance from its spelling; the
        // profile rejects it later because no typed authored reference made
        // normalized IR available.
        const auto relative_mirror_path = std::filesystem::relative(
            mirror_path, std::filesystem::current_path());
        REQUIRE(relative_mirror_path.is_relative());
        auto legacy_relative = gltf.loadGltfBinary(
            relative_mirror_path.string());
        CHECK(legacy_relative.authored_asset_reference.empty());
        const auto legacy_group = std::ranges::find(
            legacy_relative.material_primitives, 0u,
            &ModelTemplate::MaterialPrimitives::source_material_index);
        REQUIRE(legacy_group != legacy_relative.material_primitives.end());
        const auto legacy_source = source_for(
            legacy_group->material, "asset:legacy-relative.glb#0");
        CHECK_FALSE(legacy_source.has_normalized_alpha_ir);
        CHECK_THROWS_WITH(
            build_identity_table(legacy_source, "legacy-relative.glb"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_profile_alpha_ir_missing@1"));

        const auto load_transform_source =
            [&](std::string_view filename,
                const nlohmann::json &transform) {
                const auto path = temp_dir / "transform" /
                                  std::string{filename};
                std::filesystem::create_directories(
                    path.parent_path());
                TestGltfFragmentFixture::writeWp369Glb(
                    path, transform);
                const auto authored =
                    "transform/" + std::string{filename};
                auto model = gltf.commit(
                    gltf.prepareGltfBinary(path.string()),
                    GltfAuthoredAssetReference{authored});
                const auto group = std::ranges::find(
                    model.material_primitives, 0u,
                    &ModelTemplate::MaterialPrimitives::source_material_index);
                if (group == model.material_primitives.end()) {
                    throw std::logic_error(
                        "WP369 transform material is missing");
                }
                const auto material = group->material;
                auto source = source_for(
                    material, "asset:" + authored + "#0");
                return std::pair{
                    std::move(model), std::move(source)};
            };
        auto [identity_transform_model, identity_transform_source] =
            load_transform_source(
                "identity.glb",
                {{"offset", {0.0, 0.0}},
                 {"scale", {1.0, 1.0}},
                 {"rotation", 0.0}});
        CHECK(identity_transform_source.normalized_uv_transform_state ==
              static_cast<std::uint32_t>(
                  NormalizedAlphaUvTransformState::identity));
        CHECK_NOTHROW(build_identity_table(
            identity_transform_source, "transform/identity.glb"));

        auto [nonidentity_transform_model, nonidentity_transform_source] =
            load_transform_source(
                "nonidentity.glb", {{"offset", {0.5, 0.0}}});
        CHECK(nonidentity_transform_source.normalized_uv_transform_state ==
              static_cast<std::uint32_t>(
                  NormalizedAlphaUvTransformState::non_identity));
        CHECK_THROWS_WITH(
            build_identity_table(
                nonidentity_transform_source,
                "transform/nonidentity.glb"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_profile_uv_transform_unsupported@1"));

        auto [invalid_transform_model, invalid_transform_source] =
            load_transform_source(
                "invalid.glb", {{"offset", {0.5}}});
        CHECK(invalid_transform_source.normalized_uv_transform_state ==
              static_cast<std::uint32_t>(
                  NormalizedAlphaUvTransformState::invalid));
        CHECK_THROWS_WITH(
            build_identity_table(
                invalid_transform_source,
                "transform/invalid.glb"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_profile_uv_transform_unsupported@1"));

        const auto active_shader_checkpoint =
            shaders.checkpointRegistrations();
        const auto active_variant =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                rendering_config);
        REQUIRE(isValidRenderingPassId(active_variant.flat));
        REQUIRE(generated_registration_count(
                    active_shader_checkpoint) > 0);
        renderer.render();
        vkcore.waitIdle();

        const auto profile_active =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto raster_active =
            RendererTestAccess::readRenderTarget(renderer,
                "gbuffer_albedo", ImageSubresourceRange{});
        constexpr std::uint32_t miss_marker = 1280u;
        constexpr std::uint32_t hit_marker = 0x3690u;
        constexpr std::array<double, 2> alpha_mip0{
            64.0 / 255.0, 192.0 / 255.0};
        constexpr std::array<double, 2> uv_x{0.25, 0.75};
        constexpr double cutoff = 0.5;
        for (std::size_t lane = 0; lane < 2; ++lane) {
            CAPTURE(lane, alpha_mip0[lane]);
            const auto boundary_margin =
                std::min(std::abs(uv_x[lane] - 0.5),
                         std::min(uv_x[lane], 1.0 - uv_x[lane]));
            CHECK(boundary_margin == 0.25);
            const bool cpu_ignored = alpha_mip0[lane] < cutoff;
            const bool gpu_ignored =
                marker_at(profile_active,
                          static_cast<std::uint32_t>(lane)) ==
                miss_marker;
            CHECK(gpu_ignored == cpu_ignored);
        }
        const auto ray_visible = [&](const RenderTargetReadback &readback,
                                     std::size_t lane) {
            const auto marker = marker_at(
                readback, static_cast<std::uint32_t>(lane));
            if (marker != miss_marker && marker != hit_marker) {
                throw std::runtime_error(
                    "WP369 ray visibility marker is outside the fixture ABI");
            }
            return marker == hit_marker;
        };
        const auto raster_visible = [&](const RenderTargetReadback &readback,
                                        std::size_t lane) {
            const auto bgr = readback.format ==
                                 vk::Format::eB8G8R8A8Unorm ||
                             readback.format ==
                                 vk::Format::eB8G8R8A8Srgb;
            const auto rgb = readback.format ==
                                 vk::Format::eR8G8B8A8Unorm ||
                             readback.format ==
                                 vk::Format::eR8G8B8A8Srgb;
            if ((!bgr && !rgb) || lane >= 6) {
                throw std::logic_error(
                    "WP369 raster oracle readback contract is invalid: " +
                    vk::to_string(readback.format));
            }
            for (std::size_t offset = 0;
                 offset + 3 < readback.bytes.size(); offset += 4) {
                const auto red = readback.bytes[
                    offset + (bgr ? 2 : 0)];
                const auto green = readback.bytes[offset + 1];
                const auto blue = readback.bytes[
                    offset + (bgr ? 0 : 2)];
                const auto high = [](std::uint8_t value) {
                    return value > 190;
                };
                const auto low = [](std::uint8_t value) {
                    return value < 130;
                };
                const bool matches = [&] {
                    switch (lane) {
                    case 0:
                        return high(red) && low(green) && low(blue);
                    case 1:
                        return low(red) && high(green) && low(blue);
                    case 2:
                        return low(red) && low(green) && high(blue);
                    case 3:
                        return high(red) && high(green) && low(blue);
                    case 4:
                        return low(red) && high(green) && high(blue);
                    case 5:
                        return high(red) && low(green) && high(blue);
                    default:
                        return false;
                    }
                }();
                if (matches) {
                    return true;
                }
            }
            return false;
        };
        const auto require_shared_visibility =
            [&](const RenderTargetReadback &ray,
                const RenderTargetReadback &raster,
                const std::array<bool, 6> &expected) {
                for (std::size_t lane = 0; lane < expected.size(); ++lane) {
                    CAPTURE(lane, expected[lane]);
                    const auto rt = ray_visible(ray, lane);
                    const auto raster_result = raster_visible(raster, lane);
                    CHECK(rt == expected[lane]);
                    CHECK(raster_result == expected[lane]);
                    CHECK(rt == raster_result);
                }
            };
        constexpr std::array<bool, 6> active_visibility{
            false, true, true, false, true, true};
        require_shared_visibility(
            profile_active, raster_active, active_visibility);
        CHECK(ray_visible(profile_active, 6));
        CHECK(ray_visible(profile_active, 7));
        CHECK(profile_active.bytes != profile_absent.bytes);

        auto profile_tables = GET_MODULE(FrameResources)
                                  .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(profile_tables != nullptr);
        REQUIRE(profile_tables->ray_tables.profile_material_rows.size() == 8);
        const auto material_row = [&](const auto &tables,
                                      std::uint32_t source_material_index)
            -> const RayProfileMaterialTableRow & {
            const auto key = source_material_index == 6
                                 ? std::string{
                                       "document:materials/wp369.material.json#ProjectOpaque"}
                                 : "asset:models/tree.glb#" +
                                       std::to_string(source_material_index);
            const auto scene_row = std::ranges::find(
                tables.row_snapshot->rows, key,
                &RaySceneRow::material_reference);
            if (scene_row == tables.row_snapshot->rows.end()) {
                throw std::out_of_range(
                    "WP369 material row is absent");
            }
            return tables.ray_tables.profile_material_rows.at(
                scene_row->material_index);
        };
        CHECK(material_row(*profile_tables, 2).texture_index ==
              rayMissingTextureIndex);
        CHECK(material_row(*profile_tables, 2).base_factor_a == 1.0F);
        CHECK(material_row(*profile_tables, 3).uses_vertex_color == 1u);
        CHECK(material_row(*profile_tables, 4).uses_vertex_color == 1u);
        CHECK(material_row(*profile_tables, 6).alpha_mode == 0u);
        CHECK(material_row(*profile_tables, 7).alpha_mode == 2u);
        CHECK(material_row(*profile_tables, 5).cutoff == 2.0F);
        const auto project_source = source_for(
            material_by_source[6],
            "document:materials/wp369.material.json#ProjectOpaque");
        CHECK_FALSE(project_source.has_normalized_alpha_ir);

        PublishMaterialInstanceOverrideDescV1 relative;
        relative.instance = instances.animationInstance(model_instance);
        relative.frame_revision = 1;
        relative.values.mask = materialOverrideBaseColor |
                               materialOverrideUvTransform;
        relative.values.base_color_factor = {1.0F, 1.0F, 1.0F, 0.0F};
        relative.values.uv_offset = {0.5F, 0.0F};
        relative.values.uv_scale = {1.0F, 1.0F};
        REQUIRE(instances.publishMaterialInstanceOverride(
                    model_instance, relative) == Animation::Status::ok);
        renderer.render();
        vkcore.waitIdle();
        const auto relative_frame =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto relative_raster =
            RendererTestAccess::readRenderTarget(renderer,
                "gbuffer_albedo", ImageSubresourceRange{});
        constexpr std::array<bool, 6> relative_visibility{
            true, false, true, false, true, true};
        require_shared_visibility(
            relative_frame, relative_raster, relative_visibility);
        CHECK(relative_frame.bytes != profile_active.bytes);
        auto relative_tables = GET_MODULE(FrameResources)
                                   .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(relative_tables != nullptr);
        CHECK(material_row(*relative_tables, 2).base_factor_a == 1.0F);
        CHECK(material_row(*relative_tables, 0).uv_offset_scale ==
              std::array<float, 4>{0.5F, 0.0F, 1.0F, 1.0F});

        PublishMaterialInstanceAbsoluteOverrideDescV2 absolute_uv;
        absolute_uv.instance = instances.animationInstance(model_instance);
        absolute_uv.frame_revision = 1;
        absolute_uv.source_material_index = 1;
        absolute_uv.values.mask = materialOverrideUvTransform;
        absolute_uv.values.uv_offset = {0.0F, 0.0F};
        absolute_uv.values.uv_scale = {1.0F, 1.0F};
        REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(
                    model_instance, absolute_uv) == Animation::Status::ok);
        renderer.render();
        vkcore.waitIdle();
        const auto absolute_uv_frame =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto absolute_uv_raster =
            RendererTestAccess::readRenderTarget(renderer,
                "gbuffer_albedo", ImageSubresourceRange{});
        constexpr std::array<bool, 6> absolute_uv_visibility{
            true, true, true, false, true, true};
        require_shared_visibility(
            absolute_uv_frame, absolute_uv_raster,
            absolute_uv_visibility);
        auto absolute_uv_tables = GET_MODULE(FrameResources)
                                      .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(absolute_uv_tables != nullptr);
        CHECK(material_row(*absolute_uv_tables, 0).uv_offset_scale ==
              std::array<float, 4>{0.5F, 0.0F, 1.0F, 1.0F});
        CHECK(material_row(*absolute_uv_tables, 1).uv_offset_scale ==
              std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F});

        PublishMaterialInstanceAbsoluteOverrideDescV2 negative_alpha;
        negative_alpha.instance = instances.animationInstance(model_instance);
        negative_alpha.frame_revision = 1;
        negative_alpha.source_material_index = 2;
        negative_alpha.values.mask = materialOverrideBaseColor;
        negative_alpha.values.base_color_factor =
            {0.1F, 0.1F, 1.0F, -0.1F};
        REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(
                    model_instance, negative_alpha) ==
                Animation::Status::ok);
        renderer.render();
        vkcore.waitIdle();
        const auto clamped_frame =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto clamped_raster =
            RendererTestAccess::readRenderTarget(renderer,
                "gbuffer_albedo", ImageSubresourceRange{});
        require_shared_visibility(
            clamped_frame, clamped_raster,
            absolute_uv_visibility);
        auto clamped_tables = GET_MODULE(FrameResources)
                                  .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(clamped_tables != nullptr);
        CHECK(material_row(*clamped_tables, 2).base_factor_a == -0.1F);

        const auto require_geometry_flags =
            [&](std::uint32_t expected) {
                const auto tables =
                    GET_MODULE(FrameResources)
                        .rayTracingSceneShaderBindingTablesLeaseForTesting();
                REQUIRE(tables != nullptr);
                REQUIRE(tables->row_snapshot != nullptr);
                REQUIRE_FALSE(tables->row_snapshot->rows.empty());
                for (const auto &row : tables->row_snapshot->rows) {
                    CHECK(row.geometry_flags == expected);
                }
                const auto diagnostics =
                    RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
                REQUIRE_FALSE(diagnostics.active_blas_inputs.empty());
                for (const auto &resident :
                     diagnostics.active_blas_inputs) {
                    CHECK(resident.geometry_flags == expected);
                }
            };

        // WP377 step 1: the profile is active and authored geometry flags are
        // non-opaque. Both the resolved rows and resident BLAS keys must carry
        // the production value before the cache-reuse contrast begins.
        require_geometry_flags(0);

        // WP377 step 2: disable only the profile and author opaque geometry.
        // The layout stage and render both succeed, publishing an opaque
        // resident BLAS which the final step is required to inspect on reuse.
        setWp369PortableProfile(temp_dir, false);
        setWp369GeometryOpaque(temp_dir, true);
        const auto removed_variant =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                rendering_config);
        REQUIRE(isValidRenderingPassId(removed_variant.flat));
        renderer.render();
        vkcore.waitIdle();
        const auto profile_removed =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        for (std::uint32_t lane = 0; lane < 8; ++lane) {
            CAPTURE(lane);
            CHECK(marker_at(profile_removed, lane) == hit_marker);
        }
        CHECK(profile_removed.bytes != clamped_frame.bytes);
        require_geometry_flags(rayGeometryOpaque);

        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        const auto stable_root = runtime.snapshot();
        REQUIRE(stable_root != nullptr);
        const auto stable_scope = std::ranges::find_if(
            stable_root->gpu_arena->scopes,
            [](const auto &scope) {
                return scope.ray_tracing_project != nullptr;
            });
        REQUIRE(stable_scope != stable_root->gpu_arena->scopes.end());
        REQUIRE(stable_scope->acceleration_structures != nullptr);
        const auto stable_tlas =
            stable_scope->acceleration_structures
                ->topLevelDeviceAddress();
        REQUIRE(stable_tlas != 0);
        const auto stable_as =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        const auto stable_tables = GET_MODULE(FrameResources)
                                       .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(stable_tables != nullptr);
        const auto stable_generation = runtime.activeGeneration();

        // WP377 step 3: change only portable_profile. Staging must succeed;
        // Renderer::render must reject the unchanged opaque resident BLAS.
        setWp369PortableProfile(temp_dir, true);
        const auto restored_variant =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                rendering_config);
        REQUIRE(isValidRenderingPassId(restored_variant.flat));
        REQUIRE(runtime.stagedGeneration() != 0);
        CHECK_THROWS_WITH(
            renderer.render(),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_profile_opaque_as@1") &&
                Catch::Matchers::ContainsSubstring("BLAS geometry"));
        CHECK(runtime.snapshot() == stable_root);
        CHECK(runtime.activeGeneration() == stable_generation);
        CHECK(runtime.stagedGeneration() == 0);
        CHECK(stable_scope->acceleration_structures
                  ->topLevelDeviceAddress() == stable_tlas);
        CHECK(RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer)
                  .active_blas_inputs == stable_as.active_blas_inputs);
        CHECK(GET_MODULE(FrameResources)
                  .rayTracingSceneShaderBindingTablesLeaseForTesting() ==
              stable_tables);

        // The rejected generation must not poison the next frame. With no
        // staged publication left, the previous profile-OFF generation draws
        // the same production bytes again.
        renderer.render();
        vkcore.waitIdle();
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_a", ImageSubresourceRange{}).bytes ==
              profile_removed.bytes);
        require_geometry_flags(rayGeometryOpaque);

        // WP377a non-resident contrast: first publish profile ON with the
        // material rule explicitly non-opaque, then change only that rule to
        // an opaque key which is not resident in the active AS state.
        setWp369GeometryOpaque(temp_dir, false);
        const auto nonopaque_variant =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                rendering_config);
        REQUIRE(isValidRenderingPassId(nonopaque_variant.flat));
        REQUIRE(runtime.stagedGeneration() != 0);
        renderer.render();
        vkcore.waitIdle();
        REQUIRE(runtime.stagedGeneration() == 0);
        require_geometry_flags(0);

        setWp369MaterialRuleOpaque(temp_dir, true);
        const auto nonresident_opaque_variant =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                rendering_config);
        REQUIRE(isValidRenderingPassId(
            nonresident_opaque_variant.flat));
        REQUIRE(runtime.stagedGeneration() != 0);
        CHECK_THROWS_WITH(
            renderer.render(),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_profile_opaque_as@1") &&
                Catch::Matchers::ContainsSubstring("BLAS geometry"));
        CHECK(runtime.stagedGeneration() == 0);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan WP369 material-alpha fixture unavailable");
        throw;
    }
#else
    SUCCEED(
        "WP369 generated profile shaders require the runtime compiler; the production loader rejection is covered by the RT2b matrix test");
#endif
}

TEST_CASE(
    "WP368 production project load reaches RT2b tables descriptors and GPU",
    "[WP368][headless][gpu][ray-tracing][rt2b][tables][bda][production][mutation][purge]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeRt2bTableProject(temp_dir);
        const auto active_config =
            readTextFile(temp_dir / "hybrid.json");
        const auto inactive_config =
            wp368InactiveRenderingConfig(active_config);
        writeTextFile(temp_dir / "hybrid.json",
                      inactive_config);
        configureRayQueryTestRuntime(temp_dir);

        auto &vkcore = GET_MODULE(VulkanManageCore);
        const auto &capabilities =
            vkcore.getRuntimeCapabilities();
        if (!capabilities.ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        if (!capabilities.ray_material_bindless) {
            SKIP("pelican.vulkan.ray_material_bindless@1 is unavailable");
        }
        REQUIRE(capabilities.runtime_descriptor_array);
        REQUIRE(capabilities
                    .shader_sampled_image_array_non_uniform_indexing);
        REQUIRE(capabilities.descriptor_binding_partially_bound);
        REQUIRE(capabilities.max_bound_descriptor_sets >= 4);
        REQUIRE(capabilities.max_descriptor_set_samplers >= 1026);
        REQUIRE(capabilities.max_descriptor_set_sampled_images >= 1026);
        REQUIRE(capabilities.max_per_stage_descriptor_samplers >= 1026);
        REQUIRE(
            capabilities.max_per_stage_descriptor_sampled_images >= 1026);
        REQUIRE(capabilities.max_per_stage_resources >= 1035);

        // ProjectBasicConfig resolves the path for I/O. The model and every
        // table identity below must still retain the authored reference.
        auto &model_assets = GET_MODULE(ModelAssetContainer);
        const auto &loaded =
            model_assets.getModelTemplateByName("tree");
        REQUIRE(loaded.authored_asset_reference ==
                "models/tree.glb");
        REQUIRE(loaded.material_primitives.size() == 4);

        std::array<std::uint64_t, 4> geometry_by_mesh{};
        std::array<std::uint32_t, 4> presence_by_mesh{};
        std::array<GlobalMaterialId, 4> material_by_mesh{};
        material_by_mesh.fill(invalidMaterialId());
        for (const auto &group : loaded.material_primitives) {
            REQUIRE(group.primitives.size() == 1);
            const auto &primitive = group.primitives.front();
            REQUIRE(primitive.mesh_index <
                    geometry_by_mesh.size());
            const auto mesh = primitive.mesh_index;
            REQUIRE(geometry_by_mesh[mesh] == 0);
            geometry_by_mesh[mesh] =
                primitive.geometry_allocation_id;
            presence_by_mesh[mesh] =
                primitive.ray_attribute_presence;
            material_by_mesh[mesh] = group.material;
        }
        for (std::size_t mesh = 0; mesh < 4; ++mesh) {
            CAPTURE(mesh);
            REQUIRE(geometry_by_mesh[mesh] != 0);
            REQUIRE(isValidMaterialId(
                material_by_mesh[mesh]));
        }
        constexpr auto all_attributes =
            rayGeometryAttributeNormal |
            rayGeometryAttributeUv0 |
            rayGeometryAttributeColor0;
        CHECK(presence_by_mesh[0] == 0);
        CHECK(presence_by_mesh[1] == all_attributes);
        CHECK(presence_by_mesh[2] == all_attributes);
        CHECK(presence_by_mesh[3] == all_attributes);

        auto &materials = GET_MODULE(MaterialContainer);
        CHECK(materials.materialDocumentReference(
                  material_by_mesh[0]).empty());
        CHECK(materials.materialDocumentReference(
                  material_by_mesh[1]).empty());
        CHECK(materials.materialDocumentReference(
                  material_by_mesh[2]) ==
              "materials/forest.material.json");
        CHECK(materials.materialName(material_by_mesh[2]) ==
              "ProjectOak");
        CHECK(materials.materialDocumentReference(
                  material_by_mesh[3]) ==
              "materials/forest.material.json");
        CHECK(materials.materialName(material_by_mesh[3]) ==
              "ProjectPine");

        const auto source_for = [&](GlobalMaterialId material,
                                    std::string key) {
            return materials.rayMaterialTableSource(
                material, std::move(key),
                materials.rayMaterialGeneration(material),
                materials.rayMaterialLease(material));
        };
        const auto embedded_red = source_for(
            material_by_mesh[0],
            "asset:models/tree.glb#0");
        const auto embedded_green = source_for(
            material_by_mesh[1],
            "asset:models/tree.glb#1");
        CHECK(embedded_red.base_color_texture_identity ==
              "asset:models/tree.glb#texture:gltf-texture/0");
        CHECK(embedded_green.base_color_texture_identity ==
              "asset:models/tree.glb#texture:gltf-texture/1");

        // Reverting full source-index provenance makes both mutations pass.
        CHECK_THROWS_WITH(
            source_for(material_by_mesh[0],
                       "asset:models/tree.glb#1"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_material_key_mismatch@1"));
        CHECK_THROWS_WITH(
            source_for(material_by_mesh[1],
                       "asset:models/tree.glb#0"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_material_key_mismatch@1"));

        // These factors and names came through real project material and
        // primitive-binding documents.
        const auto project_oak = source_for(
            material_by_mesh[2],
            "document:materials/forest.material.json#ProjectOak");
        const auto project_pine = source_for(
            material_by_mesh[3],
            "document:materials/forest.material.json#ProjectPine");
        CHECK(project_oak.base_color_factor[0] == 0.25F);
        CHECK(project_pine.base_color_factor[0] == 0.75F);
        CHECK_THROWS_WITH(
            source_for(
                material_by_mesh[2],
                "document:materials/forest.material.json#ProjectPine"),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_material_key_mismatch@1"));

        auto &instances =
            GET_MODULE(PolygonInstanceContainer);
        (void)instances.placeModelInstance(loaded);

        // Initialize a genuinely absent layout. This is the ShaderLibrary
        // registration baseline that active RT2b must later restore.
        auto &renderer = GET_MODULE(Renderer);
        renderer.render();
        vkcore.waitIdle();
        CHECK_THROWS(RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{}));
        auto &shaders = GET_MODULE(ShaderLibrary);
        const auto registration_baseline =
            shaders.checkpointRegistrations();
        const auto baseline_count =
            shaders.registrationCount();
        auto &pipelines = GET_MODULE(PipelineFactory);
        const auto pipeline_baseline =
            pipelines.checkpointRegistrations();

        // Mutate the real runtime fact consumed by production pipeline
        // creation. The exact set 0-3 layout requires 1026 sampled images;
        // rejection must happen before a descriptor-set-layout Vulkan object
        // is created. Restoring only the old static 1024 check, omitting set 2,
        // or moving validation after object creation breaks this contrast.
        {
            auto &mutable_capabilities =
                const_cast<VulkanRuntimeCapabilities &>(capabilities);
            struct RestoreSampledImageLimit {
                VulkanRuntimeCapabilities &capabilities;
                std::uint32_t value;
                ~RestoreSampledImageLimit() {
                    capabilities.max_descriptor_set_sampled_images = value;
                }
            } restore{
                mutable_capabilities,
                mutable_capabilities.max_descriptor_set_sampled_images};
            mutable_capabilities.max_descriptor_set_sampled_images = 1025;
            REQUIRE_THROWS_WITH(
                loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                    active_config),
                Catch::Matchers::ContainsSubstring(
                    "maxDescriptorSetSampledImages") &&
                    Catch::Matchers::ContainsSubstring("required>=1026") &&
                    Catch::Matchers::ContainsSubstring("actual=1025") &&
                    Catch::Matchers::ContainsSubstring(
                        capabilities.device_name));
        }
        const auto failed_pipeline_checkpoint =
            pipelines.checkpointRegistrations();
        CHECK(failed_pipeline_checkpoint.pipeline_count ==
              pipeline_baseline.pipeline_count);
        CHECK(failed_pipeline_checkpoint.descriptor_set_layout_keys.size() ==
              pipeline_baseline.descriptor_set_layout_keys.size());
        CHECK(failed_pipeline_checkpoint
                  .descriptor_set_layout_creation_count ==
              pipeline_baseline.descriptor_set_layout_creation_count);
        CHECK(shaders.registrationCount() == baseline_count);
        CHECK(shaders.registrationsSince(
                  registration_baseline).empty());
        CHECK_THROWS(RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{}));

        std::vector<RayTracingSbtWriteObservation> writes;
        const ScopedRayTracingSbtWriteObserver observer{
            [&](const auto &write) {
                writes.push_back(write);
            }};
        const auto active_variant =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                active_config);
        REQUIRE(isValidRenderingPassId(active_variant.flat));
        REQUIRE_FALSE(shaders.registrationsSince(
                          registration_baseline).empty());
        REQUIRE(shaders.registrationCount() >
                baseline_count);

        renderer.render();
        vkcore.waitIdle();

        auto &frame_resources = GET_MODULE(FrameResources);
        auto tables_lease =
            frame_resources
                .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(tables_lease != nullptr);
        const auto &tables = *tables_lease;
        REQUIRE(tables.row_snapshot != nullptr);
        REQUIRE(tables.geometry_snapshot != nullptr);
        REQUIRE(tables.material_table_buffer != nullptr);
        REQUIRE(tables.geometry_table_buffer != nullptr);
        CHECK(tables.ray_table_descriptor_pool.get() !=
              vk::DescriptorPool{});
        CHECK(tables.ray_table_descriptor_set.get() !=
              vk::DescriptorSet{});

        const std::vector<std::string> expected_material_keys{
            "asset:models/tree.glb#0",
            "asset:models/tree.glb#1",
            "document:materials/forest.material.json#ProjectOak",
            "document:materials/forest.material.json#ProjectPine",
        };
        CHECK(tables.ray_tables.material_keys ==
              expected_material_keys);
        REQUIRE(tables.ray_tables.textures.size() == 2);
        CHECK(tables.ray_tables.textures[0].canonical_identity ==
              "asset:models/tree.glb#texture:gltf-texture/0");
        CHECK(tables.ray_tables.textures[1].canonical_identity ==
              "asset:models/tree.glb#texture:gltf-texture/1");
        CHECK(tables.ray_tables.textures[0].srgb_view !=
              tables.ray_tables.textures[1].srgb_view);

        const auto index_of = [](const auto &values,
                                 const auto &needle) {
            const auto found =
                std::ranges::find(values, needle);
            if (found == values.end()) {
                throw std::out_of_range(
                    "WP368 expected table identity is absent");
            }
            return static_cast<std::uint32_t>(
                std::distance(values.begin(), found));
        };
        std::array<std::uint32_t, 4> material_index{};
        std::array<std::uint32_t, 4> geometry_index{};
        for (std::size_t lane = 0; lane < 4; ++lane) {
            material_index[lane] = index_of(
                tables.ray_tables.material_keys,
                expected_material_keys[lane]);
            geometry_index[lane] = index_of(
                tables.ray_tables.geometry_allocation_ids,
                geometry_by_mesh[lane]);
        }
        CHECK(tables.ray_tables.geometry_rows[
                  geometry_index[0]].attribute_presence == 0);
        CHECK(tables.ray_tables.geometry_rows[
                  geometry_index[1]].attribute_presence ==
              all_attributes);

        {
            const auto runtime =
                GET_MODULE(FrameGraphRuntimeContainer).snapshot();
            REQUIRE(runtime != nullptr);
            REQUIRE(runtime->gpu_arena != nullptr);
            const auto owner_scope = std::find_if(
                runtime->gpu_arena->scopes.begin(),
                runtime->gpu_arena->scopes.end(),
                [](const auto &scope) {
                    return scope.ray_tracing_project != nullptr;
                });
            REQUIRE(owner_scope !=
                    runtime->gpu_arena->scopes.end());
            REQUIRE(owner_scope->acceleration_structures !=
                    nullptr);
            CHECK(owner_scope->acceleration_structures
                      ->activeRaySceneRowsForTesting() ==
                  tables.row_snapshot.get());
            CHECK(owner_scope->acceleration_structures
                      ->activeGeometrySnapshotForTesting() ==
                  tables.geometry_snapshot.get());
        }

        const auto pixel = [](const RenderTargetReadback &readback,
                              std::uint32_t x) {
            std::array<std::uint32_t, 4> result{};
            const auto offset =
                static_cast<std::size_t>(x) *
                sizeof(result);
            if (offset + sizeof(result) >
                readback.bytes.size()) {
                throw std::out_of_range(
                    "WP368 sentinel pixel is outside readback");
            }
            std::memcpy(
                result.data(),
                readback.bytes.data() + offset,
                sizeof(result));
            return result;
        };
        const auto primary =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto geometry =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_b", ImageSubresourceRange{});

        const std::array<std::array<std::uint32_t, 4>, 4>
            primary_expected{{
                {material_index[0], geometry_index[0],
                 std::bit_cast<std::uint32_t>(1.0F),
                 std::bit_cast<std::uint32_t>(1.0F)},
                {material_index[1], geometry_index[1],
                 std::bit_cast<std::uint32_t>(1.0F),
                 std::bit_cast<std::uint32_t>(0.0F)},
                {material_index[2], geometry_index[2],
                 std::bit_cast<std::uint32_t>(0.25F),
                 std::bit_cast<std::uint32_t>(0.25F)},
                {material_index[3], geometry_index[3],
                 std::bit_cast<std::uint32_t>(0.75F),
                 std::bit_cast<std::uint32_t>(0.75F)},
            }};
        const std::array<std::array<std::uint32_t, 4>, 4>
            geometry_expected{{
                {std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(1.0F), 0},
                {std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(0.0F),
                 all_attributes},
                {std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(0.0F),
                 all_attributes},
                {std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(0.0F),
                 std::bit_cast<std::uint32_t>(0.0F),
                 all_attributes},
            }};
        for (std::uint32_t lane = 0; lane < 4; ++lane) {
            CAPTURE(lane);
            CHECK(pixel(primary, lane) ==
                  primary_expected[lane]);
            CHECK(pixel(geometry, lane) ==
                  geometry_expected[lane]);
        }

        REQUIRE(writes.size() == 2);
        const auto alignment =
            capabilities.shader_group_handle_alignment;
        REQUIRE(alignment != 0);
        const auto data_start =
            (capabilities.shader_group_handle_size +
             alignment - 1u) /
            alignment * alignment;
        const auto flags_for =
            [](std::string_view key) -> std::uint32_t {
            if (key == "asset:models/tree.glb#0")
                return 101;
            if (key == "asset:models/tree.glb#1")
                return 202;
            if (key ==
                "document:materials/forest.material.json#ProjectOak")
                return 303;
            if (key ==
                "document:materials/forest.material.json#ProjectPine")
                return 303;
            throw std::out_of_range(
                "WP368 SBT material key is unexpected");
        };
        for (const auto &write : writes) {
            CHECK(write.row_digest_sha256 ==
                  tables.row_snapshot
                      ->identity_digest_sha256);
            REQUIRE(write.record_index_stride == 2);
            REQUIRE(write.record_count == 8);
            for (const auto &row :
                 tables.row_snapshot->rows) {
                for (std::uint32_t ray = 0;
                     ray < 2; ++ray) {
                    const auto at =
                        static_cast<std::size_t>(
                            row.q * 2u + ray) *
                            write.record_byte_stride +
                        data_start;
                    REQUIRE(at + 36 <=
                            write.hit_bytes.size());
                    std::array<std::uint32_t, 2>
                        engine{};
                    std::uint32_t flags = 0;
                    std::memcpy(
                        engine.data(),
                        write.hit_bytes.data() + at,
                        sizeof(engine));
                    std::memcpy(
                        &flags,
                        write.hit_bytes.data() + at + 32,
                        sizeof(flags));
                    CHECK(engine[0] ==
                          row.material_index);
                    CHECK(engine[1] ==
                          row.geometry_index);
                    CHECK(flags ==
                          flags_for(
                              row.material_reference));
                }
            }
        }

        const auto row_for =
            [&](std::string_view key)
                -> const RaySceneRow & {
            const auto found = std::ranges::find(
                tables.row_snapshot->rows, key,
                &RaySceneRow::material_reference);
            if (found ==
                tables.row_snapshot->rows.end()) {
                throw std::out_of_range(
                    "WP368 scene row is absent");
            }
            return *found;
        };
        const auto red_q =
            row_for(expected_material_keys[0]).q;
        const auto green_q =
            row_for(expected_material_keys[1]).q;
        REQUIRE(red_q != green_q);

        // Mutate only final TLAS record-offset routing. The same table and
        // shaders must now select the other genuinely different 1x1 image.
        {
            const ScopedRayTracingTlasRecordOffsetOverrideForTesting
                swapped_rows{
                    [red_q, green_q](
                        std::uint32_t q,
                        std::uint32_t) {
                        if (q == red_q)
                            return green_q * 2u;
                        if (q == green_q)
                            return red_q * 2u;
                        return q * 2u;
                    }};
            renderer.render();
            vkcore.waitIdle();
        }
        const auto wrong_primary =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_a", ImageSubresourceRange{});
        const auto wrong_geometry =
            RendererTestAccess::readRenderTarget(renderer,
                "wp366_b", ImageSubresourceRange{});
        CHECK(pixel(wrong_primary, 0) ==
              primary_expected[1]);
        CHECK(pixel(wrong_primary, 1) ==
              primary_expected[0]);
        CHECK(pixel(wrong_geometry, 0) ==
              geometry_expected[1]);
        CHECK(pixel(wrong_geometry, 1) ==
              geometry_expected[0]);
        CHECK(wrong_primary.bytes != primary.bytes);
        CHECK(wrong_geometry.bytes != geometry.bytes);

        renderer.render();
        vkcore.waitIdle();
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_a",
                  ImageSubresourceRange{}).bytes ==
              primary.bytes);
        CHECK(RendererTestAccess::readRenderTarget(renderer,
                  "wp366_b",
                  ImageSubresourceRange{}).bytes ==
              geometry.bytes);

        tables_lease.reset();
        auto retiring =
            frame_resources
                .rayTracingSceneShaderBindingTablesLeaseForTesting();
        REQUIRE(retiring != nullptr);
        std::weak_ptr<
            const RayTracingSceneShaderBindingTables>
            retired_tables = retiring;
        retiring.reset();

        const auto purged_variant =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                inactive_config);
        REQUIRE(isValidRenderingPassId(
            purged_variant.flat));
        for (std::size_t frame = 0; frame < 3;
             ++frame) {
            renderer.render();
            vkcore.waitIdle();
        }
        CHECK(frame_resources
                  .rayTracingSceneShaderBindingTables() ==
              nullptr);
        CHECK(retired_tables.expired());
        CHECK_THROWS(RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{}));
        CHECK(shaders.registrationCount() ==
              baseline_count);
        CHECK(shaders.registrationsSince(
                  registration_baseline).empty());

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error,
            "Vulkan WP368 production project fixture unavailable");
        throw;
    }
#else
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        writeRt2bTableProject(temp_dir);
        const auto authored_root = Pelican::TestSupport::renderGraphRoot(
            nlohmann::json{
                {"ray_group_layout", "ray_layout.json"},
                {"ray_bindings", "ray_bindings.json"},
            });
        std::size_t document_loads = 0;
        CHECK_THROWS_WITH(
            loadRayTracingProjectConfig(
                authored_root,
                [&](std::string_view reference) {
                    ++document_loads;
                    return readTextFile(
                        temp_dir /
                        std::filesystem::path{
                            std::string{reference}});
                },
                false),
            Catch::Matchers::ContainsSubstring(
                "pelican.ray_tracing.layout_requires_runtime_compiler@1"));
        CHECK(document_loads == 0);
        std::filesystem::remove_all(temp_dir);
    } catch (...) {
        std::filesystem::remove_all(temp_dir);
        throw;
    }
#endif
}

TEST_CASE(
    "WP367 production anyhit A B proves integer predicates occurrence wire SBT and reload",
    "[WP367][headless][gpu][ray-tracing][rt2a][oracle][reload][wire]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeRt2aAnyHitProject(temp_dir);
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }

        CommonPolygonVertData geometry;
        geometry.indices = {0, 1, 2};
        geometry.pos = {{-0.75f, -0.75f, 0.0f},
                        {0.75f, -0.75f, 0.0f},
                        {0.0f, 0.75f, 0.0f}};
        geometry.normal.assign(3, glm::vec3{0.0f, 0.0f, 1.0f});
        geometry.texcoord.assign(3, glm::vec2{0.0f});
        geometry.color.assign(3, glm::vec4{1.0f});
        const auto primitive =
            GET_MODULE(VertBufContainer).addPrimitiveEntry(
                std::move(geometry));
        const auto material =
            GET_MODULE(StandardMaterialResource)
                .standardTransparentMaterial();
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto place = [&](std::uint64_t asset_id,
                               std::string authored_reference,
                               glm::vec3 translation) {
            ModelTemplate model;
            model.asset_id = ModelAssetId{asset_id};
            model.authored_asset_reference =
                std::move(authored_reference);
            model.material_primitives = {
                ModelTemplate::MaterialPrimitives{
                    .material = material,
                    .primitives = {primitive},
                    .source_material_index = 0,
                }};
            const auto placed = instances.placeModelInstance(model);
            instances.setTrs(
                placed, translation,
                glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                glm::vec3{1.0f});
            return placed;
        };
        const auto single_instance = place(
            36701, "models/wp367_single.glb", {-2.0f, 0.0f, 8.0f});
        const auto front_instance = place(
            36702, "models/wp367_front.glb", {2.0f, 0.0f, 8.0f});
        const auto back_instance = place(
            36703, "models/wp367_back.glb", {2.0f, 0.0f, 16.0f});
        CHECK(instances.isModelInstanceAlive(single_instance));
        CHECK(instances.isModelInstanceAlive(front_instance));
        CHECK(instances.isModelInstanceAlive(back_instance));

        RayTracingProjectConfig project{
            .layout = parseRayGroupLayout(
                readTextFile(temp_dir / "ray_layout.json"),
                "ray_layout.json"),
        };
        project.bindings = parseRayBindings(
            readTextFile(temp_dir / "ray_bindings.json"),
            project.layout, "ray_bindings.json");
        const auto snapshots = instances.rayQueryGeometryInstances();
        const auto rows = resolveRayTracingSceneRows(snapshots, project);
        REQUIRE(rows.rows.size() == 3);
        // MG-2a: layout v1 wrote q into the instance custom index, and this
        // test read q back through it. A version 3 row carries the layout's
        // default custom index, so rows are told apart on the GPU by their
        // u32 record, and the predicates match the custom index they have.
        struct RowIdentity {
            std::uint32_t q = 0;
            std::uint32_t custom_index = 0;
            std::uint32_t record = 0;
        };
        const auto row_for = [&](std::string_view authored_reference) {
            const auto row = std::find_if(
                rows.rows.begin(), rows.rows.end(), [&](const auto &candidate) {
                    return snapshots.at(candidate.source_index)
                               .material_asset_reference == authored_reference;
                });
            if (row == rows.rows.end()) {
                throw std::runtime_error(
                    "WP367 row snapshot lost an authored model reference");
            }
            if (row->record_data.size() < sizeof(std::uint32_t)) {
                throw std::runtime_error(
                    "WP367 row snapshot lost its u32 record");
            }
            RowIdentity result{.q = row->q,
                               .custom_index = row->instance_custom_index};
            std::memcpy(&result.record, row->record_data.data(),
                        sizeof(result.record));
            return result;
        };
        const auto single = row_for("models/wp367_single.glb");
        const auto front = row_for("models/wp367_front.glb");
        const auto back = row_for("models/wp367_back.glb");
        CHECK(single.record == wp367SingleRecord);
        CHECK(front.record == wp367FrontRecord);
        CHECK(back.record == wp367BackRecord);
        CHECK(single.q != front.q);
        CHECK(single.q != back.q);
        CHECK(front.q != back.q);

        const auto single_predicate =
            RayTracingAnyHitIntegerPredicate::acceptMatching({
                .primitive_id = 0,
                .instance_custom_index = single.custom_index,
                .record_u32 = single.record,
            });
        const auto overlap_predicate =
            RayTracingAnyHitIntegerPredicate::ignoreMatching({
                .primitive_id = 0,
                .instance_custom_index = front.custom_index,
                .record_u32 = front.record,
            });
        const auto always_predicate =
            RayTracingAnyHitIntegerPredicate::alwaysAccept();
        writeWp367ActualAnyHitShaders(
            temp_dir, single_predicate, overlap_predicate);

        std::vector<RayTracingPipelineCreateObservation> creates;
        std::vector<RayTracingSbtWriteObservation> writes;
        const ScopedRayTracingPipelineCreateObserver create_observer{
            [&](const auto &observation) { creates.push_back(observation); }};
        const ScopedRayTracingSbtWriteObserver sbt_observer{
            [&](const auto &observation) { writes.push_back(observation); }};
        auto &renderer = GET_MODULE(Renderer);
        REQUIRE(creates.size() == 2);
        for (const auto &create : creates) {
            REQUIRE(create.hit_group_any_hit_shaders.size() == 6);
            CHECK(std::count_if(
                      create.hit_group_any_hit_shaders.begin(),
                      create.hit_group_any_hit_shaders.end(),
                      [](const auto index) {
                          return index != VK_SHADER_UNUSED_KHR;
                      }) == 3);
        }

        renderer.render();
        vkcore.waitIdle();
        const auto actual = RendererTestAccess::readRenderTarget(renderer,
            "wp367_actual", ImageSubresourceRange{});
        const auto always = RendererTestAccess::readRenderTarget(renderer,
            "wp367_always", ImageSubresourceRange{});
        REQUIRE(actual.format == vk::Format::eR32G32B32A32Uint);
        REQUIRE(always.format == actual.format);
        const auto pixel = [](const RenderTargetReadback &readback,
                              std::uint32_t x) {
            std::array<std::uint32_t, 4> result{};
            const auto offset = static_cast<std::size_t>(x) * sizeof(result);
            if (offset + sizeof(result) > readback.bytes.size()) {
                throw std::out_of_range(
                    "WP367 sentinel pixel is outside readback");
            }
            std::memcpy(result.data(), readback.bytes.data() + offset,
                        sizeof(result));
            return result;
        };
        const auto actual_single = pixel(actual, 0);
        const auto always_single = pixel(always, 0);
        const auto actual_overlap = pixel(actual, 1);
        const auto always_overlap = pixel(always, 1);
        CHECK(actual_single[0] == single.custom_index);
        CHECK(always_single[0] == single.custom_index);
        CHECK(actual_single[2] == single.record);
        CHECK(always_single[2] == single.record);
        CHECK(actual_single[3] == wp367MarkerProof(
                                      wp367ShadowRayMarker,
                                      wp367SingleGroup));
        CHECK(always_single[3] == wp367MarkerProof(
                                      wp367ShadowRayMarker,
                                      wp367SingleGroup));
        CHECK(actual_overlap[0] == back.custom_index);
        CHECK(always_overlap[0] == front.custom_index);
        CHECK(actual_overlap[2] == back.record);
        CHECK(always_overlap[2] == front.record);
        CHECK(actual_overlap[3] == wp367MarkerProof(
                                       wp367ShadowRayMarker,
                                       wp367BackGroup));
        CHECK(always_overlap[3] == wp367MarkerProof(
                                       wp367ShadowRayMarker,
                                       wp367FrontGroup));
        CHECK(pixel(actual, 2) ==
              std::array{0xffffffffu, 0u, 0x6ffu, 0x5ffu});
        CHECK(pixel(always, 2) ==
              std::array{0xffffffffu, 0u, 0x6ffu, 0x5ffu});

        const auto oracle_triangle = [](
            double x, double z, const RowIdentity &row) {
            return RayTracingOracleTriangle{
                .vertices = {
                    RayTracingOracleVec3{x - 0.75, -0.75, z},
                    RayTracingOracleVec3{x + 0.75, -0.75, z},
                    RayTracingOracleVec3{x, 0.75, z},
                },
                .record_id = row.record,
                .primitive_id = 0,
                .instance_custom_index = row.custom_index,
                .record_u32 = row.record,
            };
        };
        const RayTracingOracleRay single_ray{
            .origin = {-2.0, 0.0, 0.0},
            .direction = {0.0, 0.0, 1.0},
        };
        const std::array single_triangles{
            oracle_triangle(-2.0, 8.0, single)};
        const auto cpu_single_actual = traceRayTracingOracle(
            single_ray, single_triangles, single_predicate);
        const auto cpu_single_always = traceRayTracingOracle(
            single_ray, single_triangles, always_predicate);
        REQUIRE(cpu_single_actual.hit);
        REQUIRE(cpu_single_always.hit);
        CHECK(rayTracingOracleContainsGpuHit(
            *cpu_single_actual.hit,
            std::bit_cast<float>(actual_single[1]), actual_single[2]));
        CHECK(rayTracingOracleContainsGpuHit(
            *cpu_single_always.hit,
            std::bit_cast<float>(always_single[1]), always_single[2]));

        const RayTracingOracleRay overlap_ray{
            .origin = {2.0, 0.0, 0.0},
            .direction = {0.0, 0.0, 1.0},
        };
        const std::array overlap_triangles{
            oracle_triangle(2.0, 8.0, front),
            oracle_triangle(2.0, 16.0, back),
        };
        const auto cpu_overlap_actual = traceRayTracingOracle(
            overlap_ray, overlap_triangles, overlap_predicate);
        const auto cpu_overlap_always = traceRayTracingOracle(
            overlap_ray, overlap_triangles, always_predicate);
        REQUIRE(cpu_overlap_actual.hit);
        REQUIRE(cpu_overlap_always.hit);
        CHECK(rayTracingOracleContainsGpuHit(
            *cpu_overlap_actual.hit,
            std::bit_cast<float>(actual_overlap[1]), actual_overlap[2]));
        CHECK(rayTracingOracleContainsGpuHit(
            *cpu_overlap_always.hit,
            std::bit_cast<float>(always_overlap[1]), always_overlap[2]));

        REQUIRE(writes.size() == 2);
        CHECK(writes[0].row_digest_sha256 ==
              writes[1].row_digest_sha256);
        CHECK(writes[0].pipeline_handle != writes[1].pipeline_handle);
        const auto &capabilities = vkcore.getRuntimeCapabilities();
        const auto alignment = capabilities.shader_group_handle_alignment;
        REQUIRE(alignment != 0);
        const auto data_start =
            (capabilities.shader_group_handle_size + alignment - 1u) /
            alignment * alignment;
        std::array<std::uint32_t, 3> marker_by_q{};
        std::array<std::uint32_t, 3> group_marker_by_q{};
        marker_by_q[single.q] = single.record;
        marker_by_q[front.q] = front.record;
        marker_by_q[back.q] = back.record;
        group_marker_by_q[single.q] = wp367SingleGroup;
        group_marker_by_q[front.q] = wp367FrontGroup;
        group_marker_by_q[back.q] = wp367BackGroup;
        for (const auto &write : writes) {
            REQUIRE(write.record_index_stride == 2);
            REQUIRE(write.record_count == 6);
            for (std::uint32_t q = 0; q < 3; ++q) {
                for (std::uint32_t r = 0; r < 2; ++r) {
                    const auto at =
                        static_cast<std::size_t>(q * 2u + r) *
                            write.record_byte_stride +
                        data_start;
                    std::array<std::uint32_t, 3> record{};
                    REQUIRE(at + sizeof(record) <= write.hit_bytes.size());
                    std::memcpy(record.data(), write.hit_bytes.data() + at,
                                sizeof(record));
                    CHECK(record[0] ==
                          (r == 1u ? marker_by_q[q] : 0u));
                    CHECK(record[1] ==
                          (r == 1u ? wp367ShadowRayMarker : 0u));
                    CHECK(record[2] ==
                          (r == 1u ? group_marker_by_q[q] : 0u));
                }
            }
        }

        const auto plan = renderer.currentFramePlanJson();
        const auto check_occurrences = [&](std::string_view lane) {
            const auto name = "wp367_" + std::string{lane};
            const auto node = std::find_if(
                plan.at("nodes").begin(), plan.at("nodes").end(),
                [&](const auto &candidate) {
                    return candidate.value("name", std::string{}) == name;
                });
            REQUIRE(node != plan.at("nodes").end());
            const auto &resolution = node->at("shader_resolution");
            REQUIRE(resolution.at("hit_slot_count") == 6);
            const auto &closesthit_slots =
                resolution.at("closesthit_slots");
            REQUIRE(closesthit_slots.size() == 3);
            REQUIRE(resolution.contains("anyhit_slots"));
            const auto &slots = resolution.at("anyhit_slots");
            REQUIRE(slots.size() == 3);
            constexpr std::array<std::size_t, 3> expected_slots{1, 3, 5};
            constexpr std::array<std::string_view, 3> groups{
                "single", "front", "back"};
            for (std::size_t index = 0; index < slots.size(); ++index) {
                CHECK(closesthit_slots.at(index).at("hit_slot") ==
                      expected_slots[index]);
                CHECK(closesthit_slots.at(index).at("stage") ==
                      "closesthit");
                CHECK(slots.at(index).at("hit_slot") ==
                      expected_slots[index]);
                CHECK(slots.at(index).at("stage") == "anyhit");
                CHECK(slots.at(index).at("declared_ref") ==
                      "project://shaders/wp367_" + std::string{lane} +
                          "_" + std::string{groups[index]});
                for (const auto forbidden :
                     {"classifier", "golden_portable", "subset_certified",
                      "reason", "reasons"}) {
                    CHECK_FALSE(slots.at(index).contains(forbidden));
                }
            }
        };
        check_occurrences("actual");
        check_occurrences("always");

        auto &reload_service = GET_MODULE(watch::ReloadService);
        const auto single_key =
            watch::makeAssetKey("shaders/wp367_actual_single.rahit");
        const auto front_key =
            watch::makeAssetKey("shaders/wp367_actual_front.rahit");
        REQUIRE(GET_MODULE(ShaderLibrary).handlesReload(single_key));
        REQUIRE(GET_MODULE(ShaderLibrary).handlesReload(front_key));

        struct SingleNegativeControl {
            std::string input;
            RayTracingAnyHitIntegerPredicate predicate;
            std::uint32_t ray_marker = wp367ShadowRayMarker;
            std::uint32_t group_marker = wp367SingleGroup;
        };
        const std::array single_negative_controls{
            SingleNegativeControl{
                "primitive_id",
                RayTracingAnyHitIntegerPredicate::acceptMatching({
                    .primitive_id = 1,
                    .instance_custom_index = single.custom_index,
                    .record_u32 = single.record,
                })},
            SingleNegativeControl{
                "instance_custom_index",
                RayTracingAnyHitIntegerPredicate::acceptMatching({
                    .primitive_id = 0,
                    .instance_custom_index = single.custom_index + 1u,
                    .record_u32 = single.record,
                })},
            SingleNegativeControl{
                "record_u32",
                RayTracingAnyHitIntegerPredicate::acceptMatching({
                    .primitive_id = 0,
                    .instance_custom_index = single.custom_index,
                    .record_u32 = single.record + 1u,
                })},
            SingleNegativeControl{
                "record_ray_marker",
                single_predicate,
                wp367ShadowRayMarker + 1u,
                wp367SingleGroup},
            SingleNegativeControl{
                "record_group_marker",
                single_predicate,
                wp367ShadowRayMarker,
                wp367SingleGroup + 1u},
        };
        const auto miss_pixel = pixel(actual, 2);
        std::uint64_t reload_sequence = 1;
        for (const auto &control : single_negative_controls) {
            CAPTURE(control.input, control.ray_marker,
                    control.group_marker);
            writeWp367AnyHitShader(
                temp_dir, "wp367_actual_single", control.predicate,
                control.ray_marker, control.group_marker);
            const auto creates_before_control = creates.size();
            REQUIRE(reload_service.applyRequestForTesting(
                watch::ReloadRequest{
                    single_key, watch::ReloadKind::modified, {},
                    reload_sequence++}));
            REQUIRE(creates.size() > creates_before_control);
            renderer.render();
            vkcore.waitIdle();
            const auto rejected = RendererTestAccess::readRenderTarget(renderer,
                "wp367_actual", ImageSubresourceRange{});
            const auto unchanged_always =
                RendererTestAccess::readRenderTarget(renderer,
                    "wp367_always", ImageSubresourceRange{});
            CHECK(pixel(rejected, 0) == miss_pixel);
            CHECK(unchanged_always.bytes == always.bytes);
        }

        // Restore the exact AST predicate and both record marker guards.  A
        // hit now proves that the negative results above were caused by the
        // one changed input, not by a stale or failed reload.
        writeWp367AnyHitShader(
            temp_dir, "wp367_actual_single", single_predicate,
            wp367ShadowRayMarker, wp367SingleGroup);
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{
                single_key, watch::ReloadKind::modified, {},
                reload_sequence++}));
        renderer.render();
        vkcore.waitIdle();
        const auto restored = RendererTestAccess::readRenderTarget(renderer,
            "wp367_actual", ImageSubresourceRange{});
        const auto restored_single = pixel(restored, 0);
        CHECK(restored_single[0] == single.custom_index);
        CHECK(restored_single[2] == single.record);
        CHECK(restored_single[3] == wp367MarkerProof(
                                         wp367ShadowRayMarker,
                                         wp367SingleGroup));
        CHECK(rayTracingOracleContainsGpuHit(
            *cpu_single_actual.hit,
            std::bit_cast<float>(restored_single[1]),
            restored_single[2]));

        writeWp367AnyHitShader(
            temp_dir, "wp367_actual_front", always_predicate,
            wp367ShadowRayMarker, wp367FrontGroup);
        const auto creates_before_reload = creates.size();
        REQUIRE(reload_service.applyRequestForTesting(
            watch::ReloadRequest{front_key, watch::ReloadKind::modified,
                                 {}, reload_sequence++}));
        REQUIRE(creates.size() > creates_before_reload);
        CHECK(std::count_if(
                  creates.back().hit_group_any_hit_shaders.begin(),
                  creates.back().hit_group_any_hit_shaders.end(),
                  [](const auto index) {
                      return index != VK_SHADER_UNUSED_KHR;
                  }) == 3);

        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        const auto reloaded_actual = RendererTestAccess::readRenderTarget(renderer,
            "wp367_actual", ImageSubresourceRange{});
        const auto reloaded_always = RendererTestAccess::readRenderTarget(renderer,
            "wp367_always", ImageSubresourceRange{});
        const auto reloaded_front = pixel(reloaded_actual, 1);
        CHECK(reloaded_front[0] == front.custom_index);
        CHECK(reloaded_front[2] == front.record);
        CHECK(reloaded_front[3] == wp367MarkerProof(
                                        wp367ShadowRayMarker,
                                        wp367FrontGroup));
        CHECK(rayTracingOracleContainsGpuHit(
            *cpu_overlap_always.hit,
            std::bit_cast<float>(reloaded_front[1]), reloaded_front[2]));
        CHECK(reloaded_always.bytes == always.bytes);
        REQUIRE(writes.size() == 2);
        CHECK(writes[0].row_digest_sha256 == rows.identity_digest_sha256);
        CHECK(writes[1].row_digest_sha256 == rows.identity_digest_sha256);

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan WP367 any-hit A/B fixture unavailable");
        throw;
    }
#else
    SUCCEED(
        "WP367 layout projects are rejected before GPU work when the runtime compiler is disabled");
#endif
}

TEST_CASE(
    "WP367 production load names every anyhit ABI reject before Vulkan pipeline creation",
    "[WP367][headless][gpu][ray-tracing][rt2a][abi][production][mutation]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    struct Case {
        Wp367NegativeAnyHitLoadKind kind;
        std::string_view code;
    };
    for (const auto test_case : {
             Case{Wp367NegativeAnyHitLoadKind::payload_mismatch,
                  "pelican.ray_tracing.payload_mismatch@1"},
             Case{Wp367NegativeAnyHitLoadKind::unreached_payload_mismatch,
                  "pelican.ray_tracing.payload_mismatch@1"},
             Case{Wp367NegativeAnyHitLoadKind::vec3_attribute,
                  "pelican.ray_tracing.hit_attribute_forbidden@1"},
             Case{Wp367NegativeAnyHitLoadKind::record_mismatch,
                  "pelican.ray_tracing.shader_record_abi_mismatch@1"},
             Case{Wp367NegativeAnyHitLoadKind::multiple_incoming_payloads,
                  "pelican.ray_tracing.interface_count@1"},
         }) {
        const auto temp_dir = makeTempProjectDir();
        try {
            FastModuleContainer modules;
            writeWp367NegativeAnyHitLoadProject(temp_dir, test_case.kind);
            configureRayQueryTestRuntime(temp_dir);
            auto &vkcore = GET_MODULE(VulkanManageCore);
            if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
                SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
            }

            std::size_t create_calls = 0;
            const ScopedRayTracingPipelineCreateObserver create_observer{
                [&](const auto &) { ++create_calls; }};
            std::string error_message;
            try {
                (void)GET_MODULE(Renderer);
            } catch (const std::exception &error) {
                error_message = error.what();
            }
            const std::string expected_code{test_case.code};
            CAPTURE(expected_code, error_message);
            CHECK(error_message.find(expected_code) != std::string::npos);
            CHECK(error_message.find("wp367_negative") !=
                  std::string::npos);
            CHECK(create_calls == 0);
            vkcore.waitIdle();
            std::filesystem::remove_all(temp_dir);
        } catch (const std::exception &error) {
            std::filesystem::remove_all(temp_dir);
            TestSupport::skipIfVulkanDeviceUnavailable(
                error, "Vulkan WP367 negative any-hit fixture unavailable");
            throw;
        }
    }
#else
    SUCCEED(
        "WP367 layout projects are rejected before GPU work when the runtime compiler is disabled");
#endif
}

namespace {

void writeWp374aRt1bV2Project(
    const std::filesystem::path &directory) {
    writeRt1bLayoutProject(directory);
    auto layout = nlohmann::json::parse(
        readTextFile(directory / "ray_layout.json"));
    layout["acceleration_structure"] = {
        {"defaults",
         {{"mask", 255},
          {"instance_flags", nlohmann::json::array()},
          {"geometry_flags", nlohmann::json::array()}}},
        {"rules", nlohmann::json::array()},
    };
    writeTextFile(directory / "ray_layout.json", layout.dump(2));

    auto bindings = nlohmann::json::parse(
        readTextFile(directory / "ray_bindings.json"));
    REQUIRE(bindings.at("bindings").size() == 2);
    bindings["bindings"] = nlohmann::json::array(
        {bindings.at("bindings").at(0)});
    writeTextFile(directory / "ray_bindings.json", bindings.dump(2));
}

void writeWp374aRt2aV2Project(
    const std::filesystem::path &directory,
    std::uint32_t geometry_flags) {
    writeRt2aAnyHitProject(directory);
    auto layout = nlohmann::json::parse(
        readTextFile(directory / "ray_layout.json"));
    auto encoded_flags = nlohmann::json::array();
    if ((geometry_flags & rayGeometryOpaque) != 0) {
        encoded_flags.push_back("opaque");
    }
    if ((geometry_flags & rayGeometryNoDuplicateAnyHitInvocation) != 0) {
        encoded_flags.push_back("no_duplicate_any_hit_invocation");
    }
    layout["acceleration_structure"] = {
        {"defaults",
         {{"mask", 255},
          {"instance_flags", nlohmann::json::array()},
          {"geometry_flags", nlohmann::json::array()}}},
        {"rules",
         nlohmann::json::array(
             {{{"material",
                {{"asset", "models/wp367_single.glb"},
                 {"source_material_index", 0}}},
               {"geometry_flags", std::move(encoded_flags)}}})},
    };
    writeTextFile(directory / "ray_layout.json", layout.dump(2));

    auto bindings = nlohmann::json::parse(
        readTextFile(directory / "ray_bindings.json"));
    REQUIRE(bindings.at("bindings").size() == 3);
    bindings["bindings"] = nlohmann::json::array(
        {bindings.at("bindings").at(0)});
    writeTextFile(directory / "ray_bindings.json", bindings.dump(2));
}

CommonPolygonVertData wp374aTriangle() {
    CommonPolygonVertData geometry;
    geometry.indices = {0, 1, 2};
    geometry.pos = {{-0.75f, -0.75f, 0.0f},
                    {0.75f, -0.75f, 0.0f},
                    {0.0f, 0.75f, 0.0f}};
    geometry.normal.assign(3, glm::vec3{0.0f, 0.0f, 1.0f});
    geometry.texcoord.assign(3, glm::vec2{0.0f});
    geometry.color.assign(3, glm::vec4{1.0f});
    return geometry;
}

ModelTemplate wp374aModel(
    std::uint64_t asset_id, std::string authored_reference,
    std::uint32_t source_material_index,
    const ModelTemplate::PrimitiveRefInfo &primitive) {
    ModelTemplate model;
    model.asset_id = ModelAssetId{asset_id};
    model.authored_asset_reference = std::move(authored_reference);
    model.material_primitives = {
        ModelTemplate::MaterialPrimitives{
            .material = GET_MODULE(StandardMaterialResource)
                            .standardTransparentMaterial(),
            .primitives = {primitive},
            .source_material_index = source_material_index,
        }};
    return model;
}

ModelInstanceId placeWp374aChair(
    PolygonInstanceContainer &instances, const ModelTemplate &model,
    float x) {
    const auto id = instances.placeModelInstance(model);
    instances.setTrs(id, {x, 0.0f, 8.0f},
                     glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                     glm::vec3{1.0f});
    return id;
}

std::array<std::uint32_t, 4> wp374aPixel(
    const RenderTargetReadback &readback, std::uint32_t x) {
    std::array<std::uint32_t, 4> result{};
    const auto offset = static_cast<std::size_t>(x) * sizeof(result);
    if (offset + sizeof(result) > readback.bytes.size()) {
        throw std::out_of_range("WP374a sentinel pixel is outside readback");
    }
    std::memcpy(result.data(), readback.bytes.data() + offset,
                sizeof(result));
    return result;
}

const RaySceneRowSnapshot &wp374aActiveRows() {
    const auto runtime = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    if (runtime == nullptr || runtime->gpu_arena == nullptr) {
        throw std::runtime_error("WP374a GPU owner is absent");
    }
    const auto owner = std::ranges::find_if(
        runtime->gpu_arena->scopes,
        [](const auto &scope) { return scope.ray_tracing_project != nullptr; });
    if (owner == runtime->gpu_arena->scopes.end() ||
        owner->acceleration_structures == nullptr) {
        throw std::runtime_error("WP374a AS owner is absent");
    }
    const auto *rows =
        owner->acceleration_structures->activeRaySceneRowsForTesting();
    if (rows == nullptr) {
        throw std::runtime_error("WP374a committed row snapshot is absent");
    }
    return *rows;
}

const RaySceneRow &wp374aRowForAuthor(
    const RaySceneRowSnapshot &rows, std::uint64_t authoring_object_id) {
    const auto row = std::ranges::find(
        rows.rows, authoring_object_id,
        &RaySceneRow::authoring_object_id);
    if (row == rows.rows.end()) {
        throw std::runtime_error("WP374a authored row is absent");
    }
    return *row;
}

constexpr std::uint32_t wp374aClosestProof(
    std::uint32_t ray_marker, std::uint32_t group_marker) {
    return 0xc3670000u ^ ((ray_marker << 16u) | group_marker);
}

} // namespace

TEST_CASE(
    "WP374a two same-material chairs honor instance mask and removal in GPU output",
    "[WP374a][WP374][headless][gpu][ray-tracing][rt3-1][mask][remove]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeWp374aRt1bV2Project(temp_dir);
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        auto primitive = GET_MODULE(VertBufContainer).addPrimitiveEntry(
            wp374aTriangle());
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        const auto model = wp374aModel(
            37401, "models/tree.glb", 2, primitive);
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto left = placeWp374aChair(instances, model, -2.0f);
        const auto right = placeWp374aChair(instances, model, 2.0f);
        instances.setRayInstanceDeclaration(left, 374011, std::nullopt);
        instances.setRayInstanceDeclaration(right, 374012, std::nullopt);

        auto &renderer = GET_MODULE(Renderer);
        renderer.render();
        vkcore.waitIdle();
        const auto visible = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        CHECK(wp374aPixel(visible, 0)[0] == 0xa0u);
        CHECK(wp374aPixel(visible, 1)[0] == 0xa0u);
        const auto visible_diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);

        instances.setRayInstanceDeclaration(
            right, 374012, RayInstanceComponent{.mask = std::uint8_t{0}});
        renderer.render();
        vkcore.waitIdle();
        const auto masked = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        CHECK(wp374aPixel(masked, 0)[0] == 0xa0u);
        CHECK(wp374aPixel(masked, 1) ==
              std::array{1280u, 0xffffffffu, 0u, 0u});
        const auto masked_diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK(masked_diagnostics.tlas_build_count ==
              visible_diagnostics.tlas_build_count + 1);

        instances.setRayInstanceDeclaration(right, 374012, std::nullopt);
        renderer.render();
        vkcore.waitIdle();
        const auto restored = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        CHECK(wp374aPixel(restored, 0)[0] == 0xa0u);
        CHECK(wp374aPixel(restored, 1)[0] == 0xa0u);
        CHECK(masked.bytes != restored.bytes);
        const auto restored_diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK(restored_diagnostics.tlas_build_count ==
              masked_diagnostics.tlas_build_count + 1);

        renderer.render();
        vkcore.waitIdle();
        const auto reused = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        const auto reused_diagnostics =
            RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
        CHECK(reused.bytes == restored.bytes);
        CHECK(reused_diagnostics.tlas_build_count ==
              restored_diagnostics.tlas_build_count);
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan WP374a mask fixture unavailable");
        throw;
    }
#else
    SUCCEED("WP374a GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE(
    "WP374a force opaque on one chair suppresses its any-hit invocation",
    "[WP374a][WP374][headless][gpu][ray-tracing][rt3-1][instance-flags][any-hit]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeWp374aRt2aV2Project(temp_dir, 0);
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        auto primitive = GET_MODULE(VertBufContainer).addPrimitiveEntry(
            wp374aTriangle());
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        const auto model = wp374aModel(
            37402, "models/wp367_single.glb", 0, primitive);
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto left = placeWp374aChair(instances, model, -2.0f);
        const auto right = placeWp374aChair(instances, model, 2.0f);
        instances.setRayInstanceDeclaration(left, 374021, std::nullopt);
        instances.setRayInstanceDeclaration(
            right, 374022,
            RayInstanceComponent{
                .instance_flags =
                    std::uint32_t{rayInstanceForceOpaque}});

        auto &renderer = GET_MODULE(Renderer);
        renderer.render();
        vkcore.waitIdle();
        const auto output = RendererTestAccess::readRenderTarget(renderer,
            "wp367_actual", ImageSubresourceRange{});
        const auto left_pixel = wp374aPixel(output, 0);
        const auto right_pixel = wp374aPixel(output, 1);
        CHECK(left_pixel[2] == wp367SingleRecord);
        CHECK(right_pixel[2] == wp367SingleRecord);
        CHECK(left_pixel[3] ==
              wp367MarkerProof(wp367ShadowRayMarker, wp367SingleGroup));
        CHECK(right_pixel[3] ==
              wp374aClosestProof(wp367ShadowRayMarker, wp367SingleGroup));
        CHECK(left_pixel[3] != right_pixel[3]);
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan WP374a instance-flags fixture unavailable");
        throw;
    }
#else
    SUCCEED("WP374a GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE(
    "WP374a custom index survives q shift while production SBT sentinels stay independent",
    "[WP374a][WP374][headless][gpu][ray-tracing][rt3-1][custom-index][sbt]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeWp374aRt1bV2Project(temp_dir);
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        auto primitive = GET_MODULE(VertBufContainer).addPrimitiveEntry(
            wp374aTriangle());
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        const auto model = wp374aModel(
            37403, "models/tree.glb", 2, primitive);
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto target = placeWp374aChair(instances, model, -2.0f);
        const auto neighbor = placeWp374aChair(instances, model, 2.0f);
        constexpr std::uint32_t declared_index = 0x654321u;
        instances.setRayInstanceDeclaration(
            target, 374031,
            RayInstanceComponent{.custom_index = declared_index});
        instances.setRayInstanceDeclaration(neighbor, 374032, std::nullopt);

        std::vector<RayTracingSbtWriteObservation> writes;
        const ScopedRayTracingSbtWriteObserver observer{
            [&](const auto &write) { writes.push_back(write); }};
        auto &renderer = GET_MODULE(Renderer);
        renderer.render();
        vkcore.waitIdle();
        const auto first_q =
            wp374aRowForAuthor(wp374aActiveRows(), 374031).q;
        const auto first = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        CHECK(wp374aPixel(first, 0)[1] == declared_index);

        const auto inserted = placeWp374aChair(instances, model, 8.0f);
        // Insert an authored identity before the target in canonical order.
        instances.setRayInstanceDeclaration(inserted, 374030, std::nullopt);
        writes.clear();
        renderer.render();
        vkcore.waitIdle();
        const auto &shifted_rows = wp374aActiveRows();
        const auto &target_row = wp374aRowForAuthor(shifted_rows, 374031);
        CHECK(target_row.q != first_q);
        CHECK(target_row.instance_custom_index == declared_index);
        const auto shifted = RendererTestAccess::readRenderTarget(renderer,
            "wp366_a", ImageSubresourceRange{});
        const auto target_pixel = wp374aPixel(shifted, 0);
        CHECK(target_pixel[0] == 0xa0u);
        CHECK(target_pixel[1] == declared_index);
        CHECK(target_pixel[2] == 101u);

        REQUIRE(writes.size() == 2);
        const auto &capabilities = vkcore.getRuntimeCapabilities();
        REQUIRE(capabilities.shader_group_handle_alignment != 0);
        const auto data_start =
            (capabilities.shader_group_handle_size +
             capabilities.shader_group_handle_alignment - 1u) /
            capabilities.shader_group_handle_alignment *
            capabilities.shader_group_handle_alignment;
        for (const auto &write : writes) {
            REQUIRE(write.record_index_stride == 2);
            REQUIRE(write.record_count == 6);
            REQUIRE(write.hit_bytes.size() ==
                    static_cast<std::size_t>(write.record_byte_stride) * 6u);
            for (std::uint32_t q = 0; q < 3; ++q) {
                for (std::uint32_t r = 0; r < 2; ++r) {
                    const auto at =
                        static_cast<std::size_t>(q * 2u + r) *
                            write.record_byte_stride +
                        data_start;
                    std::array<float, 4> tint{};
                    std::uint32_t flags = 0;
                    std::memcpy(tint.data(), write.hit_bytes.data() + at,
                                sizeof(tint));
                    std::memcpy(&flags,
                                write.hit_bytes.data() + at + sizeof(tint),
                                sizeof(flags));
                    CHECK(flags == 101u);
                    CHECK(tint[0] == 1.0f);
                    CHECK(tint[1] == 0.0f);
                }
            }
        }
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan WP374a custom-index fixture unavailable");
        throw;
    }
#else
    SUCCEED("WP374a GPU acceptance is compiler-ON only");
#endif
}

TEST_CASE(
    "WP374a F G F geometry flags rebuild exact resident keys and change any-hit on GPU",
    "[WP374a][WP374][headless][gpu][ray-tracing][rt3-1][blas-cache][geometry-flags][any-hit][mutation-3][mutation-4]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    const auto temp_dir = makeTempProjectDir();
    try {
        FastModuleContainer modules;
        writeWp374aRt2aV2Project(temp_dir, 0);
        configureRayQueryTestRuntime(temp_dir);
        auto &vkcore = GET_MODULE(VulkanManageCore);
        if (!vkcore.getRuntimeCapabilities().ray_tracing_pipeline) {
            SKIP("pelican.vulkan.ray_tracing_pipeline@1 is unavailable");
        }
        auto primitive = GET_MODULE(VertBufContainer).addPrimitiveEntry(
            wp374aTriangle());
        primitive.mesh_index = 0;
        primitive.primitive_index = 0;
        const auto model = wp374aModel(
            37404, "models/wp367_single.glb", 0, primitive);
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto chair = placeWp374aChair(instances, model, -2.0f);
        instances.setRayInstanceDeclaration(chair, 374041, std::nullopt);

        auto &renderer = GET_MODULE(Renderer);
        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        auto &reload = GET_MODULE(watch::ReloadService);
        std::uint64_t previous_builds = 0;
        std::optional<std::uint64_t> allocation;
        const auto render_generation =
            [&](std::uint64_t expected_delta,
                std::uint32_t expected_flags,
                std::uint32_t expected_proof) {
                renderer.render();
                vkcore.waitIdle();
                const auto diagnostics =
                    RendererTestAccess::rayQueryAccelerationStructureDiagnostics(renderer);
                CHECK(diagnostics.blas_build_count ==
                      previous_builds + expected_delta);
                previous_builds = diagnostics.blas_build_count;
                REQUIRE(diagnostics.active_blas_inputs.size() == 1);
                const auto &resident = diagnostics.active_blas_inputs.front();
                if (!allocation) {
                    allocation = resident.geometry_allocation_id;
                }
                CHECK(resident.geometry_allocation_id == *allocation);
                CHECK(resident.geometry_flags == expected_flags);
                const auto output = RendererTestAccess::readRenderTarget(renderer,
                    "wp367_actual", ImageSubresourceRange{});
                CHECK(wp374aPixel(output, 0)[3] == expected_proof);
            };
        const auto f_proof =
            wp367MarkerProof(wp367ShadowRayMarker, wp367SingleGroup);
        const auto g_proof =
            wp374aClosestProof(wp367ShadowRayMarker, wp367SingleGroup);

        render_generation(1, 0, f_proof); // F1
        render_generation(0, 0, f_proof); // F2

        const auto publish_layout = [&](std::uint32_t flags,
                                        std::uint64_t sequence) {
            writeWp374aRt2aV2Project(temp_dir, flags);
            const auto before = runtime.snapshot();
            REQUIRE(before != nullptr);
            REQUIRE(reload.applyRequestForTesting(
                watch::ReloadRequest{
                    watch::makeAssetKey("ray_layout.json"),
                    watch::ReloadKind::modified, {}, sequence}));
            REQUIRE(runtime.stagedGeneration() != 0);
            renderer.render();
            vkcore.waitIdle();
            REQUIRE(runtime.stagedGeneration() == 0);
            REQUIRE(runtime.activeGeneration() == before->generation + 1);
        };

        publish_layout(rayGeometryOpaque, 1);
        render_generation(1, rayGeometryOpaque, g_proof); // G1
        render_generation(0, rayGeometryOpaque, g_proof); // G2

        publish_layout(0, 2);
        render_generation(1, 0, f_proof); // F3
        render_generation(0, 0, f_proof); // F4
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &error) {
        std::filesystem::remove_all(temp_dir);
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "Vulkan WP374a F-G-F fixture unavailable");
        throw;
    }
#else
    SUCCEED("WP374a GPU acceptance is compiler-ON only");
#endif
}

} // namespace Pelican
