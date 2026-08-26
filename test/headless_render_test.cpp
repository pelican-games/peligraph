#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/imageloader.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/log.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/light/lightcontainer.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
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
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/vkcore/util.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/project/vulkanviewplanning.hpp"
#include "vulkan_test_support.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <vector>
#include "ktx2_test_writer.hpp"
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

void writeRayQueryTestProject(
    const std::filesystem::path &directory,
    bool require_ray_query) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
                  rendering.dump(2));
}

void writeRtShadowMaskTestProject(
    const std::filesystem::path &directory,
    bool raster_shadow,
    bool ray_tracing_pipeline = false) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
        nlohmann::json{
            {"pipeline",
             {{"preset",
               "engine://render_pipelines/hybrid_v1.json"}}},
            {"features", std::move(features)},
        }.dump(2));
}

void writeEmbeddedRtShadowMaskTestProject(
    const std::filesystem::path &directory) {
    writeTextFile(
        directory / "scene.json",
        R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    writeTextFile(
        directory / "assets.json",
        R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
    // This intentionally has no shader defines or generated includes. The
    // compiler-OFF build must resolve every stage to embedded SPIR-V.
    writeTextFile(
        directory / "hybrid.json",
        R"json({
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
            GET_MODULE(MaterialContainer).textureViewsForTesting(dual_use_texture);
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
        REQUIRE(GET_MODULE(MaterialContainer)
                    .textureMipLevelsForTesting(mipmapped_texture) == 3);
        REQUIRE(GET_MODULE(MaterialContainer).texturePixelsForTesting(
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
            GET_MODULE(MaterialContainer).texturePixelsForTesting(
                mipmapped_texture, 1),
            4);
        require_neutral_filtered_mip(
            GET_MODULE(MaterialContainer).texturePixelsForTesting(
                mipmapped_texture, 2),
            1);
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
        REQUIRE(GET_MODULE(MaterialContainer).texturePixelsForTesting(
                    ktx_texture, 0) ==
                std::vector<uint8_t>(16, 188));
        REQUIRE(GET_MODULE(MaterialContainer).texturePixelsForTesting(
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
        writeTextFile(
            temp_dir / "hybrid.json",
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
            }
                .dump(2));

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
            materials.textureDimensionForTesting(
                cube_texture) ==
            SurfaceTextureDimension::cube);
        REQUIRE(
            materials.textureViewTypeForTesting(
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
            materials.textureDimensionForTesting(
                array_texture) ==
            SurfaceTextureDimension::two_d_array);
        REQUIRE(
            materials.textureViewTypeForTesting(
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
            materials.textureDimensionForTesting(
                volume_texture) ==
            SurfaceTextureDimension::three_d);
        REQUIRE(
            materials.textureViewTypeForTesting(
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
            materials.materialSamplerResolutionForTesting(
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            materialResourceRenderingConfig().dump(2));

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
            materials
                .screenInputBindingRevisionForTesting(
                    material_id,
                    geometry_pass->definition);
        REQUIRE(initial_binding_revision != 0);
        auto non_consumer_pass =
            geometry_pass->definition;
        non_consumer_pass.materialInfo().contract =
            MaterialPassContract::
                deferred_geometry_v1;
        REQUIRE_THROWS_WITH(
            materials
                .screenInputBindingRevisionForTesting(
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
                    false);
        REQUIRE(
            initial_tint_mip_view !=
            initial_tint_view);
        REQUIRE(
            materials
                .boundScreenInputImageViewsForTesting(
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
            materials
                .screenInputBindingRevisionForTesting(
                    material_id,
                    geometry_pass->definition);
        REQUIRE(
            direct_material_rebind_revision >
            initial_binding_revision);
        REQUIRE(
            materials
                .boundScreenInputImageViewsForTesting(
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
            materials
                .screenInputBindingRevisionForTesting(
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
            materials
                .screenInputBindingRevisionForTesting(
                    material_id,
                    missing_port_pass),
            Catch::Matchers::ContainsSubstring(
                "is not provided"));

        renderer.recreateRenderTargetsAndRebindForTesting(
            {32, 32});
        REQUIRE(
            materials
                .screenInputBindingRevisionForTesting(
                    material_id,
                    geometry_pass->definition) >
            direct_material_rebind_revision);
        const auto recreated_tint =
            GET_MODULE(RenderTargetContainer)
                .getRenderTargetIdByName(
                    "material_tint");
        REQUIRE(
            materials
                .boundScreenInputImageViewsForTesting(
                    material_id,
                    geometry_pass->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getImageSubresourceView(
                        recreated_tint,
                        full_tint_mips,
                        false)});

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
            materials
                .screenInputBindingRevisionForTesting(
                    material_id,
                    geometry_pass->definition);
        auto replacement =
            materialResourceRenderingConfig();
        replacement["buffers"][0]["size"] =
            32;
        writeTextFile(
            temp_dir / "pipeline.json",
            replacement.dump(2));
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
            materials
                .screenInputBindingRevisionForTesting(
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
            materials
                .boundScreenInputImageViewsForTesting(
                    material_id,
                    reloaded_geometry_pass
                        ->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getImageSubresourceView(
                        reloaded_tint,
                        full_tint_mips,
                        false)});

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
            invalid_replacement.dump(2));
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
            materials
                .screenInputBindingRevisionForTesting(
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(temp_dir / "assets.json",
                      R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            upscaleContractRenderingConfig().dump(2));
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
            replacement.dump(2));

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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            localReadRenderingConfig().dump(2));

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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            materialLocalReadRenderingConfig().dump(2));

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
            materials
                .screenInputBindingRevisionForTesting(
                    material_id,
                    consumer->definition) != 0);
        REQUIRE(
            materials
                .boundScreenInputImageViewsForTesting(
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            dependencySafePhysicalScopeRenderingConfig()
                .dump(2));

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
            replacement.dump(2));

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
                {
                    vk::AttachmentLoadOp::
                        eClear,
                    vk::AttachmentStoreOp::
                        eStore,
                }});

        renderer.setExecutionTracingForTesting(
            true);
        engine_time.advance();
        renderer.render();
        GET_MODULE(VulkanManageCore)
            .waitIdle();

        const auto &trace =
            renderer
                .lastExecutionTraceForTesting();
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            aliasLifetimeRenderingConfig().dump(2));

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
            renderer
                .imageAliasDependencyCountForTesting() >=
            1);

        renderer
            .recreateRenderTargetsAndRebindForTesting(
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
            renderer
                .imageAliasDependencyCountForTesting() >=
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array(
                     {"project://features/silhouette.json"})},
            }
                .dump(2));

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
            materials.materialGpuRecordForTesting(base);
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
            materials.materialGpuRecordForTesting(base);
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
        writeTextFile(
            temp_dir / "hybrid.json",
            nlohmann::json{
                {"pipeline",
                 {{"preset",
                   "engine://render_pipelines/hybrid_v1.json"}}},
                {"features",
                 nlohmann::json::array(
                     {"engine://features/clustered_lighting.json"})},
                {"xr",
                 {{"view_execution", "sequential"}}},
            }
                .dump(2));

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
                    .component =
                        {
                            {"type", "directional"},
                            {"direction",
                             {0.0, 0.0, -1.0}},
                            {"intensity", 1.0},
                            {"color",
                             {1.0, 1.0, 1.0}},
                        },
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
            renderer
                .currentFramePlanOrderForTesting();
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
        renderer.setExecutionTracingForTesting(
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            asset_path,
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
        std::filesystem::create_directories(
            temp_dir / "features");
        writeTextFile(
            temp_dir / "features" /
                "shadow_directional.json",
            engineResourceOrThrow(
                "features/shadow_directional.json"));
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
        const auto first_revision = materials.screenInputBindingRevisionForTesting(
            refraction_id, transparent->definition);
        REQUIRE(first_revision != 0);
        const auto opaque_revision =
            materials.screenInputBindingRevisionForTesting(
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
            materials.boundScreenInputImageViewsForTesting(
                material_id, opaque->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getLayeredImageView(shadow_map)});
        const auto first_views = materials.boundScreenInputImageViewsForTesting(
            refraction_id, transparent->definition);
        REQUIRE(first_views ==
                std::vector<vk::ImageView>{
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_color),
                    GET_MODULE(RenderTargetContainer).getImageView(opaque_depth),
                    GET_MODULE(RenderTargetContainer)
                        .getLayeredImageView(shadow_map)});
        renderer.recreateRenderTargetsAndRebindForTesting({32, 32});
        REQUIRE(materials.screenInputBindingRevisionForTesting(
                    refraction_id, transparent->definition) > first_revision);
        REQUIRE(
            materials.screenInputBindingRevisionForTesting(
                material_id, opaque->definition) >
            opaque_revision);
        REQUIRE(
            materials.boundScreenInputImageViewsForTesting(
                material_id, opaque->definition) ==
            std::vector<vk::ImageView>{
                GET_MODULE(RenderTargetContainer)
                    .getLayeredImageView(shadow_map)});
        REQUIRE(materials.boundScreenInputImageViewsForTesting(
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
            materials.screenInputBindingRevisionForTesting(
                material_id, opaque->definition);
        const auto previous_shadow_views =
            materials.boundScreenInputImageViewsForTesting(
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
        reloaded_shadow_feature["name"] =
            "shadow_directional_reloaded";
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
                    .render_pipeline->feature_names.begin(),
                renderer.previewGraphProgram()
                    .render_pipeline->feature_names.end(),
                "shadow_directional_reloaded") !=
            renderer.previewGraphProgram()
                .render_pipeline->feature_names.end());

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
            materials.screenInputBindingRevisionForTesting(
                material_id,
                reloaded_opaque->definition);
        REQUIRE(reloaded_opaque_revision >
                resized_opaque_revision);
        const auto reloaded_shadow_views =
            materials.boundScreenInputImageViewsForTesting(
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
                    .render_pipeline->feature_names.begin(),
                renderer.previewGraphProgram()
                    .render_pipeline->feature_names.end(),
                "shadow_directional_reloaded") !=
            renderer.previewGraphProgram()
                .render_pipeline->feature_names.end());
        REQUIRE(previous_generation->find(
                    main_render_id) != nullptr);
        REQUIRE(previous_generation->gpu_arena
                    ->resourceCount() ==
                previous_gpu_resource_count);
        REQUIRE(
            materials.screenInputBindingRevisionForTesting(
                material_id,
                reloaded_opaque->definition) ==
            reloaded_opaque_revision);
        REQUIRE(
            materials.boundScreenInputImageViewsForTesting(
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
        writeTextFile(
            temp_dir / "hybrid.json",
            nlohmann::json{
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
            }
                .dump(2));

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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            config.dump(2));

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
            state_reload.dump(2));
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
            coordinated_reload.dump(2));
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
            GET_MODULE(MaterialContainer)
                .materialOutputSchemaForTesting(
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
            failed_reload.dump(2));
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
            GET_MODULE(MaterialContainer)
                .materialOutputSchemaForTesting(
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(temp_dir / "assets.json",
                      R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            asset_path,
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
                replacement_config.dump(), {16, 16},
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
            R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
        writeTextFile(
            temp_dir / "assets.json",
            R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
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
            depthPyramidRenderingConfig().dump(2));

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
                target, mip_zero, false);
        const auto initial_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one, false);
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
                target, mip_zero, false) ==
            initial_mip_zero_view);

        renderer
            .recreateRenderTargetsAndRebindForTesting(
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
                target, mip_zero, false);
        const auto resized_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one, false);
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
                target, mip_zero, false);
        const auto pre_reload_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one, false);
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
                target, mip_zero, false);
        const auto reloaded_mip_one_view =
            render_targets.getImageSubresourceView(
                target, mip_one, false);
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
            renderer
                .rayQueryAccelerationStructureDiagnosticsForTesting();
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
        vat_primitive.vat_deformed = true;

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
        auto initial = renderer
                           .rayQueryAccelerationStructureDiagnosticsForTesting();
        REQUIRE(initial.requested);
        CHECK(initial.blas_build_count == 1);
        CHECK(initial.tlas_build_count == 1);
        CHECK(initial.active_blas_count == 1);
        CHECK(initial.tlas_instance_count == 1);
        CHECK(initial.excluded.primitive_count == 4);
        CHECK(initial.excluded.instance_count == 4);
        CHECK(initial.excluded.skinned_primitive_count == 1);
        CHECK(initial.excluded.morph_primitive_count == 1);
        CHECK(initial.excluded.vat_primitive_count == 1);
        CHECK(initial.excluded.blas_ineligible_primitive_count == 1);
        REQUIRE(initial.excluded.skinned_names.size() == 1);
        REQUIRE(initial.excluded.morph_names.size() == 1);
        REQUIRE(initial.excluded.vat_names.size() == 1);
        REQUIRE(initial.excluded.blas_ineligible_names.size() == 1);
        REQUIRE(initial.active_blas_inputs.size() == 1);
        const auto old_blas_input =
            initial.active_blas_inputs.front();

        // Transform changes are TLAS data, not a BLAS invalidation source.
        instances.setTrs(
            model_instance, {0.1F, 0.0F, 0.0F},
            glm::quat{1.0F, 0.0F, 0.0F, 0.0F},
            {1.0F, 1.0F, 1.0F});
        renderer.render();
        const auto transformed =
            renderer
                .rayQueryAccelerationStructureDiagnosticsForTesting();
        CHECK(transformed.blas_build_count ==
              initial.blas_build_count);
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
            renderer
                .rayQueryAccelerationStructureDiagnosticsForTesting();
        CHECK(rebuilt.blas_build_count ==
              transformed.blas_build_count + 1);
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
            renderer
                .rayQueryAccelerationStructureDiagnosticsForTesting();
        CHECK(reallocated.geometry_pool_invalidation_count == 1);
        CHECK(reallocated.blas_build_count ==
              rebuilt.blas_build_count + 1);
        REQUIRE(reallocated.active_blas_inputs.size() == 1);
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
                  .at("static_only") == true);
        CHECK(plan.at("ray_query_acceleration_structures")
                  .at("excluded")
                  .at("primitive_count") == 4);
        CHECK(plan.at("ray_query_acceleration_structures")
                  .at("excluded")
                  .at("blas_ineligible_primitive_count") == 1);
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
    "ray query and RT pipeline shadow masks match pixel-exactly across the static-only boundary",
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
            vat,
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
                blocker == BlockerKind::vat) {
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
                candidate.vat_deformed =
                    blocker == BlockerKind::vat;
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
                    .component = {
                        {"type", "directional"},
                        // A background sample at the cleared world origin
                        // intersects the x=-0.25 blocker along this ray.
                        // The corner assertions therefore exercise coverage,
                        // rather than relying on an incidental miss.
                        {"direction", {0.5, 0.0, -1.0}},
                        {"intensity", 4.0},
                        {"color", {1.0, 1.0, 1.0}},
                    },
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
                    renderer.readR8RenderTargetForTesting(
                        "rt_shadow_mask"),
                .pipeline_mask =
                    renderer.readR8RenderTargetForTesting(
                        "rt_shadow_mask_pipeline"),
                .color = GET_MODULE(RenderTarget)
                             .readbackLastFrameRGBA8(),
                .diagnostics =
                    renderer
                        .rayQueryAccelerationStructureDiagnosticsForTesting(),
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
        const auto vat =
            render(BlockerKind::vat, false);
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
        require_pipeline_match(vat);
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
        // These blockers are still rasterized into gbuffer_worldpos, but the
        // WP281/WP282 static-only TLAS deliberately omits them. Their missing
        // shadows are an asserted image contract, not a blessed golden.
        REQUIRE(dark_count(skinned.mask.pixels) == 0);
        REQUIRE(dark_count(morph.mask.pixels) == 0);
        REQUIRE(dark_count(vat.mask.pixels) == 0);
        REQUIRE(skinned.diagnostics.excluded.skinned_primitive_count == 1);
        REQUIRE(morph.diagnostics.excluded.morph_primitive_count == 1);
        REQUIRE(vat.diagnostics.excluded.vat_primitive_count == 1);

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
    "ray query and RT pipeline shadow masks match for an empty static scene",
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
                .component = {
                    {"type", "directional"},
                    {"direction", {0.0, 0.0, -1.0}},
                    {"intensity", 1.0},
                    {"color", {1.0, 1.0, 1.0}},
                },
            },
        });
        auto &camera = GET_MODULE(Camera);
        camera.setPos({0.0F, 0.0F, 2.0F});
        camera.setDir({0.0F, 0.0F, -1.0F});
        camera.setUp({0.0F, 1.0F, 0.0F});

        renderer.render();
        vkcore.waitIdle();
        const auto mask =
            renderer.readR8RenderTargetForTesting(
                "rt_shadow_mask");
        const auto pipeline_mask =
            renderer.readR8RenderTargetForTesting(
                "rt_shadow_mask_pipeline");
        REQUIRE(mask.extent == vk::Extent2D{32, 32});
        REQUIRE(pipeline_mask.extent == mask.extent);
        REQUIRE(pipeline_mask.pixels == mask.pixels);
        REQUIRE(std::all_of(
            mask.pixels.begin(), mask.pixels.end(),
            [](const auto value) { return value > 192; }));
        const auto diagnostics =
            renderer
                .rayQueryAccelerationStructureDiagnosticsForTesting();
        REQUIRE(diagnostics.requested);
        REQUIRE(diagnostics.tlas_build_count == 1);
        REQUIRE(diagnostics.active_blas_count == 0);
        REQUIRE(diagnostics.tlas_instance_count == 0);
        REQUIRE(diagnostics.excluded.instance_count == 1);
        REQUIRE(
            diagnostics.excluded.skinned_primitive_count == 1);
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
                .component = {
                    {"type", "directional"},
                    {"direction", {0.0, 0.0, -1.0}},
                    {"intensity", 1.0},
                    {"color", {1.0, 1.0, 1.0}},
                },
            },
        });

        renderer.render();
        vkcore.waitIdle();
        const auto mask =
            renderer.readR8RenderTargetForTesting(
                "rt_shadow_mask");
        const auto pipeline_mask =
            renderer.readR8RenderTargetForTesting(
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
            renderer
                .rayQueryAccelerationStructureDiagnosticsForTesting();
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

} // namespace Pelican
