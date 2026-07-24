#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/imageloader.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/log.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderer/shadowdepthpasscontainer.hpp"
#include "../src/core/renderer/velocitypasscontainer.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpassconfigregistration.hpp"
#include "../src/core/renderingpass/renderpipelinegpuarena.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/watch/reloadservice.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include "ktx2_test_writer.hpp"

namespace Pelican {

namespace {

std::filesystem::path makeTempProjectDir() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("pelican_headless_render_" + std::to_string(suffix));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeTextFile(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream file{path, std::ios::binary};
    file << contents;
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

void renderClearFrame(RenderTarget &render_target, vk::ClearColorValue clear_color) {
    auto frame = render_target.render_begin();

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

    render_target.render_end();
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

const char *pipelineReloadFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.2, 0.4, 0.6, 1.0);
}
)glsl";
}

nlohmann::json gpuArenaRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "features": [
    "engine://features/debug_draw.json",
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
            GET_MODULE(MaterialContainer).textureViewsForTesting(dual_use_texture);
        REQUIRE(data_view);
        REQUIRE(color_view);
        REQUIRE(data_view != color_view);
        const auto ktx_bytes = TestKtx2::makeRgba8Srgb188();
        const auto ktx_loaded = loadImageMemory(ktx_bytes, "headless-known-188.ktx2");
        REQUIRE(static_cast<unsigned>(ktx_loaded.pixels.front()) == 188);
        const auto ktx_texture = GET_MODULE(MaterialContainer).registerTexture(
            ktx_loaded, "headless-known-188.ktx2");
        const auto [ktx_data_view, ktx_color_view] =
            GET_MODULE(MaterialContainer).textureViewsForTesting(ktx_texture);
        REQUIRE(ktx_data_view);
        REQUIRE(ktx_color_view);
        REQUIRE(ktx_data_view != ktx_color_view);
        REQUIRE(GET_MODULE(MaterialContainer).textureMipLevelsForTesting(ktx_texture) == 2);
        const std::array clears{
            vk::ClearColorValue{std::array{1.0f, 0.0f, 0.0f, 1.0f}},
            vk::ClearColorValue{std::array{0.0f, 0.0f, 1.0f, 1.0f}},
            vk::ClearColorValue{std::array{0.0f, 1.0f, 0.0f, 1.0f}},
        };

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
        SKIP(std::string{"Vulkan headless rendering unavailable: "} + ex.what());
    }
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(asset_path, R"json({"models":[]})json");
        writeTextFile(
            temp_dir / "hybrid.json",
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"},
                  {"settings",
                   {{"msaa",
                     {{"samples", 4},
                      {"fallback", "lower_supported"},
                      {"scope", "geometry"}}}}}}},
            }
                .dump(2));

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
        const auto frame_plan_json =
            renderer.currentFramePlanJson();
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
            surface, surface_reference, lowered);
        MaterialInfo material{
            .vert_shader = shaders.vertex,
            .frag_shader = shaders.fragment,
            .base_color_texture = standard.whiteTexture(),
            .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
            .normal_texture = standard.normalDefaultTexture(),
            .emissive_texture = standard.emissiveDefaultTexture(),
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
                refraction_lowered);
        MaterialInfo refraction_material{
            .vert_shader = refraction_shaders.vertex,
            .frag_shader = refraction_shaders.fragment,
            .base_color_texture = standard.whiteTexture(),
            .metallic_roughness_texture =
                standard.metallicRoughnessDefaultTexture(),
            .normal_texture = standard.normalDefaultTexture(),
            .emissive_texture = standard.emissiveDefaultTexture(),
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
        const auto first_revision = materials.screenInputBindingRevisionForTesting(
            refraction_id, transparent->definition);
        REQUIRE(first_revision != 0);
        const auto opaque_color = GET_MODULE(RenderTargetContainer)
                                      .getRenderTargetIdByName("opaque_color");
        const auto opaque_depth = GET_MODULE(RenderTargetContainer)
                                      .getRenderTargetIdByName("opaque_depth");
        const auto first_views = materials.boundScreenInputImageViewsForTesting(
            refraction_id, transparent->definition);
        REQUIRE(first_views ==
                std::vector<vk::ImageView>{
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_color),
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_depth)});
        renderer.recreateRenderTargetsAndRebindForTesting({32, 32});
        REQUIRE(materials.screenInputBindingRevisionForTesting(
                    refraction_id, transparent->definition) > first_revision);
        REQUIRE(materials.boundScreenInputImageViewsForTesting(
                    refraction_id, transparent->definition) ==
                std::vector<vk::ImageView>{
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_color),
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_depth)});

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

        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        SKIP(std::string{"Vulkan hybrid rendering unavailable: "} + ex.what());
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(temp_dir / "assets.json",
                      R"json({"models":[]})json");
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
            initial_config.dump());

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
            replacement.dump());
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
            unsupported_feature.dump());
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
        SKIP(
            std::string{
                "Vulkan pipeline reload unavailable: "} +
            error.what());
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(asset_path, R"json({"models":[]})json");
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
        const auto baseline =
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw, &debug_text);
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
        const auto config = gpuArenaRenderingConfig().dump();
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
                    &debug_text) == baseline);
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

        const auto registered =
            registerRenderingPassConfigFromJsonData(
                config, {16, 16},
                gpuArenaRegistrationDependencies({}));
        REQUIRE(registered.runtime_generation == 1);
        auto generation = runtime.snapshot();
        REQUIRE(generation != nullptr);
        REQUIRE(generation->generation == 1);
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
            RenderPipelineGpuResourceKind::debug_text_pass));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::shadow_depth_pass));
        REQUIRE(has_kind(
            RenderPipelineGpuResourceKind::velocity_pass));
        const auto committed =
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw, &debug_text);
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
            batch_config_json.dump();

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
            [](const RenderPipelineRuntimeGeneration &) {
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
        REQUIRE_THROWS_WITH(
            registerRenderingPassConfigVariantsFromJsonData(
                batch_config, {16, 16},
                std::move(failed_variant_batch)),
            "Injected variant-batch validation failure");
        REQUIRE(runtime.snapshot() == generation);
        REQUIRE(
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw,
                &debug_text) == committed);
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
                replacement_config.dump(), {16, 16},
                gpuArenaRegistrationDependencies(
                    std::move(
                        failed_replacement_options))),
            "Injected render pipeline GPU registration failure");
        REQUIRE(runtime.snapshot() == generation);
        REQUIRE(
            inspectRenderPipelineGpuRegistryCounts(
                registries, &debug_draw,
                &debug_text) == committed);
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
                replacement_config.dump(), {16, 16},
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
                registries, &debug_draw, &debug_text);
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
                registries, &debug_draw, &debug_text);
        REQUIRE(retired.render_targets <
                both_generations.render_targets);
        REQUIRE(retired.frame_graph_buffers <
                both_generations.frame_graph_buffers);
        REQUIRE(retired.compute_tasks <
                both_generations.compute_tasks);
        REQUIRE_THROWS(
            GET_MODULE(RenderTargetContainer)
                .getMetadata(old_target));

        GET_MODULE(VulkanManageCore).waitIdle();
        std::filesystem::remove_all(temp_dir);
    } catch (const std::exception &ex) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(temp_dir);
        }
        SKIP(std::string{
                 "Vulkan GPU arena transaction unavailable: "} +
             ex.what());
    }
#endif
}

} // namespace Pelican
