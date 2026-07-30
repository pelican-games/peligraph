#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/renderer/shadowdepthpasscontainer.hpp"
#include "../src/core/renderer/velocitypasscontainer.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/rendercompilerprogram.hpp"
#include "../src/core/renderingpass/renderingpassconfigregistration.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/renderingpass/vulkannativescopeexecutor.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/rendertarget.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

std::filesystem::path makeTempProjectDir() {
    const auto suffix =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    auto dir =
        std::filesystem::temp_directory_path() /
        ("pelican_native_scope_" +
         std::to_string(suffix));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeTextFile(
    const std::filesystem::path &path,
    const std::string &contents) {
    std::ofstream file{path, std::ios::binary};
    file << contents;
}

nlohmann::json makeProjectConfig(
    const std::filesystem::path &scene_path,
    const std::filesystem::path &asset_path) {
    return nlohmann::json{
        {"basic_config",
         {
             {"window_size",
              {{"width", 16}, {"height", 16}}},
             {"scene_data_json",
              scene_path.generic_string()},
             {"asset_data_json",
              asset_path.generic_string()},
         }},
    };
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

const char *pipelineReloadFragmentShader() {
    return R"glsl(
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.2, 0.4, 0.6, 1.0);
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

inline constexpr std::string_view
    nativeClearScopeImplementation =
        "pelican.test.vulkan.clear_attachment@1";

nlohmann::json nativeScopeRenderingConfig() {
    return nlohmann::json::parse(R"json(
{
  "render_targets": [
    {
      "name": "native_color",
      "extent_scale": 1.0,
      "format": "R8G8B8A8_UNORM",
      "format_class": "data",
      "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
    }
  ],
  "rendering_passes": [
    {
      "name": "native_scope_main",
      "passes": [
        {
          "name": "native_clear",
          "type": "fullscreen",
          "output": {"color": "native_color", "depth": null},
          "shader": {
            "vertex": "shaders/native_fullscreen",
            "fragment": "shaders/native_fallback"
          }
        },
        {
          "name": "native_present",
          "type": "fullscreen",
          "input": ["native_color"],
          "input_footprints": {"native_color": "arbitrary"},
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/native_fullscreen",
            "fragment": "shaders/native_copy"
          }
        }
      ]
    }
  ]
}
)json");
}


class NativeClearVulkanCompilerProgram final
    : public RenderCompilerProgram {
    std::array<float, 4> clear_color_;

  public:
    explicit NativeClearVulkanCompilerProgram(
        std::array<float, 4> clear_color)
        : clear_color_{clear_color} {}

    mutable std::size_t compile_calls = 0;

    RenderCompilerProgramSelection selection(
        const RenderCompilerBackendContext
            &backend_context) const override {
        auto selected =
            defaultVulkanRenderCompilerProgram()
                .selection(backend_context);
        selected.name =
            "test.native_clear_vulkan";
        selected.implementation =
            "test.native_clear_vulkan_v1";
        return selected;
    }

    RenderCompilerProgramOutput compile(
        const RenderCompilerProgramInput
            &input) const override {
        ++compile_calls;
        const auto &backend =
            requireVulkanRenderCompilerBackendContext(
                input.backend_context);
        auto output =
            defaultVulkanRenderCompilerProgram()
                .compile(input);
        for (auto &variant : output.variants) {
            if (variant.physical_package == nullptr) {
                continue;
            }
            auto &physical =
                requireVulkanRenderCompilerPhysicalPackage(
                    *variant.physical_package);
            if (!physical.target_plans.contains(
                    "native_scope_main")) {
                continue;
            }
            const auto &context =
                requireVulkanTargetPlanVerificationContext(
                    physical,
                    "native_scope_main");
            const auto &automatic =
                *context.automatic_plan;
            const auto scope =
                std::find_if(
                    automatic.scopes.begin(),
                    automatic.scopes.end(),
                    [](const auto &candidate) {
                        return std::find(
                                   candidate.nodes.begin(),
                                   candidate.nodes.end(),
                                   "native_clear") !=
                               candidate.nodes.end();
                    });
            if (scope == automatic.scopes.end() ||
                scope->nodes !=
                    std::vector<std::string>{
                        "native_clear"}) {
                throw std::runtime_error(
                    "NativeScope clear fixture requires an isolated "
                    "native_clear physical scope");
            }

            std::map<
                std::string,
                LogicalAccessMode,
                std::less<>>
                accesses;
            const auto merge =
                [&](std::string_view resource,
                    LogicalAccessMode access) {
                    const auto [found, inserted] =
                        accesses.emplace(
                            std::string{resource},
                            access);
                    if (!inserted &&
                        found->second != access) {
                        found->second =
                            LogicalAccessMode::
                                read_write;
                    }
                };
            for (const auto &node_name :
                 scope->nodes) {
                const auto node =
                    std::find_if(
                        context.logical_graph
                            ->nodes.begin(),
                        context.logical_graph
                            ->nodes.end(),
                        [&](const auto &candidate) {
                            return candidate.name ==
                                   node_name;
                        });
                if (node ==
                    context.logical_graph
                        ->nodes.end()) {
                    throw std::runtime_error(
                        "NativeScope fixture physical node is absent "
                        "from its canonical logical graph");
                }
                for (const auto &use : node->uses) {
                    if (use.input_value) {
                        merge(
                            use.input_value
                                ->resource,
                            use.access);
                    }
                    if (use.output_value &&
                        (!use.input_value ||
                         use.input_value
                                 ->resource !=
                             use.output_value
                                 ->resource)) {
                        merge(
                            use.output_value
                                ->resource,
                            use.access);
                    }
                }
            }

            VulkanNativeScopeDeclaration declaration{
                .scope = scope->id,
                .implementation =
                    std::string{
                        nativeClearScopeImplementation},
                .queue_capability =
                    "pelican.vulkan.graphics@1",
                .synchronization =
                    VulkanNativeScopeSynchronizationMode::
                        automatic,
                .capture_compatible = true,
                .device_loss_recoverable = true,
                .hot_reloadable = true,
                .implementation_config =
                    nlohmann::ordered_json{
                        {"clear_color",
                         nlohmann::ordered_json::array(
                             {clear_color_[0],
                              clear_color_[1],
                              clear_color_[2],
                              clear_color_[3]})},
                    },
            };
            if (!backend
                     .enabled_device_extensions
                     .empty()) {
                declaration.required_extensions = {
                    backend
                        .enabled_device_extensions
                        .front(),
                };
            }
            for (const auto &[name, access] :
                 accesses) {
                const auto resource =
                    std::find_if(
                        context.logical_graph
                            ->resources.begin(),
                        context.logical_graph
                            ->resources.end(),
                        [&](const auto &candidate) {
                            return candidate.name ==
                                   name;
                        });
                if (resource ==
                    context.logical_graph
                        ->resources.end()) {
                    throw std::runtime_error(
                        "NativeScope fixture resource is absent from "
                        "its canonical logical graph");
                }
                declaration.resources.push_back(
                    VulkanNativeScopeResourceBoundary{
                        .logical_resource = name,
                        .semantic_type =
                            semanticTypeIdName(
                                resource->type
                                    .semantic),
                        .access = access,
                    });
            }

            auto package =
                ejectVulkanCompletePhysicalPlanPackage(
                    automatic);
            package.native_scopes.push_back(
                std::move(declaration));
            installVerifiedVulkanCompletePhysicalPlanPackage(
                physical,
                "native_scope_main",
                std::move(package));
        }
        return output;
    }
};

struct NativeClearExecutorStats {
    std::atomic_size_t prepare_calls{0};
    std::atomic_size_t record_calls{0};
    std::atomic_size_t destroyed_executors{0};
};

class NativeClearExecutor final
    : public VulkanNativeScopeExecutor {
    std::array<float, 4> clear_color_;
    const DebugUtilsDispatch *debug_utils_ =
        nullptr;
    std::shared_ptr<NativeClearExecutorStats>
        stats_;

  public:
    NativeClearExecutor(
        std::array<float, 4> clear_color,
        const DebugUtilsDispatch &debug_utils,
        std::shared_ptr<NativeClearExecutorStats>
            stats)
        : clear_color_{clear_color},
          debug_utils_{&debug_utils},
          stats_{std::move(stats)} {}

    ~NativeClearExecutor() override {
        ++stats_->destroyed_executors;
    }

    void record(
        const VulkanNativeScopeRecordContext
            &context) const override {
        if (!context.command_buffer ||
            context.scope == nullptr ||
            context.declaration == nullptr ||
            context.resources.size() != 1) {
            throw std::runtime_error(
                "NativeScope clear executor received an invalid "
                "record context");
        }
        const auto &image =
            context.resources.front().image;
        if (!image.attachment_view ||
            image.attachment_layout !=
                vk::ImageLayout::
                    eColorAttachmentOptimal ||
            image.samples !=
                vk::SampleCountFlagBits::e1) {
            throw std::runtime_error(
                "NativeScope clear executor requires one single-sample "
                "color attachment");
        }

        auto image_view =
            image.attachment_view;
        auto extent =
            vk::Extent2D{
                image.extent.width,
                image.extent.height};
        if (!image.attachments.empty()) {
            const auto &attachment =
                image.attachments.front();
            if (attachment.aspect !=
                    VulkanPhysicalAttachmentAspect::
                        color ||
                !attachment.image_view) {
                throw std::runtime_error(
                    "NativeScope clear executor received a non-color "
                    "physical attachment");
            }
            image_view =
                attachment.image_view;
            extent = attachment.extent;
        }

        ScopedCommandDebugLabel label{
            *debug_utils_,
            context.command_buffer,
            "NativeScope/test.clear_attachment"};
        vk::RenderingAttachmentInfo color;
        color.imageView = image_view;
        color.imageLayout =
            image.attachment_layout;
        color.loadOp =
            vk::AttachmentLoadOp::eClear;
        color.storeOp =
            vk::AttachmentStoreOp::eStore;
        color.clearValue.color =
            vk::ClearColorValue{
                clear_color_};

        vk::RenderingInfo rendering;
        rendering.renderArea =
            vk::Rect2D{{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.viewMask =
            context.scope->view_mask;
        rendering.setColorAttachments(
            color);
        context.command_buffer
            .beginRendering(rendering);
        context.command_buffer
            .endRendering();
        ++stats_->record_calls;
    }
};

class NativeClearProviderOwner {
    VulkanNativeScopeExecutorRegistry
        *registry_ = nullptr;
    internal::RegistrationOwner owner_ =
        internal::engineRegistrationOwner;
    std::shared_ptr<const void> code_lease_;

  public:
    explicit NativeClearProviderOwner(
        std::shared_ptr<NativeClearExecutorStats>
            stats)
        : registry_{
              &vulkanNativeScopeExecutorRegistry()},
          owner_{
              internal::allocateRegistrationOwner()},
          code_lease_{std::make_shared<int>(238)} {
        try {
            const auto capabilities =
                vulkanNativeScopeExecutorCapability(
                    VulkanNativeScopeExecutorCapability::
                        automatic_synchronization) |
                vulkanNativeScopeExecutorCapability(
                    VulkanNativeScopeExecutorCapability::
                        capture_compatible) |
                vulkanNativeScopeExecutorCapability(
                    VulkanNativeScopeExecutorCapability::
                        device_loss_recoverable) |
                vulkanNativeScopeExecutorCapability(
                    VulkanNativeScopeExecutorCapability::
                        hot_reloadable);
            (void)registry_->registerProvider(
                VulkanNativeScopeExecutorProvider{
                    .provider =
                        "test.vulkan.native_scope",
                    .implementation =
                        std::string{
                            nativeClearScopeImplementation},
                    .capabilities =
                        capabilities,
                    .generation_lease =
                        code_lease_,
                    .prepare =
                        [stats = std::move(stats)](
                            const VulkanNativeScopePrepareContext
                                &context)
                        -> std::shared_ptr<
                            const VulkanNativeScopeExecutor> {
                            if (context.device
                                    .vulkan_core ==
                                    nullptr ||
                                context.resources
                                        .size() != 1 ||
                                !context.declaration
                                     .implementation_config
                                     .is_object()) {
                                throw std::runtime_error(
                                    "NativeScope clear provider received "
                                    "an incomplete prepare context");
                            }
                            const auto found =
                                context.declaration
                                    .implementation_config
                                    .find(
                                        "clear_color");
                            if (found ==
                                    context.declaration
                                        .implementation_config
                                        .end() ||
                                !found->is_array() ||
                                found->size() != 4) {
                                throw std::runtime_error(
                                    "NativeScope clear provider requires "
                                    "clear_color[4]");
                            }
                            std::array<float, 4>
                                clear{};
                            for (std::size_t index = 0;
                                 index < clear.size();
                                 ++index) {
                                if (!found->at(index)
                                         .is_number()) {
                                    throw std::runtime_error(
                                        "NativeScope clear color must be "
                                        "numeric");
                                }
                                clear[index] =
                                    found->at(index)
                                        .get<float>();
                                if (!std::isfinite(
                                        clear[index]) ||
                                    clear[index] < 0.0f ||
                                    clear[index] > 1.0f) {
                                    throw std::runtime_error(
                                        "NativeScope clear color is out "
                                        "of range");
                                }
                            }
                            ++stats->prepare_calls;
                            return std::make_shared<
                                NativeClearExecutor>(
                                clear,
                                context.device
                                    .vulkan_core
                                    ->getDebugUtils(),
                                stats);
                        },
                },
                owner_);
            registry_->activateOwner(owner_);
        } catch (...) {
            registry_->releaseOwner(owner_);
            internal::releaseRegistrationOwner(
                owner_);
            throw;
        }
    }

    ~NativeClearProviderOwner() {
        registry_->releaseOwner(owner_);
        internal::releaseRegistrationOwner(
            owner_);
    }

    NativeClearProviderOwner(
        const NativeClearProviderOwner &) = delete;
    NativeClearProviderOwner &operator=(
        const NativeClearProviderOwner &) = delete;
};

class VulkanValidationErrorCollector {
    vk::Instance instance_;
    VkDebugUtilsMessengerEXT messenger_ =
        VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT
        destroy_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;

    static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT
            *data,
        void *user_data) noexcept {
        if ((severity &
             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ==
            0) {
            return VK_FALSE;
        }
        auto *collector =
            static_cast<
                VulkanValidationErrorCollector *>(
                user_data);
        try {
            std::scoped_lock lock{
                collector->mutex_};
            std::ostringstream message;
            if (data != nullptr &&
                data->pMessageIdName != nullptr) {
                message << data->pMessageIdName
                        << ": ";
            }
            message
                << (data != nullptr &&
                            data->pMessage != nullptr
                        ? data->pMessage
                        : "Vulkan validation error");
            collector->messages_.push_back(
                message.str());
        } catch (...) {
        }
        return VK_FALSE;
    }

  public:
    explicit VulkanValidationErrorCollector(
        vk::Instance instance)
        : instance_{instance} {
        const auto create =
            reinterpret_cast<
                PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    static_cast<VkInstance>(
                        instance_),
                    "vkCreateDebugUtilsMessengerEXT"));
        destroy_ =
            reinterpret_cast<
                PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    static_cast<VkInstance>(
                        instance_),
                    "vkDestroyDebugUtilsMessengerEXT"));
        if (create == nullptr ||
            destroy_ == nullptr) {
            throw std::runtime_error(
                "VK_EXT_debug_utils messenger is unavailable");
        }
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = &callback;
        info.pUserData = this;
        const auto result =
            create(
                static_cast<VkInstance>(
                    instance_),
                &info, nullptr, &messenger_);
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "failed to create Vulkan validation error collector");
        }
    }

    ~VulkanValidationErrorCollector() {
        if (messenger_ != VK_NULL_HANDLE &&
            destroy_ != nullptr) {
            destroy_(
                static_cast<VkInstance>(
                    instance_),
                messenger_, nullptr);
        }
    }

    std::vector<std::string>
    messages() const {
        std::scoped_lock lock{mutex_};
        return messages_;
    }
};


} // namespace

TEST_CASE(
    "WP238e NativeScope records Vulkan commands and rebuilds through "
    "renderer generations",
    "[wp238e][headless][render][native-scope][vulkan][validation][capture]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();
    std::filesystem::path temp_dir;
    bool runtime_ready = false;
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
            R"json({"models":[]})json");
        writeTextFile(
            temp_dir / "shaders" /
                "native_fullscreen.vert",
            gpuArenaFullscreenVertexShader());
        writeTextFile(
            temp_dir / "shaders" /
                "native_fallback.frag",
            pipelineReloadFragmentShader());
        writeTextFile(
            temp_dir / "shaders" /
                "native_copy.frag",
            gpuArenaCopyFragmentShader());
        const auto config =
            nativeScopeRenderingConfig();
        writeTextFile(
            temp_dir / "native_scope.json",
            config.dump(2));

        auto project =
            makeProjectConfig(
                "scene.json", "assets.json");
        project["basic_config"]
               ["default_scene_id"] =
            "default_scene";
        project["basic_config"]
               ["rendering_config_json"] =
            "native_scope.json";
        project["basic_config"]
               ["default_rendering_pass"] =
            "native_scope_main";
        GET_MODULE(ProjectSource)
            .setSourceByData(project.dump());
        GET_MODULE(PathResolver).setup(
            temp_dir, false);
        auto &launch =
            GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent =
            vk::Extent2D{16, 16};
        launch.headless_frames = 8;
        launch.gpu_labels = true;
        auto &engine_time =
            GET_MODULE(EngineTime);
        engine_time.setup(
            EngineTime::Mode::fixed_step,
            1.0 / 60.0);

        auto &renderer = GET_MODULE(Renderer);
        runtime_ready = true;
        auto &vulkan =
            GET_MODULE(VulkanManageCore);
        std::optional<
            VulkanValidationErrorCollector>
            validation;
        if (vulkan.getDebugUtils()
                .getStatus()
                .enabled) {
            validation.emplace(
                vulkan.getInstance());
        }

        auto stats =
            std::make_shared<
                NativeClearExecutorStats>();
        NativeClearProviderOwner provider{
            stats};
        auto &runtime =
            GET_MODULE(FrameGraphRuntimeContainer);
        const auto register_program =
            [&](const RenderCompilerProgram
                    *program) {
                RenderingPassConfigRegistrationDependencies::
                    Options options;
                options.render_compiler_program =
                    program;
                return registerRenderingPassConfigFromJsonData(
                    config.dump(),
                    {16, 16},
                    gpuArenaRegistrationDependencies(
                        std::move(options)));
            };
        const auto render_once =
            [&] {
                engine_time.advance();
                renderer.render();
                vulkan.waitIdle();
            };
        const auto center_pixel =
            [&] {
                const auto pixels =
                    GET_MODULE(RenderTarget)
                        .readbackLastFrameRGBA8();
                REQUIRE(
                    pixels.size() ==
                    16u * 16u * 4u);
                const auto offset =
                    (8u * 16u + 8u) * 4u;
                return std::array<std::uint8_t, 4>{
                    pixels[offset],
                    pixels[offset + 1],
                    pixels[offset + 2],
                    pixels[offset + 3],
                };
            };

        NativeClearVulkanCompilerProgram
            first_compiler{
                {1.0f, 0.0f, 1.0f, 1.0f}};
        const auto first_registration =
            register_program(
                &first_compiler);
        REQUIRE(
            first_compiler.compile_calls == 1);
        REQUIRE(
            first_registration
                .target_plans.size() == 1);
        REQUIRE(
            stats->prepare_calls.load() == 1);
        auto first_generation =
            runtime.snapshot();
        REQUIRE(first_generation != nullptr);
        const auto *first_program =
            first_generation->find(
                first_generation
                    ->name_to_id.at(
                        "native_scope_main"));
        REQUIRE(first_program != nullptr);
        REQUIRE(
            first_program->frame_graph
                .native_scopes != nullptr);
        const auto prepared_scopes =
            first_program->frame_graph
                .native_scopes->scopes();
        REQUIRE(
            prepared_scopes.size() == 1);
        const auto enabled_extensions =
            vulkan.getEnabledDeviceExtensions();
        if (enabled_extensions.empty()) {
            CHECK(
                prepared_scopes.front()
                    .declaration()
                    .required_extensions
                    .empty());
        } else {
            REQUIRE(
                prepared_scopes.front()
                    .declaration()
                    .required_extensions
                    .size() == 1);
            CHECK(
                prepared_scopes.front()
                    .declaration()
                    .required_extensions
                    .front() ==
                enabled_extensions.front());
        }

        render_once();
        REQUIRE(
            center_pixel() ==
            (std::array<std::uint8_t, 4>{
                255, 0, 255, 255}));
        REQUIRE(
            stats->record_calls.load() == 1);
        const auto plan_json =
            renderer.currentFramePlanJson();
        REQUIRE(
            plan_json.contains(
                "native_scope_executors"));
        const auto &scope_json =
            plan_json.at(
                "native_scope_executors")
                .at("scopes")
                .at(0);
        CHECK(
            scope_json.at(
                "implementation")
                .get<std::string>() ==
            std::string{
                nativeClearScopeImplementation});
        CHECK(
            scope_json.at("provider") ==
            "test.vulkan.native_scope");
        CHECK(
            scope_json.at(
                "synchronization") ==
            "automatic");

        NativeClearVulkanCompilerProgram
            second_compiler{
                {0.0f, 1.0f, 0.0f, 1.0f}};
        (void)register_program(
            &second_compiler);
        REQUIRE(
            second_compiler.compile_calls == 1);
        REQUIRE(
            stats->prepare_calls.load() == 2);
        auto second_generation =
            runtime.snapshot();
        REQUIRE(
            second_generation != nullptr);
        REQUIRE(
            second_generation !=
            first_generation);
        REQUIRE(
            stats->destroyed_executors
                    .load() == 0);

        first_generation.reset();
        render_once();
        REQUIRE(
            center_pixel() ==
            (std::array<std::uint8_t, 4>{
                0, 255, 0, 255}));
        // Rotate every offscreen in-flight slot so the first generation's
        // submitted-frame lease can retire.
        render_once();
        render_once();
        REQUIRE(
            stats->destroyed_executors
                    .load() == 1);

        (void)register_program(nullptr);
        second_generation.reset();
        for (int index = 0;
             index < 3; ++index) {
            render_once();
        }
        REQUIRE(
            stats->destroyed_executors
                    .load() == 2);

        if (validation) {
            const auto errors =
                validation->messages();
            for (const auto &error : errors) {
                INFO(error);
            }
            REQUIRE(errors.empty());
        }
        vulkan.waitIdle();
        std::filesystem::remove_all(
            temp_dir);
    } catch (const std::exception &error) {
        if (!temp_dir.empty()) {
            std::filesystem::remove_all(
                temp_dir);
        }
        if (runtime_ready) {
            throw;
        }
        SKIP(
            std::string{
                "Vulkan NativeScope execution unavailable: "} +
            error.what());
    }
#endif
}

} // namespace Pelican
