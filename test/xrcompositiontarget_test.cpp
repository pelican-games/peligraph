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
    bool saw_zero_layer = false;
    bool graphics_release_layout_is_runtime_layout = false;
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
XrSwapchain fakeSwapchain(std::uint32_t view) {
    return reinterpret_cast<XrSwapchain>(std::uintptr_t{0x401 + view});
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

std::uint32_t viewFor(XrSwapchain swapchain) {
    if (swapchain == fakeSwapchain(0)) return 0;
    if (swapchain == fakeSwapchain(1)) return 1;
    FAIL("unknown fake swapchain");
    return 0;
}

std::string eye(std::uint32_t view) { return view == 0 ? "L" : "R"; }

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
        CHECK(layer.views[view].subImage.swapchain == fakeSwapchain(view));
        CHECK(layer.views[view].subImage.imageArrayIndex == 0);
        CHECK(layer.views[view].subImage.imageRect.offset.x == 0);
        CHECK(layer.views[view].subImage.imageRect.offset.y == 0);
        CHECK(layer.views[view].subImage.imageRect.extent.width ==
              static_cast<std::int32_t>(64 + view * 8));
        CHECK(layer.views[view].subImage.imageRect.extent.height == 64);
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
    *count = 2;
    if (capacity == 0) return XR_SUCCESS;
    REQUIRE(capacity >= 2);
    formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
    formats[1] = static_cast<std::int64_t>(static_cast<VkFormat>(test_format));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeCreateSwapchain(XrSession,
                                        const XrSwapchainCreateInfo *info,
                                        XrSwapchain *swapchain) {
    const auto view = active_fake->create_count++;
    active_fake->calls.emplace_back("create:" + eye(view));
    REQUIRE(view < 2);
    active_fake->swapchain_create_infos[view] = *info;
    if (active_fake->failure == "create_" + eye(view)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *swapchain = fakeSwapchain(view);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeDestroySwapchain(XrSwapchain swapchain) {
    active_fake->calls.emplace_back("destroy:" + eye(viewFor(swapchain)));
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
    const auto view = viewFor(swapchain);
    active_fake->calls.emplace_back("acquire:" + eye(view));
    if (active_fake->failure == "acquire_" + eye(view)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *image_index = 10 + view;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeWaitSwapchainImage(XrSwapchain swapchain,
                                           const XrSwapchainImageWaitInfo *) {
    const auto view = viewFor(swapchain);
    active_fake->calls.emplace_back("wait:" + eye(view));
    if (active_fake->timeout_once[view] && active_fake->wait_counts[view]++ == 0) {
        return XR_TIMEOUT_EXPIRED;
    }
    if (active_fake->failure == "wait_" + eye(view)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (active_fake->failure == "loss_wait_" + eye(view)) {
        return XR_ERROR_SESSION_LOST;
    }
    if (active_fake->failure == "loss_pending_wait_" + eye(view)) {
        return XR_SESSION_LOSS_PENDING;
    }
    if (active_fake->failure == "instance_loss_wait_" + eye(view)) {
        return XR_ERROR_INSTANCE_LOST;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fakeReleaseSwapchainImage(
    XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *) {
    const auto view = viewFor(swapchain);
    active_fake->calls.emplace_back("release:" + eye(view));
    if (active_fake->failure == "release_" + eye(view)) {
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
  public:
    void initialize(
        const Pelican::OpenXr::XrCompositionGraphicsConfig &config) override {
        active_fake->calls.emplace_back("graphics_init");
        CHECK(config.swapchains[0] == fakeSwapchain(0));
        CHECK(config.swapchains[1] == fakeSwapchain(1));
        CHECK(config.color_format == test_format);
        active_fake->graphics_release_layout_is_runtime_layout =
            config.release_layout == vk::ImageLayout::eColorAttachmentOptimal &&
            config.release_layout != vk::ImageLayout::ePresentSrcKHR;
        // Deliberately do not call enumerate_swapchain_images. No fake VkImage
        // exists anywhere in this protocol fixture.
    }

    Pelican::FrameRenderContext beginView(std::uint32_t view,
                                          std::uint32_t image_index,
                                          std::uint32_t in_flight) override {
        active_fake->calls.emplace_back("begin:" + eye(view));
        CHECK(image_index == 10 + view);
        return {
            .extent = vk::Extent2D{64 + view * 8, 64},
            .required_layout = vk::ImageLayout::eColorAttachmentOptimal,
            .in_flight_frame_index = in_flight,
        };
    }

    void submitView(std::uint32_t view, std::uint32_t) override {
        active_fake->calls.emplace_back("submit:" + eye(view));
        if (active_fake->failure == "submit_" + eye(view)) {
            throw std::runtime_error("fake GPU submit failure");
        }
    }

    void waitForSubmission(std::uint32_t view, std::uint32_t) override {
        active_fake->calls.emplace_back("gpu_wait:" + eye(view));
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

TEST_CASE("OpenXR composition owns two arraySize-one swapchains and one projection layer",
          "[openxr][composition]") {
    FakeRuntime fake;
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
    makeReady(fake, session);
    Pelican::OpenXr::XrCompositionTarget target{
        compositionDependencies(session), std::make_unique<FakeGraphics>()};

    REQUIRE(fake.create_count == 2);
    for (std::uint32_t view = 0; view < 2; ++view) {
        const auto &info = fake.swapchain_create_infos[view];
        CHECK(info.arraySize == 1);
        CHECK(info.faceCount == 1);
        CHECK(info.mipCount == 1);
        CHECK(info.sampleCount == 1);
        CHECK(info.width == 64 + view * 8);
        CHECK(info.height == 64);
        CHECK(info.format ==
              static_cast<std::int64_t>(static_cast<VkFormat>(test_format)));
        CHECK(info.usageFlags == XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    }
    CHECK(fake.graphics_release_layout_is_runtime_layout);
    CHECK(fake.enumerate_images_count == 0);

    auto frame = beginFrame(fake, session);
    target.prepareFrame(frame.timing, frame.views);
    submitBoth(target);
    target.endLogicalFrame();

    CHECK(fake.calls == std::vector<std::string>{
                            "acquire:L", "wait:L", "acquire:R", "wait:R",
                            "begin:L", "submit:L", "begin:R", "submit:R",
                            "gpu_wait:L", "gpu_wait:R", "release:L", "release:R",
                            "end_projection"});
    CHECK(fake.saw_projection_layer);
    CHECK_FALSE(fake.saw_zero_layer);
    CHECK(target.swapchainState(0) ==
          Pelican::OpenXr::XrSwapchainState::released);
    CHECK(target.swapchainState(1) ==
          Pelican::OpenXr::XrSwapchainState::released);
    CHECK_FALSE(target.generationTeardownRequired());
}

TEST_CASE("OpenXR swapchain wait timeout retries the same acquired image",
          "[openxr][composition][timeout]") {
    for (const auto timeout_view : {0U, 1U}) {
        DYNAMIC_SECTION("timeout view " << timeout_view) {
            FakeRuntime fake;
            fake.timeout_once[timeout_view] = true;
            FakeScope scope{fake};
            Pelican::OpenXr::SessionRuntime session{sessionDependencies()};
            makeReady(fake, session);
            Pelican::OpenXr::XrCompositionTarget target{
                compositionDependencies(session), std::make_unique<FakeGraphics>()};
            auto frame = beginFrame(fake, session);
            target.prepareFrame(frame.timing, frame.views);
            submitBoth(target);
            target.endLogicalFrame();

            CHECK(fake.wait_counts[timeout_view] == 2);
            CHECK(std::count(fake.calls.begin(), fake.calls.end(),
                             "acquire:" + eye(timeout_view)) == 1);
            CHECK(std::count(fake.calls.begin(), fake.calls.end(),
                             "wait:" + eye(timeout_view)) == 2);
            CHECK(fake.saw_projection_layer);
        }
    }
}

TEST_CASE("OpenXR partial failures unwind each swapchain legally and close zero-layer",
          "[openxr][composition][unwind]") {
    struct Case {
        const char *failure;
        std::vector<std::string> expected;
        Pelican::OpenXr::XrSwapchainState left;
        Pelican::OpenXr::XrSwapchainState right;
        bool teardown;
    };
    const std::vector<Case> cases{
        {"acquire_L", {"acquire:L", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::idle,
         Pelican::OpenXr::XrSwapchainState::idle, false},
        {"acquire_R", {"acquire:L", "wait:L", "acquire:R", "release:L", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
         Pelican::OpenXr::XrSwapchainState::idle, false},
        {"wait_L", {"acquire:L", "wait:L", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::acquired,
         Pelican::OpenXr::XrSwapchainState::idle, true},
        {"wait_R", {"acquire:L", "wait:L", "acquire:R", "wait:R", "release:L", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
         Pelican::OpenXr::XrSwapchainState::acquired, true},
        {"submit_L", {"acquire:L", "wait:L", "acquire:R", "wait:R", "begin:L", "submit:L",
                      "release:L", "release:R", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
         Pelican::OpenXr::XrSwapchainState::released, true},
        {"submit_R", {"acquire:L", "wait:L", "acquire:R", "wait:R", "begin:L", "submit:L",
                      "begin:R", "submit:R", "gpu_wait:L", "release:L", "release:R", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
         Pelican::OpenXr::XrSwapchainState::released, true},
        {"release_L", {"acquire:L", "wait:L", "acquire:R", "wait:R", "begin:L", "submit:L",
                       "begin:R", "submit:R", "gpu_wait:L", "gpu_wait:R", "release:L",
                       "release:R", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::submitted,
         Pelican::OpenXr::XrSwapchainState::released, true},
        {"release_R", {"acquire:L", "wait:L", "acquire:R", "wait:R", "begin:L", "submit:L",
                       "begin:R", "submit:R", "gpu_wait:L", "gpu_wait:R", "release:L",
                       "release:R", "end_zero"},
         Pelican::OpenXr::XrSwapchainState::released,
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
            CHECK(target.swapchainState(0) == test.left);
            CHECK(target.swapchainState(1) == test.right);
            CHECK(target.generationTeardownRequired() == test.teardown);
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
             LossCase{"loss_wait_R",
                      Pelican::OpenXr::XrTerminalPath::loss_pending},
             LossCase{"loss_pending_wait_R",
                      Pelican::OpenXr::XrTerminalPath::loss_pending},
             LossCase{"instance_loss_wait_R",
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
                                    "acquire:L", "wait:L", "acquire:R",
                                    "wait:R", "release:L"});
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

TEST_CASE("OpenXR second swapchain creation failure destroys the first",
          "[openxr][composition][creation]") {
    FakeRuntime fake;
    fake.failure = "create_R";
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime session{sessionDependencies()};

    CHECK_THROWS_WITH(
        Pelican::OpenXr::XrCompositionTarget(
            compositionDependencies(session), std::make_unique<FakeGraphics>()),
        Catch::Matchers::ContainsSubstring("xrCreateSwapchain"));
    CHECK(fake.calls ==
          std::vector<std::string>{"create:L", "create:R", "destroy:L"});
    CHECK(fake.enumerate_images_count == 0);
}
