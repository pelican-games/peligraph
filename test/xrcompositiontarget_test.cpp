#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/openxr/openxrcompositiontarget.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr vk::Format test_format = vk::Format::eR8G8B8A8Srgb;

struct FakeRuntime {
    std::string failure;
    std::vector<std::string> calls;
    std::deque<XrSessionState> events;
    std::array<std::uint32_t, 2> wait_counts{};
    std::array<bool, 2> timeout_once{};
    std::array<XrSwapchainCreateInfo, 2> swapchain_create_infos{};
    std::uint32_t create_count = 0;
    std::uint32_t enumerate_images_count = 0;
    bool should_render = true;
    bool saw_projection_layer = false;
    bool saw_depth_chain = false;
    bool saw_zero_layer = false;
    bool graphics_release_layout_is_runtime_layout = false;
    bool composition_depth_enabled = false;
    float expected_near_z = 0.05F;
    float expected_far_z = 1000.0F;
};

thread_local FakeRuntime *active_fake = nullptr;

struct FakeScope {
    explicit FakeScope(FakeRuntime &fake) { active_fake = &fake; }
    ~FakeScope() { active_fake = nullptr; }
};

XrInstance fakeInstance() {
    return reinterpret_cast<XrInstance>(std::uintptr_t{0x101});
}
XrSession fakeSession() {
    return reinterpret_cast<XrSession>(std::uintptr_t{0x201});
}
XrSpace fakeSpace() { return reinterpret_cast<XrSpace>(std::uintptr_t{0x301}); }
XrSwapchain fakeSwapchain(std::uint32_t kind) {
    return reinterpret_cast<XrSwapchain>(std::uintptr_t{0x401 + kind});
}
VkInstance fakeVkInstance() {
    return reinterpret_cast<VkInstance>(std::uintptr_t{0x501});
}
VkPhysicalDevice fakeVkPhysicalDevice() {
    return reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x502});
}
VkDevice fakeVkDevice() {
    return reinterpret_cast<VkDevice>(std::uintptr_t{0x503});
}

std::uint32_t swapchainKind(XrSwapchain swapchain) {
    if (swapchain == fakeSwapchain(0)) return 0;
    if (swapchain == fakeSwapchain(1)) return 1;
    FAIL("unknown fake swapchain");
    return 0;
}

std::string eye(std::uint32_t view) { return view == 0 ? "L" : "R"; }
std::string swapchainName(std::uint32_t kind) {
    return kind == 0 ? "color" : "depth";
}

XrResult XRAPI_CALL fakeCreateSession(XrInstance, const XrSessionCreateInfo *,
                                      XrSession *session) {
    *session = fakeSession();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeDestroySession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL fakeEnumerateReferenceSpaces(
    XrSession, std::uint32_t capacity, std::uint32_t *count,
    XrReferenceSpaceType *spaces) {
    *count = 1;
    if (capacity != 0) spaces[0] = XR_REFERENCE_SPACE_TYPE_LOCAL;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeCreateReferenceSpace(XrSession,
                                             const XrReferenceSpaceCreateInfo *,
                                             XrSpace *space) {
    *space = fakeSpace();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeDestroySpace(XrSpace) { return XR_SUCCESS; }
XrResult XRAPI_CALL fakePollEvent(XrInstance, XrEventDataBuffer *event) {
    if (active_fake->events.empty()) return XR_EVENT_UNAVAILABLE;
    XrEventDataSessionStateChanged changed{
        XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    changed.session = fakeSession();
    changed.state = active_fake->events.front();
    active_fake->events.pop_front();
    *reinterpret_cast<XrEventDataSessionStateChanged *>(event) = changed;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeBeginSession(XrSession, const XrSessionBeginInfo *) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeEndSession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL fakeWaitFrame(XrSession, const XrFrameWaitInfo *,
                                  XrFrameState *state) {
    state->predictedDisplayTime = 1234567;
    state->predictedDisplayPeriod = 11111111;
    state->shouldRender = active_fake->should_render ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeBeginFrame(XrSession, const XrFrameBeginInfo *) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeEndFrame(XrSession, const XrFrameEndInfo *info) {
    if (info->layerCount == 0) {
        CHECK(info->layers == nullptr);
        active_fake->calls.emplace_back("end_zero");
        active_fake->saw_zero_layer = true;
        return XR_SUCCESS;
    }

    active_fake->calls.emplace_back("end_projection");
    REQUIRE(info->layerCount == 1);
    REQUIRE(info->layers != nullptr);
    REQUIRE(info->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    const auto &layer =
        *reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[0]);
    CHECK(layer.space == fakeSpace());
    REQUIRE(layer.viewCount == 2);
    for (std::uint32_t view = 0; view < 2; ++view) {
        CHECK(layer.views[view].subImage.swapchain == fakeSwapchain(0));
        CHECK(layer.views[view].subImage.imageArrayIndex == view);
        CHECK(layer.views[view].subImage.imageRect.offset.x == 0);
        CHECK(layer.views[view].subImage.imageRect.offset.y == 0);
        CHECK(layer.views[view].subImage.imageRect.extent.width ==
              72);
        CHECK(layer.views[view].subImage.imageRect.extent.height == 64);
        if (active_fake->composition_depth_enabled) {
            REQUIRE(layer.views[view].next != nullptr);
            const auto &depth =
                *reinterpret_cast<
                    const XrCompositionLayerDepthInfoKHR *>(
                    layer.views[view].next);
            CHECK(depth.type ==
                  XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR);
            CHECK(depth.subImage.swapchain ==
                  fakeSwapchain(1));
            CHECK(depth.subImage.imageArrayIndex == view);
            CHECK(depth.subImage.imageRect.extent.width == 72);
            CHECK(depth.subImage.imageRect.extent.height == 64);
            CHECK(depth.minDepth == 0.0F);
            CHECK(depth.maxDepth == 1.0F);
            CHECK(depth.nearZ ==
                  active_fake->expected_near_z);
            CHECK(depth.farZ ==
                  active_fake->expected_far_z);
            active_fake->saw_depth_chain = true;
        } else {
            CHECK(layer.views[view].next == nullptr);
        }
    }
    active_fake->saw_projection_layer = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeLocateViews(XrSession, const XrViewLocateInfo *,
                                    XrViewState *state, std::uint32_t capacity,
                                    std::uint32_t *count, XrView *views) {
    REQUIRE(capacity >= 2);
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    views[0].pose.orientation.w = 1.0F;
    views[1].pose.orientation.w = 1.0F;
    views[0].pose.position.x = -0.032F;
    views[1].pose.position.x = 0.032F;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeEnumerateViewConfigurationViews(
    XrInstance, XrSystemId, XrViewConfigurationType type, std::uint32_t capacity,
    std::uint32_t *count, XrViewConfigurationView *views) {
    CHECK(type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
    *count = 2;
    if (capacity == 0) return XR_SUCCESS;
    REQUIRE(capacity >= 2);
    for (std::uint32_t view = 0; view < 2; ++view) {
        views[view].recommendedImageRectWidth = 64 + view * 8;
        views[view].maxImageRectWidth = 128;
        views[view].recommendedImageRectHeight = 64;
        views[view].maxImageRectHeight = 128;
        views[view].recommendedSwapchainSampleCount = 4;
        views[view].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeEnumerateSwapchainFormats(XrSession, std::uint32_t capacity,
                                                  std::uint32_t *count,
                                                  std::int64_t *formats) {
    *count = 3;
    if (capacity == 0) return XR_SUCCESS;
    REQUIRE(capacity >= 3);
    formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
    formats[1] = static_cast<std::int64_t>(static_cast<VkFormat>(test_format));
    formats[2] = VK_FORMAT_D32_SFLOAT;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeCreateSwapchain(XrSession,
                                        const XrSwapchainCreateInfo *info,
                                        XrSwapchain *swapchain) {
    const auto kind = active_fake->create_count++;
    active_fake->calls.emplace_back(
        "create:" + swapchainName(kind));
    REQUIRE(kind < 2);
    active_fake->swapchain_create_infos[kind] = *info;
    if (active_fake->failure ==
        "create_" + swapchainName(kind)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *swapchain = fakeSwapchain(kind);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeDestroySwapchain(XrSwapchain swapchain) {
    active_fake->calls.emplace_back(
        "destroy:" +
        swapchainName(swapchainKind(swapchain)));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeEnumerateSwapchainImages(
    XrSwapchain, std::uint32_t, std::uint32_t *, XrSwapchainImageBaseHeader *) {
    ++active_fake->enumerate_images_count;
    FAIL("protocol fake must not enumerate or manufacture Vulkan images");
    return XR_ERROR_RUNTIME_FAILURE;
}
XrResult XRAPI_CALL fakeAcquireSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *,
    std::uint32_t *image_index) {
    const auto kind = swapchainKind(swapchain);
    active_fake->calls.emplace_back(
        "acquire:" + swapchainName(kind));
    if (active_fake->failure ==
        "acquire_" + swapchainName(kind)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *image_index = 10 + kind;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeWaitSwapchainImage(XrSwapchain swapchain,
                                           const XrSwapchainImageWaitInfo *) {
    const auto kind = swapchainKind(swapchain);
    active_fake->calls.emplace_back(
        "wait:" + swapchainName(kind));
    if (active_fake->timeout_once[kind] &&
        active_fake->wait_counts[kind]++ == 0) {
        return XR_TIMEOUT_EXPIRED;
    }
    if (active_fake->failure ==
        "wait_" + swapchainName(kind)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (active_fake->failure ==
        "loss_wait_" + swapchainName(kind)) {
        return XR_ERROR_SESSION_LOST;
    }
    if (active_fake->failure ==
        "loss_pending_wait_" + swapchainName(kind)) {
        return XR_SESSION_LOSS_PENDING;
    }
    if (active_fake->failure ==
        "instance_loss_wait_" + swapchainName(kind)) {
        return XR_ERROR_INSTANCE_LOST;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeReleaseSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *) {
    const auto kind = swapchainKind(swapchain);
    active_fake->calls.emplace_back(
        "release:" + swapchainName(kind));
    if (active_fake->failure ==
        "release_" + swapchainName(kind)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
}

PFN_xrVoidFunction fakeFunction(const std::string &name) {
    if (name == "xrCreateSession")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateSession);
    if (name == "xrDestroySession")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroySession);
    if (name == "xrPollEvent")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakePollEvent);
    if (name == "xrBeginSession")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeBeginSession);
    if (name == "xrEndSession")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeEndSession);
    if (name == "xrWaitFrame")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeWaitFrame);
    if (name == "xrBeginFrame")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeBeginFrame);
    if (name == "xrEndFrame")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeEndFrame);
    if (name == "xrLocateViews")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeLocateViews);
    if (name == "xrEnumerateReferenceSpaces")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeEnumerateReferenceSpaces);
    if (name == "xrCreateReferenceSpace")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateReferenceSpace);
    if (name == "xrDestroySpace")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroySpace);
    if (name == "xrEnumerateViewConfigurationViews")
        return reinterpret_cast<PFN_xrVoidFunction>(
            &fakeEnumerateViewConfigurationViews);
    if (name == "xrEnumerateSwapchainFormats")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeEnumerateSwapchainFormats);
    if (name == "xrCreateSwapchain")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateSwapchain);
    if (name == "xrDestroySwapchain")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroySwapchain);
    if (name == "xrEnumerateSwapchainImages")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeEnumerateSwapchainImages);
    if (name == "xrAcquireSwapchainImage")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeAcquireSwapchainImage);
    if (name == "xrWaitSwapchainImage")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeWaitSwapchainImage);
    if (name == "xrReleaseSwapchainImage")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeReleaseSwapchainImage);
    return nullptr;
}

XrResult XRAPI_CALL fakeGetInstanceProcAddr(XrInstance instance, const char *name,
                                            PFN_xrVoidFunction *function) {
    CHECK(instance == fakeInstance());
    *function = fakeFunction(name);
    return *function == nullptr ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_SUCCESS;
}

Pelican::OpenXr::XrSessionDependencies sessionDependencies() {
    return {
        .get_instance_proc_addr = &fakeGetInstanceProcAddr,
        .instance = fakeInstance(),
        .system_id = 17,
        .vulkan_instance = fakeVkInstance(),
        .vulkan_physical_device = fakeVkPhysicalDevice(),
        .vulkan_device = fakeVkDevice(),
        .graphics_queue_family_index = 7,
        .graphics_queue_index = 0,
    };
}

class FakeGraphics final : public Pelican::OpenXr::IXrCompositionGraphics {
    bool has_depth = false;

  public:
    void initialize(
        const Pelican::OpenXr::XrCompositionGraphicsConfig &config) override {
        active_fake->calls.emplace_back("graphics_init");
        CHECK(config.color_swapchain == fakeSwapchain(0));
        has_depth =
            config.depth_swapchain != XR_NULL_HANDLE;
        if (has_depth) {
            CHECK(config.depth_swapchain ==
                  fakeSwapchain(1));
        }
        CHECK(config.extent == vk::Extent2D{72, 64});
        CHECK(config.color_format == test_format);
        CHECK(config.depth_format ==
              (has_depth
                   ? vk::Format::eD32Sfloat
                   : vk::Format::eUndefined));
        active_fake->graphics_release_layout_is_runtime_layout =
            config.release_layout == vk::ImageLayout::eColorAttachmentOptimal &&
            config.release_layout != vk::ImageLayout::ePresentSrcKHR;
        // Deliberately do not call enumerate_swapchain_images. No fake VkImage
        // exists anywhere in this protocol fixture.
    }

    Pelican::FrameRenderContext beginView(std::uint32_t view,
                                          Pelican::OpenXr::
                                              XrCompositionAcquiredImages images,
                                          std::uint32_t in_flight) override {
        active_fake->calls.emplace_back("begin:" + eye(view));
        CHECK(images.color == 10);
        CHECK(images.depth ==
              (has_depth
                   ? std::optional<std::uint32_t>{11}
                   : std::nullopt));
        return {
            .color_base_array_layer = view,
            .color_array_layers = 1,
            .extent = vk::Extent2D{72, 64},
            .required_layout = vk::ImageLayout::eColorAttachmentOptimal,
            .in_flight_frame_index = in_flight,
        };
    }

    Pelican::FrameRenderContext beginViewFamily(
        Pelican::OpenXr::XrCompositionAcquiredImages images,
        std::uint32_t in_flight) override {
        active_fake->calls.emplace_back("begin:family");
        CHECK(images.color == 10);
        CHECK(images.depth ==
              (has_depth
                   ? std::optional<std::uint32_t>{11}
                   : std::nullopt));
        return {
            .color_layer_attachments =
                {vk::ImageView{
                     reinterpret_cast<VkImageView>(
                         std::uintptr_t{0x701})},
                 vk::ImageView{
                     reinterpret_cast<VkImageView>(
                         std::uintptr_t{0x702})}},
            .color_array_layers = 2,
            .extent = vk::Extent2D{72, 64},
            .required_layout =
                vk::ImageLayout::eColorAttachmentOptimal,
            .in_flight_frame_index = in_flight,
        };
    }

    void submitView(std::uint32_t view, std::uint32_t) override {
        active_fake->calls.emplace_back("submit:" + eye(view));
        if (active_fake->failure == "submit_" + eye(view)) {
            throw std::runtime_error("fake GPU submit failure");
        }
    }

    void submitViewFamily(std::uint32_t) override {
        active_fake->calls.emplace_back("submit:family");
        if (active_fake->failure == "submit_family") {
            throw std::runtime_error(
                "fake GPU family submit failure");
        }
    }

    void waitForSubmission(std::uint32_t view, std::uint32_t) override {
        active_fake->calls.emplace_back("gpu_wait:" + eye(view));
    }

    void waitForViewFamilySubmission(std::uint32_t) override {
        active_fake->calls.emplace_back("gpu_wait:family");
    }
};

struct BegunFrame {
    Pelican::OpenXr::XrDisplayTiming timing;
    Pelican::OpenXr::XrLocatedViews views;
};

void makeReady(FakeRuntime &fake, Pelican::OpenXr::SessionRuntime &session) {
    fake.events.push_back(XR_SESSION_STATE_READY);
    session.pollEvents();
    REQUIRE(session.isSessionRunning());
}

BegunFrame beginFrame(FakeRuntime &fake,
                      Pelican::OpenXr::SessionRuntime &session) {
    const auto timing = session.waitFrame();
    session.beginFrame();
    auto views = timing.shouldRender()
                     ? session.locateViews(timing)
                     : Pelican::OpenXr::XrLocatedViews{};
    fake.calls.clear();
    return {timing, std::move(views)};
}

Pelican::OpenXr::XrCompositionDependencies compositionDependencies(
    Pelican::OpenXr::SessionRuntime &session) {
    return {
        .get_instance_proc_addr = &fakeGetInstanceProcAddr,
        .session_runtime = &session,
        .vulkan = nullptr,
        .renderer_color_format = test_format,
        .image_wait_timeout = 1,
        .composition_layer_depth_enabled =
            active_fake != nullptr &&
            active_fake->composition_depth_enabled,
    };
}

void submitBoth(Pelican::OpenXr::XrCompositionTarget &target) {
    target.beginLogicalFrame(2);
    (void)target.beginView(0);
    target.endView(0);
    (void)target.beginView(1);
    target.endView(1);
}

} // namespace

TEST_CASE("OpenXR composition owns one two-layer color swapchain and one projection layer",
          "[openxr][composition]") {
    FakeRuntime fake;
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
    makeReady(fake, session);
    Pelican::OpenXr::XrCompositionTarget target{
        compositionDependencies(session), std::make_unique<FakeGraphics>()};

    REQUIRE(fake.create_count == 1);
    const auto &info = fake.swapchain_create_infos[0];
    CHECK(info.arraySize == 2);
    CHECK(info.faceCount == 1);
    CHECK(info.mipCount == 1);
    CHECK(info.sampleCount == 1);
    CHECK(info.width == 72);
    CHECK(info.height == 64);
    CHECK(info.format ==
          static_cast<std::int64_t>(
              static_cast<VkFormat>(test_format)));
    CHECK(info.usageFlags ==
          XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    const auto capabilities =
        target.compositionCapabilities();
    CHECK(capabilities.array_color_swapchain);
    CHECK(capabilities.view_family_execution);
    CHECK_FALSE(capabilities.depth_submission);
    CHECK(capabilities.depth_reason ==
          "XR_KHR_composition_layer_depth_not_enabled");
    CHECK(target.supportsViewFamilyExecution());
    CHECK(fake.graphics_release_layout_is_runtime_layout);
    CHECK(fake.enumerate_images_count == 0);

    auto frame = beginFrame(fake, session);
    target.prepareFrame(frame.timing, frame.views);
    submitBoth(target);
    target.endLogicalFrame();

    CHECK(fake.calls == std::vector<std::string>{
                            "acquire:color", "wait:color",
                            "begin:L", "submit:L", "begin:R", "submit:R",
                            "gpu_wait:L", "gpu_wait:R", "release:color",
                            "end_projection"});
    CHECK(fake.saw_projection_layer);
    CHECK_FALSE(fake.saw_zero_layer);
    CHECK(target.swapchainState(0) ==
          Pelican::OpenXr::XrSwapchainState::released);
    CHECK(target.swapchainState(1) ==
          Pelican::OpenXr::XrSwapchainState::released);
    CHECK_FALSE(target.generationTeardownRequired());
}

TEST_CASE("OpenXR composition submits both array layers through one view-family command",
          "[openxr][composition][view-family]") {
    FakeRuntime fake;
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{
        sessionDependencies()};
    makeReady(fake, session);
    Pelican::OpenXr::XrCompositionTarget target{
        compositionDependencies(session),
        std::make_unique<FakeGraphics>()};

    auto frame = beginFrame(fake, session);
    target.prepareFrame(frame.timing, frame.views);
    target.beginLogicalFrame(2);
    const auto context =
        target.beginViewFamily(2);
    CHECK(context.color_array_layers == 2);
    CHECK(context.color_layer_attachments.size() == 2);
    target.endViewFamily();
    target.endLogicalFrame();

    CHECK(fake.calls ==
          std::vector<std::string>{
              "acquire:color", "wait:color",
              "begin:family", "submit:family",
              "gpu_wait:family", "release:color",
              "end_projection"});
    CHECK(fake.saw_projection_layer);
}

TEST_CASE("OpenXR composition submits a two-layer depth swapchain chain when enabled",
          "[openxr][composition][composition-depth]") {
    FakeRuntime fake{
        .composition_depth_enabled = true,
        .expected_near_z = 0.1F,
        .expected_far_z = 500.0F,
    };
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{
        sessionDependencies()};
    makeReady(fake, session);
    Pelican::OpenXr::XrCompositionTarget target{
        compositionDependencies(session),
        std::make_unique<FakeGraphics>()};

    REQUIRE(fake.create_count == 2);
    const auto &depth_info =
        fake.swapchain_create_infos[1];
    CHECK(depth_info.arraySize == 2);
    CHECK(depth_info.width == 72);
    CHECK(depth_info.height == 64);
    CHECK(depth_info.format == VK_FORMAT_D32_SFLOAT);
    CHECK(depth_info.usageFlags ==
          XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    const auto capabilities =
        target.compositionCapabilities();
    CHECK(capabilities.depth_submission);
    CHECK(capabilities.depth_format ==
          vk::Format::eD32Sfloat);
    CHECK(capabilities.depth_reason.empty());

    auto frame = beginFrame(fake, session);
    target.prepareFrame(
        frame.timing, frame.views, 0.1F, 500.0F);
    target.beginLogicalFrame(2);
    (void)target.beginViewFamily(2);
    target.endViewFamily();
    target.endLogicalFrame();

    CHECK(fake.calls ==
          std::vector<std::string>{
              "acquire:color", "wait:color",
              "acquire:depth", "wait:depth",
              "begin:family", "submit:family",
              "gpu_wait:family", "release:depth",
              "release:color", "end_projection"});
    CHECK(fake.saw_depth_chain);
}

TEST_CASE("OpenXR composition retains a submitted generation until both view fences complete",
          "[openxr][composition][submission-lifetime]") {
    FakeRuntime fake;
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
    makeReady(fake, session);
    Pelican::OpenXr::XrCompositionTarget target{
        compositionDependencies(session), std::make_unique<FakeGraphics>()};
    auto frame = beginFrame(fake, session);
    target.prepareFrame(frame.timing, frame.views);

    auto lease = std::make_shared<int>(42);
    std::weak_ptr<int> observed = lease;
    target.beginLogicalFrame(2);
    (void)target.beginView(0);
    target.endView(0, lease);
    lease.reset();
    CHECK_FALSE(observed.expired());

    (void)target.beginView(1);
    target.endView(1);
    CHECK_FALSE(observed.expired());
    target.endLogicalFrame();
    CHECK(observed.expired());
}

TEST_CASE("OpenXR swapchain wait timeout retries the same acquired image",
          "[openxr][composition][timeout]") {
    for (const bool with_depth : {false, true}) {
        DYNAMIC_SECTION("depth " << with_depth) {
            FakeRuntime fake;
            fake.composition_depth_enabled = with_depth;
            const auto timeout_kind = with_depth ? 1U : 0U;
            fake.timeout_once[timeout_kind] = true;
            FakeScope scope{fake};
            Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
            makeReady(fake, session);
            Pelican::OpenXr::XrCompositionTarget target{
                compositionDependencies(session), std::make_unique<FakeGraphics>()};
            auto frame = beginFrame(fake, session);
            target.prepareFrame(frame.timing, frame.views);
            submitBoth(target);
            target.endLogicalFrame();

            CHECK(fake.wait_counts[timeout_kind] == 2);
            CHECK(std::count(fake.calls.begin(), fake.calls.end(),
                             "acquire:" +
                                 swapchainName(timeout_kind)) == 1);
            CHECK(std::count(fake.calls.begin(), fake.calls.end(),
                             "wait:" +
                                 swapchainName(timeout_kind)) == 2);
            CHECK(fake.saw_projection_layer);
        }
    }
}

TEST_CASE("OpenXR partial failures unwind each swapchain legally and close zero-layer",
          "[openxr][composition][unwind]") {
    struct Case {
        const char *failure;
        std::vector<std::string> expected;
        Pelican::OpenXr::XrSwapchainState state;
        bool teardown;
    };
    const std::vector<Case> cases{
        {"acquire_color",
         {"acquire:color", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::idle, false},
        {"wait_color",
         {"acquire:color", "wait:color", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::acquired, true},
        {"submit_L",
         {"acquire:color", "wait:color",
          "begin:L", "submit:L", "release:color",
          "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released, true},
        {"submit_R",
         {"acquire:color", "wait:color",
          "begin:L", "submit:L", "begin:R", "submit:R",
          "gpu_wait:L", "release:color", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released, true},
        {"release_color",
         {"acquire:color", "wait:color",
          "begin:L", "submit:L", "begin:R", "submit:R",
          "gpu_wait:L", "gpu_wait:R", "release:color",
          "end_zero"},
         Pelican::OpenXr::XrSwapchainState::submitted, true},
    };

    for (const auto &test : cases) {
        DYNAMIC_SECTION(test.failure) {
            FakeRuntime fake;
            fake.failure = test.failure;
            FakeScope scope{fake};
            Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
            makeReady(fake, session);
            Pelican::OpenXr::XrCompositionTarget target{
                compositionDependencies(session), std::make_unique<FakeGraphics>()};
            auto frame = beginFrame(fake, session);
            target.prepareFrame(frame.timing, frame.views);

            CHECK_THROWS([&] {
                submitBoth(target);
                target.endLogicalFrame();
            }());
            CHECK(fake.calls == test.expected);
            CHECK(target.swapchainState(0) == test.state);
            CHECK(target.swapchainState(1) == test.state);
            CHECK(target.generationTeardownRequired() == test.teardown);
            CHECK(fake.saw_zero_layer);
            CHECK_FALSE(fake.saw_projection_layer);
        }
    }
}

TEST_CASE("OpenXR depth failures preserve legal color and depth release order",
          "[openxr][composition][composition-depth][unwind]") {
    struct Case {
        const char *failure;
        std::vector<std::string> expected;
        Pelican::OpenXr::XrSwapchainState color;
        Pelican::OpenXr::XrSwapchainState depth;
        bool teardown;
    };
    const std::vector<Case> cases{
        {"acquire_depth",
         {"acquire:color", "wait:color",
          "acquire:depth", "release:color", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
         Pelican::OpenXr::XrSwapchainState::idle, false},
        {"wait_depth",
         {"acquire:color", "wait:color",
          "acquire:depth", "wait:depth",
          "release:color", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
         Pelican::OpenXr::XrSwapchainState::acquired, true},
        {"release_depth",
         {"acquire:color", "wait:color",
          "acquire:depth", "wait:depth",
          "begin:L", "submit:L", "begin:R", "submit:R",
          "gpu_wait:L", "gpu_wait:R",
          "release:depth", "release:color", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
         Pelican::OpenXr::XrSwapchainState::submitted, true},
    };

    for (const auto &test : cases) {
        DYNAMIC_SECTION(test.failure) {
            FakeRuntime fake{
                .failure = test.failure,
                .composition_depth_enabled = true,
            };
            FakeScope scope{fake};
            Pelican::OpenXr::SessionRuntime session{
                sessionDependencies()};
            makeReady(fake, session);
            Pelican::OpenXr::XrCompositionTarget target{
                compositionDependencies(session),
                std::make_unique<FakeGraphics>()};
            auto frame = beginFrame(fake, session);
            target.prepareFrame(frame.timing, frame.views);

            CHECK_THROWS([&] {
                submitBoth(target);
                target.endLogicalFrame();
            }());
            CHECK(fake.calls == test.expected);
            CHECK(target.swapchainState(0) ==
                  test.color);
            CHECK(target.depthSwapchainState() ==
                  test.depth);
            CHECK(target.generationTeardownRequired() ==
                  test.teardown);
            CHECK(fake.saw_zero_layer);
            CHECK_FALSE(fake.saw_projection_layer);
        }
    }
}

TEST_CASE("OpenXR session loss routes to generation teardown without another frame call",
          "[openxr][composition][loss]") {
    struct LossCase {
        const char *failure;
        Pelican::OpenXr::XrTerminalPath terminal_path;
    };
    for (const auto &test : {
             LossCase{"loss_wait_color",
                      Pelican::OpenXr::XrTerminalPath::loss_pending},
             LossCase{"loss_pending_wait_color",
                      Pelican::OpenXr::XrTerminalPath::loss_pending},
             LossCase{"instance_loss_wait_color",
                      Pelican::OpenXr::XrTerminalPath::instance_loss_pending},
         }) {
        DYNAMIC_SECTION(test.failure) {
            FakeRuntime fake;
            fake.failure = test.failure;
            FakeScope scope{fake};
            Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
            makeReady(fake, session);
            Pelican::OpenXr::XrCompositionTarget target{
                compositionDependencies(session), std::make_unique<FakeGraphics>()};
            auto frame = beginFrame(fake, session);
            target.prepareFrame(frame.timing, frame.views);

            CHECK_THROWS(target.beginLogicalFrame(2));
            CHECK(fake.calls == std::vector<std::string>{
                                    "acquire:color",
                                    "wait:color"});
            CHECK(session.terminalPath() == test.terminal_path);
            CHECK(target.generationTeardownRequired());
            CHECK_FALSE(fake.saw_zero_layer);
        }
    }
}

TEST_CASE("OpenXR shouldRender false closes without swapchain work",
          "[openxr][composition][zero-layer]") {
    FakeRuntime fake;
    fake.should_render = false;
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
    makeReady(fake, session);
    Pelican::OpenXr::XrCompositionTarget target{
        compositionDependencies(session), std::make_unique<FakeGraphics>()};
    auto frame = beginFrame(fake, session);

    target.endFrameWithoutLayers(frame.timing);
    CHECK(fake.calls == std::vector<std::string>{"end_zero"});
    CHECK(fake.saw_zero_layer);
}

TEST_CASE("OpenXR discarded renderable timing still closes with zero layers",
          "[openxr][composition][zero-layer][discard]") {
    FakeRuntime fake;
    fake.should_render = true;
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
    makeReady(fake, session);
    Pelican::OpenXr::XrCompositionTarget target{
        compositionDependencies(session), std::make_unique<FakeGraphics>()};
    auto frame = beginFrame(fake, session);

    target.endFrameWithoutLayers(frame.timing);
    CHECK(fake.calls == std::vector<std::string>{"end_zero"});
    CHECK(fake.saw_zero_layer);
}

TEST_CASE("OpenXR color swapchain creation failure is fatal",
          "[openxr][composition][creation][color]") {
    FakeRuntime fake;
    fake.failure = "create_color";
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{sessionDependencies()};

    CHECK_THROWS_WITH(
        Pelican::OpenXr::XrCompositionTarget(
            compositionDependencies(session), std::make_unique<FakeGraphics>()),
        Catch::Matchers::ContainsSubstring("xrCreateSwapchain"));
    CHECK(fake.calls ==
          std::vector<std::string>{"create:color"});
    CHECK(fake.enumerate_images_count == 0);
}

TEST_CASE("OpenXR depth swapchain creation failure falls back to color-only",
          "[openxr][composition][creation][composition-depth]") {
    FakeRuntime fake{
        .failure = "create_depth",
        .composition_depth_enabled = true,
    };
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{
        sessionDependencies()};
    {
        Pelican::OpenXr::XrCompositionTarget target{
            compositionDependencies(session),
            std::make_unique<FakeGraphics>()};
        const auto capabilities =
            target.compositionCapabilities();
        CHECK_FALSE(capabilities.depth_submission);
        CHECK(capabilities.depth_reason.find(
                  "depth_swapchain_creation_failed") !=
              std::string::npos);
    }
    CHECK(fake.calls ==
          std::vector<std::string>{
              "create:color", "create:depth",
              "graphics_init", "destroy:color"});
    CHECK(fake.enumerate_images_count == 0);
}
