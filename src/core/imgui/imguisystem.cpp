#include "imguisystem.hpp"

#include "imguiruntime.hpp"
#include "planviewer.hpp"
#include "../appflow/enginetime.hpp"
#include "../os/actionmap.hpp"
#include "../os/inputstate.hpp"
#include "../os/window.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace Pelican {

namespace {

constexpr std::string_view toggleActionDocument = R"json({
  "schema": "pelican.input_actions",
  "version": 1,
  "action_sets": [{
    "name": "engine_ui",
    "actions": [{"name": "toggle_engine_ui", "type": "button"}]
  }]
})json";

constexpr std::string_view toggleProfileDocument = R"json({
  "schema": "pelican.input_profile",
  "version": 1,
  "name": "engine_ui",
  "bindings": [{"action": "toggle_engine_ui", "binding": "kbd:f1"}]
})json";

ImGuiKey toImGuiKey(KeyCode code) {
    if (code >= KeyCode::A && code <= KeyCode::Z) {
        return static_cast<ImGuiKey>(ImGuiKey_A +
                                     (static_cast<int>(code) - static_cast<int>(KeyCode::A)));
    }
    if (code >= KeyCode::Num0 && code <= KeyCode::Num9) {
        return static_cast<ImGuiKey>(ImGuiKey_0 +
                                     (static_cast<int>(code) - static_cast<int>(KeyCode::Num0)));
    }
    if (code >= KeyCode::F1 && code <= KeyCode::F12) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 +
                                     (static_cast<int>(code) - static_cast<int>(KeyCode::F1)));
    }
    switch (code) {
    case KeyCode::ArrowUp: return ImGuiKey_UpArrow;
    case KeyCode::ArrowDown: return ImGuiKey_DownArrow;
    case KeyCode::ArrowLeft: return ImGuiKey_LeftArrow;
    case KeyCode::ArrowRight: return ImGuiKey_RightArrow;
    case KeyCode::Space: return ImGuiKey_Space;
    case KeyCode::Enter: return ImGuiKey_Enter;
    case KeyCode::Escape: return ImGuiKey_Escape;
    case KeyCode::Tab: return ImGuiKey_Tab;
    case KeyCode::Backspace: return ImGuiKey_Backspace;
    case KeyCode::LeftShift: return ImGuiKey_LeftShift;
    case KeyCode::RightShift: return ImGuiKey_RightShift;
    case KeyCode::LeftControl: return ImGuiKey_LeftCtrl;
    case KeyCode::RightControl: return ImGuiKey_RightCtrl;
    case KeyCode::LeftAlt: return ImGuiKey_LeftAlt;
    case KeyCode::RightAlt: return ImGuiKey_RightAlt;
    case KeyCode::LeftSuper: return ImGuiKey_LeftSuper;
    case KeyCode::RightSuper: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

int toImGuiMouseButton(KeyCode code) {
    switch (code) {
    case KeyCode::MouseLeft: return ImGuiMouseButton_Left;
    case KeyCode::MouseRight: return ImGuiMouseButton_Right;
    case KeyCode::MouseMiddle: return ImGuiMouseButton_Middle;
    case KeyCode::MouseButton4: return 3;
    case KeyCode::MouseButton5: return 4;
    default: return -1;
    }
}

void feedOrderedInput(const FrameInput &frame_input) {
    auto &io = ImGui::GetIO();
    for (const auto &event : frame_input.ordered_events) {
        switch (event.type) {
        case InputEvent::Type::button: {
            const auto mouse_button = toImGuiMouseButton(event.code);
            if (mouse_button >= 0) {
                io.AddMouseButtonEvent(mouse_button, event.pressed != 0);
                break;
            }
            const auto key = toImGuiKey(event.code);
            if (key != ImGuiKey_None) {
                io.AddKeyEvent(key, event.pressed != 0);
            }
            break;
        }
        case InputEvent::Type::cursor_move:
            io.AddMousePosEvent(event.mouse_x, event.mouse_y);
            break;
        case InputEvent::Type::scroll:
            io.AddMouseWheelEvent(event.axis_x, event.axis_y);
            break;
        case InputEvent::Type::character:
            io.AddInputCharacter(event.codepoint);
            break;
        case InputEvent::Type::axis:
        case InputEvent::Type::gamepad_button:
        case InputEvent::Type::gamepad_axis:
            break;
        }
    }

    const auto &snapshot = frame_input.snapshot;
    io.AddKeyEvent(ImGuiMod_Ctrl, snapshot.getKey(KeyCode::LeftControl) ||
                                      snapshot.getKey(KeyCode::RightControl));
    io.AddKeyEvent(ImGuiMod_Shift, snapshot.getKey(KeyCode::LeftShift) ||
                                       snapshot.getKey(KeyCode::RightShift));
    io.AddKeyEvent(ImGuiMod_Alt, snapshot.getKey(KeyCode::LeftAlt) ||
                                     snapshot.getKey(KeyCode::RightAlt));
    io.AddKeyEvent(ImGuiMod_Super, snapshot.getKey(KeyCode::LeftSuper) ||
                                       snapshot.getKey(KeyCode::RightSuper));
}

} // namespace

struct ImGuiSystem::Impl {
    Window &window;
    InputActionMap toggle_actions;
    vk::Format color_format;
    VkFormat raw_color_format;
    VkPipelineRenderingCreateInfo pipeline_rendering_info{};
    bool platform_initialized = false;
    bool renderer_initialized = false;
    bool frame_started = false;
    bool visible = true;
    bool show_demo = false;
    bool show_stats = true;
    bool show_plan_viewer = true;
    PlanViewer plan_viewer;
    std::uint64_t public_api_calls = 0;

    Impl()
        : window(GET_MODULE(Window)),
          toggle_actions([] {
              auto actions = parseInputActionsString(toggleActionDocument);
              return applyInputProfile(actions, parseInputProfileString(toggleProfileDocument, actions));
          }()),
          color_format(GET_MODULE(RenderTargetContainer)
                           .getMetadata(GET_MODULE(RenderTargetContainer)
                                            .getRenderTargetIdByName("display"))
                           .format),
          raw_color_format(static_cast<VkFormat>(color_format)) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ++public_api_calls;
        auto &io = ImGui::GetIO();
        ++public_api_calls;
        // The pinned mainline backend has neither docking nor multi-viewport
        // enabled; keep cursor ownership in Window for v1 as well.
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();
        ++public_api_calls;

        if (!ImGui_ImplGlfw_InitForVulkan(window.nativeHandle(), false)) {
            throw std::runtime_error("ImGui GLFW backend initialization failed");
        }
        platform_initialized = true;

        auto &vk_core = GET_MODULE(VulkanManageCore);
        pipeline_rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pipeline_rendering_info.colorAttachmentCount = 1;
        pipeline_rendering_info.pColorAttachmentFormats = &raw_color_format;

        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.ApiVersion = VK_API_VERSION_1_3;
        init_info.Instance = static_cast<VkInstance>(vk_core.getInstance());
        init_info.PhysicalDevice = static_cast<VkPhysicalDevice>(vk_core.getPhysDevice());
        init_info.Device = static_cast<VkDevice>(vk_core.getDevice());
        init_info.QueueFamily = vk_core.getGraphicsQueueFamilyIndex();
        init_info.Queue = static_cast<VkQueue>(vk_core.getGraphicsQueue());
        init_info.DescriptorPoolSize = 128;
        init_info.MinImageCount = static_cast<uint32_t>(in_flight_frames_num);
        init_info.ImageCount = static_cast<uint32_t>(in_flight_frames_num);
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.UseDynamicRendering = true;
        init_info.PipelineRenderingCreateInfo = pipeline_rendering_info;
        init_info.MinAllocationSize = 1024 * 1024;
        if (!ImGui_ImplVulkan_Init(&init_info)) {
            throw std::runtime_error("ImGui Vulkan backend initialization failed");
        }
        renderer_initialized = true;
    }

    ~Impl() {
        if (frame_started) {
            ImGui::EndFrame();
        }
        if (renderer_initialized) {
            ImGui_ImplVulkan_Shutdown();
        }
        if (platform_initialized) {
            ImGui_ImplGlfw_Shutdown();
        }
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }
    }
};

ImGuiSystem::ImGuiSystem() : impl(std::make_unique<Impl>()) {}
ImGuiSystem::~ImGuiSystem() = default;

void ImGuiSystem::routeInputAndBeginFrame(InputState &input) {
    const auto frame_input = input.currentFrameInput();
    feedOrderedInput(frame_input);
    impl->public_api_calls += 5;

    const auto toggle_frame = evaluateInputActions(impl->toggle_actions, frame_input.snapshot,
                                                   {"engine_ui"});
    if (toggle_frame.get("toggle_engine_ui").pressed) {
        impl->visible = !impl->visible;
    }

    auto &io = ImGui::GetIO();
    ++impl->public_api_calls;
    const auto logical = impl->window.logicalExtent();
    const auto framebuffer = impl->window.framebufferExtent();
    io.DisplaySize = ImVec2{static_cast<float>(logical.width), static_cast<float>(logical.height)};
    io.DisplayFramebufferScale = ImVec2{
        logical.width == 0 ? 1.0f : static_cast<float>(framebuffer.width) / logical.width,
        logical.height == 0 ? 1.0f : static_cast<float>(framebuffer.height) / logical.height,
    };
    const auto dt = GET_MODULE(EngineTime).dt();
    io.DeltaTime = dt > 0.0 ? static_cast<float>(dt) : 1.0f / 60.0f;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ++impl->public_api_calls;
    impl->frame_started = true;

    if (impl->visible) {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Pelican")) {
                ImGui::MenuItem("Frame Plan Viewer", nullptr, &impl->show_plan_viewer);
                ImGui::MenuItem("Frame Stats", nullptr, &impl->show_stats);
                ImGui::MenuItem("Dear ImGui Demo", nullptr, &impl->show_demo);
                ImGui::Separator();
                ImGui::TextDisabled("F1 hides developer UI");
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
        if (impl->show_demo) ImGui::ShowDemoWindow(&impl->show_demo);
        if (impl->show_stats) {
            ImGui::SetNextWindowPos(ImVec2{12.0f, 36.0f}, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::Begin("Pelican Engine Stats", &impl->show_stats,
                         ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("F1: toggle developer UI");
            ImGui::Separator();
            ImGui::Text("FPS %.1f", io.Framerate);
            ImGui::Text("Frame %llu", static_cast<unsigned long long>(GET_MODULE(EngineTime).frameIndex()));
            ImGui::Text("Frame time %.3f ms", io.DeltaTime * 1000.0f);
            ImGui::End();
        }
        if (impl->show_plan_viewer) impl->plan_viewer.draw(&impl->show_plan_viewer);
        impl->public_api_calls += 12;
    }

    applyImGuiCaptureForActions(input, io.WantCaptureKeyboard, io.WantCaptureMouse);
}

void ImGuiSystem::render(vk::CommandBuffer command_buffer, vk::ImageView target_view,
                         vk::Extent2D target_extent, vk::Format target_format) {
    if (!impl->frame_started) {
        throw std::runtime_error("ImGui render called without an interactive input frame");
    }
    if (target_format != impl->color_format) {
        throw std::runtime_error("ImGui display target format changed without backend recreation");
    }

    ImGui::Render();
    ++impl->public_api_calls;
    impl->frame_started = false;

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = static_cast<VkImageView>(target_view);
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.extent = VkExtent2D{target_extent.width, target_extent.height};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &color_attachment;

    const auto raw_command_buffer = static_cast<VkCommandBuffer>(command_buffer);
    vkCmdBeginRendering(raw_command_buffer, &rendering_info);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), raw_command_buffer);
    ++impl->public_api_calls;
    vkCmdEndRendering(raw_command_buffer);
}

std::uint64_t ImGuiSystem::publicApiCallCountForTesting() const noexcept {
    return impl->public_api_calls;
}

} // namespace Pelican
