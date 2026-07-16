#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/openxr/openxrsession.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct FakeEvent {
    XrStructureType type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
};

struct FakeRuntime {
    std::string failure;
    std::vector<std::string> calls;
    std::deque<FakeEvent> events;
    bool should_render = true;
    bool last_end_was_zero_layer = false;
    uint32_t update_count = 0;
};

thread_local FakeRuntime *active_fake = nullptr;

struct FakeScope {
    explicit FakeScope(FakeRuntime &fake) { active_fake = &fake; }
    ~FakeScope() { active_fake = nullptr; }
};

XrInstance fakeInstance() { return reinterpret_cast<XrInstance>(std::uintptr_t{0x101}); }
XrSession fakeSession() { return reinterpret_cast<XrSession>(std::uintptr_t{0x301}); }
XrSpace fakeSpace() { return reinterpret_cast<XrSpace>(std::uintptr_t{0x401}); }
VkInstance fakeVkInstance() { return reinterpret_cast<VkInstance>(std::uintptr_t{0x501}); }
VkPhysicalDevice fakeVkPhysicalDevice() {
    return reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x502});
}
VkDevice fakeVkDevice() { return reinterpret_cast<VkDevice>(std::uintptr_t{0x503}); }

XrResult XRAPI_CALL fakeCreateSession(XrInstance instance, const XrSessionCreateInfo *info,
                                      XrSession *session) {
    active_fake->calls.emplace_back("create_session");
    CHECK(instance == fakeInstance());
    CHECK(info->systemId == 17);
    const auto *binding = static_cast<const XrGraphicsBindingVulkan2KHR *>(info->next);
    REQUIRE(binding != nullptr);
    CHECK(binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
    // These are opaque protocol tokens.  The fixture never submits them (or a
    // fake VkImage) to Vulkan.
    CHECK(binding->instance == fakeVkInstance());
    CHECK(binding->physicalDevice == fakeVkPhysicalDevice());
    CHECK(binding->device == fakeVkDevice());
    CHECK(binding->queueFamilyIndex == 7);
    CHECK(binding->queueIndex == 0);
    if (active_fake->failure == "create_session") return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    *session = fakeSession();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeDestroySession(XrSession session) {
    active_fake->calls.emplace_back("destroy_session");
    CHECK(session == fakeSession());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeCreateReferenceSpace(XrSession session,
                                             const XrReferenceSpaceCreateInfo *info,
                                             XrSpace *space) {
    active_fake->calls.emplace_back("create_reference_space");
    CHECK(session == fakeSession());
    CHECK(info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL);
    CHECK(info->poseInReferenceSpace.orientation.w == 1.0F);
    if (active_fake->failure == "create_reference_space") return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    *space = fakeSpace();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeDestroySpace(XrSpace space) {
    active_fake->calls.emplace_back("destroy_space");
    CHECK(space == fakeSpace());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakePollEvent(XrInstance instance, XrEventDataBuffer *event) {
    active_fake->calls.emplace_back("poll_event");
    CHECK(instance == fakeInstance());
    if (active_fake->failure == "poll_event") return XR_ERROR_RUNTIME_FAILURE;
    if (active_fake->events.empty()) return XR_EVENT_UNAVAILABLE;

    const auto next = active_fake->events.front();
    active_fake->events.pop_front();
    if (next.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
        XrEventDataInstanceLossPending loss{XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING};
        loss.lossTime = 9001;
        *reinterpret_cast<XrEventDataInstanceLossPending *>(event) = loss;
    } else {
        XrEventDataSessionStateChanged changed{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
        changed.session = fakeSession();
        changed.state = next.state;
        changed.time = 8001;
        *reinterpret_cast<XrEventDataSessionStateChanged *>(event) = changed;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeBeginSession(XrSession session, const XrSessionBeginInfo *info) {
    active_fake->calls.emplace_back("begin_session");
    CHECK(session == fakeSession());
    CHECK(info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
    return active_fake->failure == "begin_session" ? XR_ERROR_SESSION_NOT_READY : XR_SUCCESS;
}

XrResult XRAPI_CALL fakeEndSession(XrSession session) {
    active_fake->calls.emplace_back("end_session");
    CHECK(session == fakeSession());
    return active_fake->failure == "end_session" ? XR_ERROR_SESSION_NOT_STOPPING : XR_SUCCESS;
}

XrResult XRAPI_CALL fakeWaitFrame(XrSession session, const XrFrameWaitInfo *,
                                  XrFrameState *state) {
    active_fake->calls.emplace_back("wait_frame");
    CHECK(session == fakeSession());
    if (active_fake->failure == "wait_frame") return XR_ERROR_RUNTIME_FAILURE;
    state->predictedDisplayTime = 1'234'567;
    state->predictedDisplayPeriod = 11'111'111;
    state->shouldRender = active_fake->should_render ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeBeginFrame(XrSession session, const XrFrameBeginInfo *) {
    active_fake->calls.emplace_back("begin_frame");
    CHECK(session == fakeSession());
    return active_fake->failure == "begin_frame" ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
}

XrResult XRAPI_CALL fakeEndFrame(XrSession session, const XrFrameEndInfo *info) {
    active_fake->calls.emplace_back("end_frame");
    CHECK(session == fakeSession());
    CHECK(info->displayTime == 1'234'567);
    CHECK(info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    active_fake->last_end_was_zero_layer = info->layerCount == 0 && info->layers == nullptr;
    if (active_fake->failure == "end_frame") return XR_ERROR_RUNTIME_FAILURE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL fakeLocateViews(XrSession session, const XrViewLocateInfo *info,
                                    XrViewState *state, uint32_t capacity, uint32_t *count,
                                    XrView *views) {
    active_fake->calls.emplace_back("locate_views");
    CHECK(session == fakeSession());
    CHECK(info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
    CHECK(info->displayTime == 1'234'567);
    CHECK(info->space == fakeSpace());
    REQUIRE(capacity >= 2);
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    views[0].pose.position.x = -0.032F;
    views[1].pose.position.x = 0.032F;
    return XR_SUCCESS;
}

PFN_xrVoidFunction fakeFunction(const std::string &name) {
    if (name == "xrCreateSession") return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateSession);
    if (name == "xrDestroySession") return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroySession);
    if (name == "xrPollEvent") return reinterpret_cast<PFN_xrVoidFunction>(&fakePollEvent);
    if (name == "xrBeginSession") return reinterpret_cast<PFN_xrVoidFunction>(&fakeBeginSession);
    if (name == "xrEndSession") return reinterpret_cast<PFN_xrVoidFunction>(&fakeEndSession);
    if (name == "xrWaitFrame") return reinterpret_cast<PFN_xrVoidFunction>(&fakeWaitFrame);
    if (name == "xrBeginFrame") return reinterpret_cast<PFN_xrVoidFunction>(&fakeBeginFrame);
    if (name == "xrEndFrame") return reinterpret_cast<PFN_xrVoidFunction>(&fakeEndFrame);
    if (name == "xrLocateViews") return reinterpret_cast<PFN_xrVoidFunction>(&fakeLocateViews);
    if (name == "xrCreateReferenceSpace")
        return reinterpret_cast<PFN_xrVoidFunction>(&fakeCreateReferenceSpace);
    if (name == "xrDestroySpace") return reinterpret_cast<PFN_xrVoidFunction>(&fakeDestroySpace);
    return nullptr;
}

XrResult XRAPI_CALL fakeGetInstanceProcAddr(XrInstance instance, const char *name,
                                            PFN_xrVoidFunction *function) {
    CHECK(instance == fakeInstance());
    active_fake->calls.emplace_back(std::string{"resolve:"} + name);
    if (active_fake->failure == std::string{"resolve:"} + name) {
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    *function = fakeFunction(name);
    return *function == nullptr ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_SUCCESS;
}

Pelican::OpenXr::XrSessionDependencies fakeDependencies() {
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

void pushState(FakeRuntime &fake, XrSessionState state) {
    fake.events.push_back({XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, state});
}

void makeReady(FakeRuntime &fake, Pelican::OpenXr::SessionRuntime &runtime) {
    pushState(fake, XR_SESSION_STATE_READY);
    runtime.pollEvents();
    REQUIRE(runtime.isSessionRunning());
    fake.calls.clear();
}

} // namespace

TEST_CASE("OpenXR session dispatch, lifecycle, and explicit state gates have a stable trace",
          "[openxr][session]") {
    FakeRuntime fake;
    FakeScope scope{fake};
    {
        Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
        const std::vector<std::string> expected_setup{
            "resolve:xrCreateSession",       "resolve:xrDestroySession",
            "resolve:xrPollEvent",           "resolve:xrBeginSession",
            "resolve:xrEndSession",          "resolve:xrWaitFrame",
            "resolve:xrBeginFrame",          "resolve:xrEndFrame",
            "resolve:xrLocateViews",         "resolve:xrCreateReferenceSpace",
            "resolve:xrDestroySpace",        "create_session",
            "create_reference_space",
        };
        CHECK(fake.calls == expected_setup);

        // FOCUSED is not a numeric running gate: without a successful READY /
        // xrBeginSession transition the session remains stopped.
        fake.calls.clear();
        pushState(fake, XR_SESSION_STATE_FOCUSED);
        runtime.pollEvents();
        CHECK_FALSE(runtime.isSessionRunning());
        CHECK_FALSE(runtime.isInputEligible());
        CHECK(fake.calls == std::vector<std::string>{"poll_event", "poll_event"});

        fake.calls.clear();
        pushState(fake, XR_SESSION_STATE_READY);
        runtime.pollEvents();
        CHECK(runtime.isSessionRunning());
        CHECK_FALSE(runtime.isInputEligible());
        CHECK(fake.calls ==
              std::vector<std::string>{"poll_event", "begin_session", "poll_event"});

        for (const auto state : {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE,
                                 XR_SESSION_STATE_FOCUSED}) {
            fake.calls.clear();
            pushState(fake, state);
            runtime.pollEvents();
            CHECK(runtime.isSessionRunning());
            CHECK(runtime.isInputEligible() == (state == XR_SESSION_STATE_FOCUSED));
            CHECK(fake.calls == std::vector<std::string>{"poll_event", "poll_event"});
        }

        fake.calls.clear();
        pushState(fake, XR_SESSION_STATE_STOPPING);
        runtime.pollEvents();
        CHECK_FALSE(runtime.isSessionRunning());
        CHECK(fake.calls ==
              std::vector<std::string>{"poll_event", "end_session", "poll_event"});

        fake.calls.clear();
        pushState(fake, XR_SESSION_STATE_IDLE);
        runtime.pollEvents();
        CHECK_FALSE(runtime.isSessionRunning());
        CHECK_FALSE(runtime.isInputEligible());
        CHECK(fake.calls == std::vector<std::string>{"poll_event", "poll_event"});

        fake.calls.clear();
        pushState(fake, XR_SESSION_STATE_LOSS_PENDING);
        runtime.pollEvents();
        CHECK(runtime.terminalPath() == Pelican::OpenXr::XrTerminalPath::loss_pending);
        CHECK(fake.calls == std::vector<std::string>{"poll_event"});
    }
    CHECK(fake.calls[fake.calls.size() - 2] == "destroy_space");
    CHECK(fake.calls.back() == "destroy_session");
}

TEST_CASE("OpenXR terminal events retain distinct paths", "[openxr][session]") {
    using Pelican::OpenXr::XrTerminalPath;

    SECTION("EXITING") {
        FakeRuntime fake;
        FakeScope scope{fake};
        Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
        fake.calls.clear();
        pushState(fake, XR_SESSION_STATE_EXITING);
        runtime.pollEvents();
        CHECK(runtime.terminalPath() == XrTerminalPath::exiting);
        CHECK(fake.calls == std::vector<std::string>{"poll_event"});
    }

    SECTION("instance loss pending") {
        FakeRuntime fake;
        FakeScope scope{fake};
        Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
        fake.calls.clear();
        fake.events.push_back({XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING,
                               XR_SESSION_STATE_UNKNOWN});
        runtime.pollEvents();
        CHECK(runtime.terminalPath() == XrTerminalPath::instance_loss_pending);
        CHECK(fake.calls == std::vector<std::string>{"poll_event"});
    }
}

TEST_CASE("OpenXR frame timing is immutable simulation-external data and every frame ends zero-layer",
          "[openxr][session][timing]") {
    STATIC_REQUIRE_FALSE((std::is_assignable_v<Pelican::OpenXr::XrDisplayTiming &,
                                                Pelican::OpenXr::XrDisplayTiming>));
    FakeRuntime fake;
    FakeScope scope{fake};
    Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
    makeReady(fake, runtime);

    Pelican::EngineTime engine_time;
    engine_time.setup(Pelican::EngineTime::Mode::fixed_step, 1.0 / 60.0);
    const auto revision_before = engine_time.timeSetRevision();

    SECTION("shouldRender false still updates and closes the frame") {
        fake.should_render = false;
        const auto frame = Pelican::OpenXr::runSessionFrame(runtime, engine_time, [&] {
            ++fake.update_count;
        });
        CHECK_FALSE(frame.display_timing.shouldRender());
        CHECK(frame.display_timing.predictedDisplayTime() == 1'234'567);
        CHECK(frame.located_views.views.empty());
        CHECK(fake.update_count == 1);
        CHECK(engine_time.frameIndex() == 1);
        CHECK(engine_time.timeSetRevision() == revision_before);
        CHECK(engine_time.now() != static_cast<double>(frame.display_timing.predictedDisplayTime()));
        CHECK(fake.calls ==
              std::vector<std::string>{"wait_frame", "begin_frame", "end_frame"});
        CHECK(fake.last_end_was_zero_layer);
    }

    SECTION("shouldRender true locates and retains views without drawing") {
        const auto frame = Pelican::OpenXr::runSessionFrame(runtime, engine_time, [&] {
            ++fake.update_count;
        });
        CHECK(frame.display_timing.shouldRender());
        CHECK(frame.display_timing.predictedDisplayPeriod() == 11'111'111);
        REQUIRE(frame.located_views.views.size() == 2);
        CHECK(frame.located_views.views[0].pose.position.x == -0.032F);
        CHECK(frame.located_views.views[1].pose.position.x == 0.032F);
        CHECK(fake.update_count == 1);
        CHECK(engine_time.timeSetRevision() == revision_before);
        CHECK(fake.calls == std::vector<std::string>{"wait_frame", "begin_frame",
                                                     "locate_views", "end_frame"});
        CHECK(fake.last_end_was_zero_layer);
    }
}

TEST_CASE("OpenXR wait, begin, and end frame failures preserve their call boundary",
          "[openxr][session][error]") {
    for (const auto *failure : {"wait_frame", "begin_frame", "end_frame"}) {
        DYNAMIC_SECTION(failure) {
            FakeRuntime fake;
            FakeScope scope{fake};
            Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
            makeReady(fake, runtime);
            fake.failure = failure;

            Pelican::EngineTime engine_time;
            engine_time.setup(Pelican::EngineTime::Mode::fixed_step, 1.0 / 60.0);
            CHECK_THROWS_WITH(
                Pelican::OpenXr::runSessionFrame(runtime, engine_time,
                                                 [&] { ++fake.update_count; }),
                Catch::Matchers::ContainsSubstring(failure == std::string{"wait_frame"}
                                                       ? "xrWaitFrame"
                                                       : failure == std::string{"begin_frame"}
                                                             ? "xrBeginFrame"
                                                             : "xrEndFrame"));

            if (failure == std::string{"wait_frame"}) {
                CHECK(fake.calls == std::vector<std::string>{"wait_frame"});
                CHECK(fake.update_count == 0);
            } else if (failure == std::string{"begin_frame"}) {
                CHECK(fake.calls ==
                      std::vector<std::string>{"wait_frame", "begin_frame"});
                CHECK(fake.update_count == 1);
            } else {
                CHECK(fake.calls ==
                      std::vector<std::string>{"wait_frame", "begin_frame", "locate_views",
                                               "end_frame"});
                CHECK(fake.update_count == 1);
            }
            CHECK(engine_time.timeSetRevision() == 0);
        }
    }
}

TEST_CASE("OpenXR event and begin/end session failures are fail-fast with explicit running state",
          "[openxr][session][error]") {
    SECTION("poll") {
        FakeRuntime fake;
        FakeScope scope{fake};
        Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
        fake.calls.clear();
        fake.failure = "poll_event";
        CHECK_THROWS_WITH(runtime.pollEvents(),
                          Catch::Matchers::ContainsSubstring("xrPollEvent"));
        CHECK(fake.calls == std::vector<std::string>{"poll_event"});
    }

    SECTION("begin session") {
        FakeRuntime fake;
        FakeScope scope{fake};
        Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
        fake.calls.clear();
        fake.failure = "begin_session";
        pushState(fake, XR_SESSION_STATE_READY);
        CHECK_THROWS_WITH(runtime.pollEvents(),
                          Catch::Matchers::ContainsSubstring("xrBeginSession"));
        CHECK_FALSE(runtime.isSessionRunning());
        CHECK(fake.calls == std::vector<std::string>{"poll_event", "begin_session"});
    }

    SECTION("end session") {
        FakeRuntime fake;
        FakeScope scope{fake};
        Pelican::OpenXr::SessionRuntime runtime{fakeDependencies()};
        makeReady(fake, runtime);
        fake.failure = "end_session";
        pushState(fake, XR_SESSION_STATE_STOPPING);
        CHECK_THROWS_WITH(runtime.pollEvents(),
                          Catch::Matchers::ContainsSubstring("xrEndSession"));
        CHECK_FALSE(runtime.isSessionRunning());
        CHECK(fake.calls == std::vector<std::string>{"poll_event", "end_session"});
    }
}

TEST_CASE("OpenXR session dispatch resolution and creation failures do not cross into Vulkan",
          "[openxr][session][error]") {
    SECTION("resolution") {
        FakeRuntime fake{.failure = "resolve:xrWaitFrame"};
        FakeScope scope{fake};
        CHECK_THROWS_WITH(Pelican::OpenXr::SessionRuntime{fakeDependencies()},
                          Catch::Matchers::ContainsSubstring("xrWaitFrame"));
        CHECK(std::find(fake.calls.begin(), fake.calls.end(), "create_session") ==
              fake.calls.end());
    }

    SECTION("session creation") {
        FakeRuntime fake{.failure = "create_session"};
        FakeScope scope{fake};
        CHECK_THROWS_WITH(Pelican::OpenXr::SessionRuntime{fakeDependencies()},
                          Catch::Matchers::ContainsSubstring("xrCreateSession"));
        CHECK(fake.calls.back() == "create_session");
    }

    SECTION("reference space creation unwinds the session") {
        FakeRuntime fake{.failure = "create_reference_space"};
        FakeScope scope{fake};
        CHECK_THROWS_WITH(Pelican::OpenXr::SessionRuntime{fakeDependencies()},
                          Catch::Matchers::ContainsSubstring("xrCreateReferenceSpace"));
        CHECK(fake.calls[fake.calls.size() - 2] == "create_reference_space");
        CHECK(fake.calls.back() == "destroy_session");
    }
}
