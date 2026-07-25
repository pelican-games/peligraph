#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/renderingpass/rendertargetimageviewresolver.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/project/targetrenderplanning.hpp"
#include "../src/core/shader/pelican_sets.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/render_pass_frame_setup.hpp"
#include "../src/core/vkcore/util.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

constexpr vk::Extent2D test_extent{8, 4};
constexpr vk::Format test_format =
    vk::Format::eR8G8B8A8Unorm;
constexpr std::uint32_t stereo_view_count = 2;
constexpr std::uint32_t stereo_view_mask = 0b11;

class TempProject {
    std::filesystem::path root_;

  public:
    TempProject() {
        const auto suffix =
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
        root_ = std::filesystem::temp_directory_path() /
                ("pelican_multiview_" +
                 std::to_string(suffix));
        std::filesystem::create_directories(root_);
        std::ofstream{root_ / "scene.json", std::ios::binary}
            << R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json";
        std::ofstream{root_ / "assets.json", std::ios::binary}
            << R"json({"schema":"pelican.assets","version":1,"assets":{}})json";
    }

    ~TempProject() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path &path() const noexcept {
        return root_;
    }
};

vk::UniqueImageView createColorView(
    vk::Device device, const ImageWrapper &image,
    vk::ImageViewType type, std::uint32_t base_layer,
    std::uint32_t layer_count) {
    if (layer_count == 0 ||
        base_layer >= image.array_layers ||
        layer_count > image.array_layers - base_layer) {
        throw std::runtime_error(
            "test image view layer range is invalid");
    }
    vk::ImageViewCreateInfo info;
    info.image = image.image.get();
    info.viewType = type;
    info.format = image.format;
    info.components = {
        vk::ComponentSwizzle::eR,
        vk::ComponentSwizzle::eG,
        vk::ComponentSwizzle::eB,
        vk::ComponentSwizzle::eA,
    };
    info.subresourceRange = {
        vk::ImageAspectFlagBits::eColor,
        0,
        image.mip_levels,
        base_layer,
        layer_count,
    };
    return device.createImageViewUnique(info);
}

VulkanUtils::ChangeImageLayoutInfo toColorAttachment() {
    return {
        .src_stage =
            vk::PipelineStageFlagBits::eTopOfPipe,
        .dst_stage =
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .src_access = {},
        .dst_access =
            vk::AccessFlagBits::eColorAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentWrite,
    };
}

VulkanUtils::ChangeImageLayoutInfo toTransferSource() {
    return {
        .src_stage =
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dst_stage =
            vk::PipelineStageFlagBits::eTransfer,
        .src_access =
            vk::AccessFlagBits::eColorAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentWrite,
        .dst_access =
            vk::AccessFlagBits::eTransferRead,
    };
}

const char *fullscreenVertexShader() {
    return R"glsl(
#version 450
vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);
void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
)glsl";
}

const char *frameProbeFragmentShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(
        pelicanFrame.camera_position.x,
        pelicanFrame.projection[0][0],
        pelicanFrame.camera_position.w,
        1.0);
}
)glsl";
}

const char *layeredInputFragmentShader() {
    return R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_sets.glsl"
#include "pelican_view.glsl"
layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform PELICAN_SAMPLER_2D_0 inputColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = PELICAN_TEXTURE_2D_0(inputColor, vec2(0.5));
}
)glsl";
}

ShaderBundleId compileBundle(
    ShaderLibrary &library, ShaderCompiler &compiler,
    std::string_view source, vk::ShaderStageFlagBits stage,
    std::string_view name,
    std::vector<std::string> defines = {}) {
    ShaderCompileOptions options;
    options.defines = std::move(defines);
    const auto compiled =
        compiler.compileSource(source, stage, name, options);
    if (!compiled.ok) {
        throw std::runtime_error(
            "multiview test shader compile failed: " +
            compiled.log);
    }
    return library.loadFromSpirv(compiled.spirv, name);
}

vk::UniqueDescriptorPool createFramePool(
    vk::Device device, std::uint32_t set_count) {
    const vk::DescriptorPoolSize pool_size{
        vk::DescriptorType::eUniformBuffer, set_count};
    vk::DescriptorPoolCreateInfo info;
    info.flags =
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    info.maxSets = set_count;
    info.setPoolSizes(pool_size);
    return device.createDescriptorPoolUnique(info);
}

vk::UniqueDescriptorSet allocateFrameSet(
    vk::Device device, vk::DescriptorPool pool,
    vk::DescriptorSetLayout layout, const BufferWrapper &buffer,
    vk::DeviceSize range) {
    vk::DescriptorSetAllocateInfo allocate;
    allocate.descriptorPool = pool;
    allocate.setSetLayouts(layout);
    auto descriptor =
        std::move(
            device.allocateDescriptorSetsUnique(allocate)
                .front());
    const vk::DescriptorBufferInfo buffer_info{
        buffer.buffer.get(), 0, range};
    vk::WriteDescriptorSet write;
    write.dstSet = descriptor.get();
    write.dstBinding = PELICAN_FRAME_UBO_BINDING;
    write.descriptorType =
        vk::DescriptorType::eUniformBuffer;
    write.setBufferInfo(buffer_info);
    device.updateDescriptorSets(write, {});
    return descriptor;
}

struct LayeredReadback {
    std::array<std::vector<std::uint8_t>,
               stereo_view_count>
        layers;
    std::uint32_t rendering_count = 0;
};

LayeredReadback renderLayered(
    const ImageWrapper &image,
    std::span<const vk::ImageView> attachment_views,
    PipelineFactory &factory, PipelineHandle pipeline,
    std::span<const vk::DescriptorSet> frame_sets,
    bool multiview) {
    const auto expected_attachment_count =
        multiview ? std::size_t{1}
                  : std::size_t{stereo_view_count};
    if (attachment_views.size() !=
            expected_attachment_count ||
        frame_sets.size() != expected_attachment_count) {
        throw std::runtime_error(
            "layered render fixture contract is inconsistent");
    }

    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto &utils = GET_MODULE(VulkanUtils);
    auto commands = vkcore.allocCmdBufs(1);
    auto &command = commands.front();
    const auto layer_bytes =
        static_cast<vk::DeviceSize>(test_extent.width) *
        test_extent.height * 4;
    const auto total_bytes =
        layer_bytes * stereo_view_count;
    auto staging = vkcore.allocBuf(
        total_bytes,
        vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eHostAccessRandom);

    command.recordBegin();
    utils.changeImageLayoutCmd(
        *command, image, vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal,
        toColorAttachment());

    LayeredReadback result;
    const auto execution_count =
        multiview ? 1u : stereo_view_count;
    for (std::uint32_t execution = 0;
         execution < execution_count; ++execution) {
        vk::RenderingAttachmentInfo color;
        color.imageView =
            attachment_views[multiview ? 0u : execution];
        color.imageLayout =
            vk::ImageLayout::eColorAttachmentOptimal;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.storeOp = vk::AttachmentStoreOp::eStore;
        color.clearValue.color =
            vk::ClearColorValue{
                std::array{0.0f, 0.0f, 0.0f, 1.0f}};

        vk::RenderingInfo rendering;
        rendering.renderArea =
            vk::Rect2D{{0, 0}, test_extent};
        rendering.layerCount = 1;
        rendering.viewMask =
            multiview ? stereo_view_mask : 0u;
        rendering.setColorAttachments(color);

        command->beginRendering(rendering);
        const vk::Viewport viewport{
            0.0f,
            0.0f,
            static_cast<float>(test_extent.width),
            static_cast<float>(test_extent.height),
            0.0f,
            1.0f,
        };
        const vk::Rect2D scissor{{0, 0}, test_extent};
        command->setViewport(0, viewport);
        command->setScissor(0, scissor);
        command->bindPipeline(
            vk::PipelineBindPoint::eGraphics,
            factory.pipeline(pipeline));
        command->bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            factory.layout(pipeline), PELICAN_SET_FRAME,
            frame_sets[multiview ? 0u : execution], {});
        command->draw(3, 1, 0, 0);
        command->endRendering();
        ++result.rendering_count;
    }

    utils.changeImageLayoutCmd(
        *command, image,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eTransferSrcOptimal,
        toTransferSource());
    std::array<vk::BufferImageCopy,
               stereo_view_count>
        copies;
    for (std::uint32_t layer = 0;
         layer < stereo_view_count; ++layer) {
        copies[layer].bufferOffset =
            layer_bytes * layer;
        copies[layer].imageSubresource = {
            vk::ImageAspectFlagBits::eColor,
            0,
            layer,
            1,
        };
        copies[layer].imageExtent = vk::Extent3D{
            test_extent.width,
            test_extent.height,
            1,
        };
    }
    command->copyImageToBuffer(
        image.image.get(),
        vk::ImageLayout::eTransferSrcOptimal,
        staging.buffer.get(), copies);
    command.recordEndSubmit();
    const auto device = vkcore.getDevice();
    REQUIRE(
        device.waitForFences(
            {command.getFence()}, VK_TRUE, UINT64_MAX) ==
        vk::Result::eSuccess);
    const auto bytes =
        vkcore.readBuf(staging, total_bytes);
    for (std::uint32_t layer = 0;
         layer < stereo_view_count; ++layer) {
        const auto begin =
            bytes.begin() +
            static_cast<std::ptrdiff_t>(
                layer_bytes * layer);
        result.layers[layer].assign(
            begin,
            begin +
                static_cast<std::ptrdiff_t>(
                    layer_bytes));
    }
    return result;
}

} // namespace

TEST_CASE(
    "typed Vulkan multiview renders two frame records in one execution",
    "[wp203b][multiview][gpu]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    TempProject project;
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(project.path(), false);
    GET_MODULE(ProjectSource).setSourceByData(
        R"json({"basic_config":{"window_size":{"width":8,"height":4},"scene_data_json":"scene.json","asset_data_json":"assets.json"}})json");
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = test_extent;

    VulkanManageCore *vkcore_ptr = nullptr;
    try {
        vkcore_ptr = &GET_MODULE(VulkanManageCore);
    } catch (const std::exception &error) {
        const std::string message = error.what();
        if (message.find(
                "No suitable Vulkan physical device found") !=
            std::string::npos) {
            SKIP(
                "WP203b multiview test requires a Vulkan device: "
                << message);
        }
        throw;
    }
    auto &vkcore = *vkcore_ptr;
    const auto features =
        vkcore.getPhysDevice()
            .getFeatures2<
                vk::PhysicalDeviceFeatures2,
                vk::PhysicalDeviceVulkan11Features>();
    const auto properties =
        vkcore.getPhysDevice()
            .getProperties2<
                vk::PhysicalDeviceProperties2,
                vk::PhysicalDeviceMultiviewProperties>();
    if (features
                .get<
                    vk::PhysicalDeviceVulkan11Features>()
                .multiview != VK_TRUE ||
        properties
                .get<
                    vk::PhysicalDeviceMultiviewProperties>()
                .maxMultiviewViewCount <
            stereo_view_count) {
        SKIP(
            "WP203b multiview test requires two-view Vulkan multiview");
    }

    ShaderCompiler compiler;
    auto &library = GET_MODULE(ShaderLibrary);
    const auto sequential_vert = compileBundle(
        library, compiler, fullscreenVertexShader(),
        vk::ShaderStageFlagBits::eVertex,
        "wp203b_sequential.vert");
    const auto sequential_frag = compileBundle(
        library, compiler, frameProbeFragmentShader(),
        vk::ShaderStageFlagBits::eFragment,
        "wp203b_sequential.frag");
    const auto multiview_frag = compileBundle(
        library, compiler, frameProbeFragmentShader(),
        vk::ShaderStageFlagBits::eFragment,
        "wp203b_multiview.frag",
        {"PELICAN_MULTIVIEW=1",
         "PELICAN_VIEW_COUNT=2"});
    const auto sequential_input_frag =
        compileBundle(
            library, compiler,
            layeredInputFragmentShader(),
            vk::ShaderStageFlagBits::eFragment,
            "wp203b_sequential_input.frag");
    const auto multiview_input_frag =
        compileBundle(
            library, compiler,
            layeredInputFragmentShader(),
            vk::ShaderStageFlagBits::eFragment,
            "wp203b_multiview_input.frag",
            {"PELICAN_MULTIVIEW=1",
             "PELICAN_VIEW_COUNT=2",
             "PELICAN_INPUT_0_LAYERED=1"});
    REQUIRE_FALSE(
        library.get(sequential_frag)
            .reflection.uses_view_index);
    REQUIRE(
        library.get(multiview_frag)
            .reflection.uses_view_index);
    REQUIRE_FALSE(
        library.get(sequential_input_frag)
            .reflection.uses_view_index);
    REQUIRE(
        library.get(multiview_input_frag)
            .reflection.uses_view_index);

    auto &factory = GET_MODULE(PipelineFactory);
    auto sequential_desc = GraphicsPipelineDesc{
        sequential_vert,
        sequential_frag,
        {test_format},
    };
    const auto sequential_pipeline =
        factory.create(sequential_desc);

    auto multiview_desc = GraphicsPipelineDesc{
        sequential_vert,
        multiview_frag,
        {test_format},
    };
    multiview_desc.view =
        GraphicsPipelineViewContract::multiview(
            stereo_view_count);
    const auto multiview_pipeline =
        factory.create(multiview_desc);

    auto invalid_desc = sequential_desc;
    invalid_desc.view =
        GraphicsPipelineViewContract::multiview(
            stereo_view_count);
    REQUIRE_THROWS_WITH(
        factory.create(invalid_desc),
        Catch::Matchers::ContainsSubstring(
            "must consume gl_ViewIndex"));

    std::array<FrameUniformData,
               stereo_view_count>
        frames;
    frames[0].camera_position =
        {0.2f, 0.0f, 0.0f, 0.1f};
    frames[0].projection[0][0] = 0.3f;
    frames[1].camera_position =
        {0.8f, 0.0f, 0.0f, 0.9f};
    frames[1].projection[0][0] = 0.7f;
    const auto multiview_bytes =
        packFrameUniformViews(frames);
    const std::array flat_frame{frames[0]};
    const auto flat_bytes =
        packFrameUniformViews(flat_frame);
    REQUIRE(
        flat_bytes.size() ==
        sizeof(FrameUniformData));
    REQUIRE(
        std::memcmp(
            flat_bytes.data(), &frames[0],
            sizeof(FrameUniformData)) == 0);

    auto &frame_resources =
        GET_MODULE(FrameResources);
    frame_resources.beginLogicalFrame(
        stereo_view_count);
    frame_resources.selectMultiview(0);
    frame_resources.updateMultiview(frames);
    const auto &managed_frame_data =
        frame_resources
            .multiviewSlotDataForTesting(0);
    REQUIRE(
        managed_frame_data.size() ==
        stereo_view_count);
    REQUIRE(
        std::memcmp(
            managed_frame_data.data(),
            frames.data(),
            multiview_bytes.size()) == 0);
    const auto managed_frame_bytes =
        vkcore.readBuf(
            frame_resources
                .multiviewSlotBufferForTesting(0),
            multiview_bytes.size());
    REQUIRE(
        managed_frame_bytes ==
        std::vector<std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(
                multiview_bytes.data()),
            reinterpret_cast<const std::uint8_t *>(
                multiview_bytes.data()) +
                multiview_bytes.size()});

    auto multiview_buffer = vkcore.allocBuf(
        multiview_bytes.size(),
        vk::BufferUsageFlagBits::eUniformBuffer,
        vma::MemoryUsage::eAuto,
        vma::AllocationCreateFlagBits::
            eHostAccessSequentialWrite);
    vkcore.writeBuf(
        multiview_buffer, multiview_bytes.data(), 0,
        multiview_bytes.size());
    std::array<BufferWrapper,
               stereo_view_count>
        sequential_buffers;
    for (std::uint32_t view = 0;
         view < stereo_view_count; ++view) {
        sequential_buffers[view] = vkcore.allocBuf(
            sizeof(FrameUniformData),
            vk::BufferUsageFlagBits::eUniformBuffer,
            vma::MemoryUsage::eAuto,
            vma::AllocationCreateFlagBits::
                eHostAccessSequentialWrite);
        vkcore.writeBuf(
            sequential_buffers[view], &frames[view],
            0, sizeof(FrameUniformData));
    }

    const auto device = vkcore.getDevice();
    auto descriptor_pool =
        createFramePool(device, 3);
    const auto frame_layout =
        factory.frameDescriptorSetLayout();
    auto multiview_set = allocateFrameSet(
        device, descriptor_pool.get(), frame_layout,
        multiview_buffer, multiview_bytes.size());
    std::array<vk::UniqueDescriptorSet,
               stereo_view_count>
        sequential_sets;
    for (std::uint32_t view = 0;
         view < stereo_view_count; ++view) {
        sequential_sets[view] = allocateFrameSet(
            device, descriptor_pool.get(), frame_layout,
            sequential_buffers[view],
            sizeof(FrameUniformData));
    }

    const auto usage =
        vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::eTransferSrc |
        vk::ImageUsageFlagBits::eSampled;

    auto &render_targets =
        GET_MODULE(RenderTargetContainer);
    const auto managed_target =
        render_targets.registerRenderTarget(
            "wp203b_layered_target", test_extent,
            "data", "data", 1.0f, std::nullopt,
            test_format, usage,
            vma::MemoryUsage::eAutoPreferDevice,
            false, {}, 1, stereo_view_count);
    REQUIRE(
        render_targets.getMetadata(managed_target)
            .array_layers ==
        stereo_view_count);
    REQUIRE(
        render_targets.getImage(managed_target)
            .array_layers ==
        stereo_view_count);
    const auto managed_left =
        render_targets.getAttachmentImageLayerView(
            managed_target, 0);
    const auto managed_right =
        render_targets.getAttachmentImageLayerView(
            managed_target, 1);
    const auto managed_layered =
        render_targets.getLayeredAttachmentImageView(
            managed_target);
    REQUIRE(managed_left);
    REQUIRE(managed_right);
    REQUIRE(managed_layered);
    REQUIRE(managed_left != managed_right);
    REQUIRE(managed_left != managed_layered);
    REQUIRE_THROWS(
        render_targets.getImageLayerView(
            managed_target, stereo_view_count));

    auto &fullscreen_passes =
        GET_MODULE(FullscreenPassContainer);
    const auto sequential_input_pipeline =
        fullscreen_passes.registerFullscreenPass(
            test_format, sequential_vert,
            sequential_input_frag);
    const auto multiview_input_pipeline =
        fullscreen_passes.registerFullscreenPass(
            test_format, sequential_vert,
            multiview_input_frag,
            {"PELICAN_MULTIVIEW=1",
             "PELICAN_VIEW_COUNT=2",
             "PELICAN_INPUT_0_LAYERED=1"},
            vk::SampleCountFlagBits::e1,
            GraphicsPipelineViewContract::multiview(
                stereo_view_count));
    const PassId sequential_input_pass{
        static_cast<int>(
            sequential_input_pipeline.value)};
    const PassId multiview_input_pass{
        static_cast<int>(
            multiview_input_pipeline.value)};
    const RenderTargetImageViewResolver
        target_views{render_targets};
    auto &frame_graph_resources =
        GET_MODULE(FrameGraphResourceContainer);
    fullscreen_passes.setInputResourcesById(
        sequential_input_pass,
        {managed_target}, {false}, {},
        target_views, frame_graph_resources, {},
        {PassInputViewDimension::sequential_2d});
    fullscreen_passes.setInputResourcesById(
        multiview_input_pass,
        {managed_target}, {false}, {},
        target_views, frame_graph_resources, {},
        {PassInputViewDimension::layered_2d_array},
        GraphicsPipelineViewContract::multiview(
            stereo_view_count));
    REQUIRE(
        fullscreen_passes
            .boundInputImageViewsForTesting(
                sequential_input_pass, 0)
            .front() == managed_left);
    REQUIRE(
        fullscreen_passes
            .boundInputImageViewsForTesting(
                sequential_input_pass, 1)
            .front() == managed_right);
    REQUIRE(
        fullscreen_passes
            .boundInputImageViewsForTesting(
                multiview_input_pass)
            .front() ==
        render_targets.getLayeredImageView(
            managed_target));

    PassDefinition runtime_pass;
    runtime_pass.name = "wp203b_runtime_multiview";
    runtime_pass.output_color = {managed_target};
    runtime_pass.input_targets = {managed_target};
    runtime_pass.input_target_history = {false};
    runtime_pass.pass_info = FullscreenPassInfo{
        .vert_shader = makeShaderReference(
            "engine://fullscreen", ShaderStage::vertex),
        .frag_shader = makeShaderReference(
            "engine://scene_present", ShaderStage::fragment),
    };
    RenderingPassDefinition runtime_definition{
        .name = "wp203b_runtime",
        .passes = {runtime_pass},
    };
    VulkanTargetPlan runtime_plan;
    runtime_plan.graph = runtime_definition.name;
    runtime_plan.view_execution_plan.view_count =
        stereo_view_count;
    runtime_plan.view_execution_plan.uses_multiview = true;
    runtime_plan.resources = {
        {
            .logical_resource = "wp203b_layered_target",
            .view_layout =
                VulkanResourceViewLayout::layered_2d_array,
            .array_layers = stereo_view_count,
        },
    };
    runtime_plan.scopes = {
        {
            .id = "wp203b_runtime_multiview",
            .nodes = {runtime_pass.name},
            .view_execution =
                VulkanScopeViewExecution::multiview,
            .view_count = stereo_view_count,
            .execution_count = 1,
            .view_mask = stereo_view_mask,
        },
    };
    const RenderTargetMetadataResolver target_metadata{
        render_targets};
    const auto compiled_runtime =
        compileRenderingPassRuntime(
            runtime_definition,
            RenderingPassRuntimeDependencies{
                .render_target = &GET_MODULE(RenderTarget),
                .render_target_metadata = &target_metadata,
                .render_target_views = &target_views,
                .shader_library = &library,
                .fullscreen_pass_container =
                    &fullscreen_passes,
                .frame_graph_resources =
                    &frame_graph_resources,
                .path_resolver =
                    &GET_MODULE(PathResolver),
                .target_plan = &runtime_plan,
            });
    REQUIRE(compiled_runtime.passes.size() == 1);
    REQUIRE(
        compiled_runtime.passes.front().view ==
        GraphicsPipelineViewContract::multiview(
            stereo_view_count));
    REQUIRE(
        compiled_runtime.passes.front()
            .definition.input_target_views ==
        std::vector{
            PassInputViewDimension::layered_2d_array});
    REQUIRE(
        fullscreen_passes
            .boundInputImageViewsForTesting(
                compiled_runtime.passes.front().pass_id)
            .front() ==
        render_targets.getLayeredImageView(
            managed_target));

    auto invalid_runtime_plan = runtime_plan;
    invalid_runtime_plan.resources.front().view_layout =
        VulkanResourceViewLayout::sequential_2d;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            runtime_definition,
            RenderingPassRuntimeDependencies{
                .render_target = &GET_MODULE(RenderTarget),
                .render_target_metadata = &target_metadata,
                .render_target_views = &target_views,
                .shader_library = &library,
                .fullscreen_pass_container =
                    &fullscreen_passes,
                .frame_graph_resources =
                    &frame_graph_resources,
                .path_resolver =
                    &GET_MODULE(PathResolver),
                .target_plan = &invalid_runtime_plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "cannot consume a sequential-only input"));

    PassDefinition managed_pass;
    managed_pass.name = "wp203b_attachment_probe";
    managed_pass.output_color = {managed_target};
    const FrameRenderContext managed_frame{
        .extent = test_extent,
    };
    const auto left_attachment =
        createColorAttachments(
            managed_frame, managed_pass, render_targets,
            GraphicsPipelineViewContract{},
            RenderPassViewInvocation{
                stereo_view_count, 0});
    const auto right_attachment =
        createColorAttachments(
            managed_frame, managed_pass, render_targets,
            GraphicsPipelineViewContract{},
            RenderPassViewInvocation{
                stereo_view_count, 1});
    const auto layered_attachment =
        createColorAttachments(
            managed_frame, managed_pass, render_targets,
            GraphicsPipelineViewContract::multiview(
                stereo_view_count),
            RenderPassViewInvocation{
                stereo_view_count, 0});
    REQUIRE(
        left_attachment.front().imageView ==
        managed_left);
    REQUIRE(
        right_attachment.front().imageView ==
        managed_right);
    REQUIRE(
        layered_attachment.front().imageView ==
        managed_layered);

    auto multiview_image = vkcore.allocImage(
        {test_extent.width, test_extent.height, 1},
        test_format, usage,
        vma::MemoryUsage::eAutoPreferDevice, {},
        VulkanProcessType::graphics, {}, 1,
        vk::SampleCountFlagBits::e1,
        stereo_view_count);
    REQUIRE(
        multiview_image.array_layers ==
        stereo_view_count);
    const std::array<std::byte, 4> one_pixel{};
    REQUIRE_THROWS_WITH(
        GET_MODULE(VulkanUtils).safeTransferMemoryToImage(
            multiview_image, one_pixel.data(),
            one_pixel.size(),
            VulkanUtils::ImageTransferInfo{}),
        Catch::Matchers::ContainsSubstring(
            "only accepts single-layer images"));
    auto multiview_attachment = createColorView(
        device, multiview_image,
        vk::ImageViewType::e2DArray, 0,
        stereo_view_count);

    auto sequential_image = vkcore.allocImage(
        {test_extent.width, test_extent.height, 1},
        test_format, usage,
        vma::MemoryUsage::eAutoPreferDevice, {},
        VulkanProcessType::graphics, {}, 1,
        vk::SampleCountFlagBits::e1,
        stereo_view_count);
    std::array<vk::UniqueImageView,
               stereo_view_count>
        sequential_attachments;
    for (std::uint32_t view = 0;
         view < stereo_view_count; ++view) {
        sequential_attachments[view] =
            createColorView(
                device, sequential_image,
                vk::ImageViewType::e2D, view, 1);
    }

    const std::array multiview_views{
        multiview_attachment.get()};
    const std::array multiview_sets{
        multiview_set.get()};
    const auto multiview = renderLayered(
        multiview_image, multiview_views, factory,
        multiview_pipeline, multiview_sets, true);

    const std::array sequential_views{
        sequential_attachments[0].get(),
        sequential_attachments[1].get(),
    };
    const std::array sequential_set_handles{
        sequential_sets[0].get(),
        sequential_sets[1].get(),
    };
    const auto sequential = renderLayered(
        sequential_image, sequential_views, factory,
        sequential_pipeline,
        sequential_set_handles, false);

    REQUIRE(multiview.rendering_count == 1);
    REQUIRE(
        sequential.rendering_count ==
        stereo_view_count);
    REQUIRE(
        multiview.layers[0] ==
        sequential.layers[0]);
    REQUIRE(
        multiview.layers[1] ==
        sequential.layers[1]);
    REQUIRE(
        multiview.layers[0] !=
        multiview.layers[1]);

    const auto first_pixel =
        [](const std::vector<std::uint8_t> &bytes) {
            return std::array{
                bytes.at(0), bytes.at(1),
                bytes.at(2), bytes.at(3)};
        };
    const auto left = first_pixel(
        multiview.layers[0]);
    const auto right = first_pixel(
        multiview.layers[1]);
    REQUIRE(left[0] == 51);
    REQUIRE(left[1] == 76);
    REQUIRE(left[2] == 25);
    REQUIRE(left[3] == 255);
    REQUIRE(right[0] == 204);
    REQUIRE(right[1] == 178);
    REQUIRE(right[2] == 229);
    REQUIRE(right[3] == 255);

    vkcore.waitIdle();
#else
    SUCCEED(
        "WP203b runtime GPU fixture is disabled with the runtime shader compiler");
#endif
}

} // namespace Pelican
