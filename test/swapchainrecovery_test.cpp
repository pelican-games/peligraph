#include "../src/core/vkcore/swapchainrecovery.hpp"
#include "../src/core/vkcore/windowsurface.hpp"

#include <catch2/catch_test_macros.hpp>
#include <array>

using namespace Pelican;

namespace {

SwapchainRecoveryKey key(
    std::uint64_t revision,
    vk::Extent2D extent = {1280, 720}) {
    return SwapchainRecoveryKey{
        .framebuffer_revision = revision,
        .framebuffer_extent = extent,
        .surface_support_fingerprint = 11,
        .output_facts_fingerprint = 22,
        .present_configuration_fingerprint = 33,
    };
}

} // namespace

TEST_CASE(
    "WP216 classifies routine WSI results without exception control flow",
    "[wp216][wsi][state]") {
    CHECK(classifyWsiResult(vk::Result::eSuccess) ==
          WsiResultClass::ready);
    CHECK(classifyWsiResult(
              vk::Result::eSuboptimalKHR) ==
          WsiResultClass::refresh_advisory);
    CHECK(classifyWsiResult(vk::Result::eTimeout) ==
          WsiResultClass::not_ready);
    CHECK(classifyWsiResult(
              vk::Result::eErrorOutOfDateKHR) ==
          WsiResultClass::swapchain_unavailable);
    CHECK(classifyWsiResult(
              vk::Result::eErrorSurfaceLostKHR) ==
          WsiResultClass::surface_unavailable);
    CHECK(classifyWsiResult(
              vk::Result::eErrorDeviceLost) ==
          WsiResultClass::device_lost);
    CHECK(classifyWsiResult(
              vk::Result::eErrorOutOfDeviceMemory) ==
          WsiResultClass::retryable_failure);
    CHECK(classifyWsiResult(
              vk::Result::eErrorInitializationFailed) ==
          WsiResultClass::fatal);
}

TEST_CASE(
    "WP216 base Vulkan retires only epochs proven by a successor reacquire",
    "[wp216][wsi][present][retirement]") {
    BasePresentRetirementTracker retirement;
    CHECK(retirement.mayRetire(1, false));
    CHECK_FALSE(retirement.mayRetire(1, true));

    retirement.noteSuccessorImageReacquired(
        2, false);
    CHECK(retirement.watermark() == 0);
    CHECK_FALSE(retirement.mayRetire(1, true));

    retirement.noteSuccessorImageReacquired(
        2, true);
    CHECK(retirement.watermark() == 2);
    CHECK(retirement.mayRetire(1, true));
    CHECK_FALSE(retirement.mayRetire(2, true));

    retirement.noteSuccessorImageReacquired(
        4, true);
    CHECK(retirement.mayRetire(3, true));
}

TEST_CASE(
    "WP216 zero extent suspends and one positive revision resumes preparation",
    "[wp216][wsi][zero-extent]") {
    WindowOutputRecoveryStateMachine state{7, key(1)};
    state.observeZeroExtent(
        7,
        FramebufferExtentSnapshot{
            .extent = {0, 720},
            .revision = 2});
    CHECK(state.kind() ==
          WindowOutputStateKind::
              suspended_zero_extent);
    CHECK(state.reason() ==
          WindowOutputRecoveryReason::zero_extent);

    CHECK_FALSE(state.resumeFromPositiveExtent(
        7, key(3, {0, 720}), 10));
    CHECK(state.resumeFromPositiveExtent(
        7, key(3, {1920, 1080}), 10));
    CHECK(state.kind() ==
          WindowOutputStateKind::refresh_pending);
    const auto request =
        state.takePreparationRequest();
    REQUIRE(request);
    CHECK(request->old_epoch == 7);
    CHECK(request->key.framebuffer_revision == 3);
    CHECK(state.kind() ==
          WindowOutputStateKind::preparing);
}

TEST_CASE(
    "WP216 duplicate suboptimal notifications coalesce by recovery key",
    "[wp216][wsi][suboptimal][coalesce]") {
    WindowOutputRecoveryStateMachine state{4, key(9)};
    CHECK(state.requestRefresh(
        4, key(9),
        WindowOutputRecoveryReason::suboptimal, 1));
    CHECK_FALSE(state.requestRefresh(
        4, key(9),
        WindowOutputRecoveryReason::suboptimal, 1));
    REQUIRE(state.takePreparationRequest());
    CHECK_FALSE(state.requestRefresh(
        4, key(10),
        WindowOutputRecoveryReason::
            framebuffer_changed,
        2));
    CHECK(state.kind() ==
          WindowOutputStateKind::preparing);
    state.preparationSucceeded(5, key(9));
    CHECK(state.kind() ==
          WindowOutputStateKind::ready);
    CHECK_FALSE(state.requestRefresh(
        5, key(9),
        WindowOutputRecoveryReason::suboptimal, 2));

    CHECK(state.requestRefresh(
        5, key(10),
        WindowOutputRecoveryReason::suboptimal, 3));
}

TEST_CASE(
    "WP216 failed recovery backs off but a new revision bypasses the old key",
    "[wp216][wsi][retry]") {
    WindowOutputRecoveryStateMachine state{2, key(1)};
    REQUIRE(state.requestRefresh(
        2, key(2),
        WindowOutputRecoveryReason::out_of_date, 10));
    REQUIRE(state.takePreparationRequest());
    state.preparationFailed(10, 5);
    CHECK(state.kind() ==
          WindowOutputStateKind::unavailable_retry);
    CHECK_FALSE(state.requestRefresh(
        0, key(2),
        WindowOutputRecoveryReason::out_of_date, 12));
    CHECK_FALSE(state.retryIfDue(14));
    CHECK(state.retryIfDue(15));
    REQUIRE(state.takePreparationRequest());

    state.preparationFailed(15, 20);
    CHECK(state.requestRefresh(
        0, key(3),
        WindowOutputRecoveryReason::
            framebuffer_changed,
        16));
}

TEST_CASE(
    "WP216 resource pressure is deferred instead of becoming terminal",
    "[wp216][wsi][retry][oom]") {
    WindowOutputRecoveryStateMachine state{
        6, key(10)};
    state.deferRetry(
        key(10),
        WindowOutputRecoveryReason::
            resource_pressure,
        20, 4);
    CHECK(state.kind() ==
          WindowOutputStateKind::
              unavailable_retry);
    CHECK(state.reason() ==
          WindowOutputRecoveryReason::
              resource_pressure);
    CHECK_FALSE(state.retryIfDue(23));
    CHECK(state.retryIfDue(24));
    const auto request =
        state.takePreparationRequest();
    REQUIRE(request);
    CHECK(request->reason ==
          WindowOutputRecoveryReason::
              resource_pressure);
}

TEST_CASE(
    "WP217 surface loss prepares a distinct surface transaction and retries it",
    "[wp217][wsi][surface][retry]") {
    WindowOutputRecoveryStateMachine state{
        8, key(4)};
    state.markSurfaceLost(8);
    CHECK(state.kind() ==
          WindowOutputStateKind::surface_lost);

    const auto first =
        state.takeSurfacePreparationRequest(
            key(5));
    REQUIRE(first);
    CHECK(first->preparation_kind ==
          WindowOutputPreparationKind::surface);
    CHECK(first->old_epoch == 8);
    CHECK(first->attempt == 1);
    CHECK(state.attempt() == 1);
    CHECK(state.kind() ==
          WindowOutputStateKind::
              preparing_surface);

    state.markSurfaceLost(0);
    CHECK(state.attempt() == 1);
    REQUIRE(
        state.takeSurfacePreparationRequest(
            key(5)));
    state.preparationFailed(20, 3);
    CHECK(state.kind() ==
          WindowOutputStateKind::
              unavailable_retry);
    CHECK(state.reason() ==
          WindowOutputRecoveryReason::
              surface_lost);
    CHECK_FALSE(state.retryIfDue(22));
    CHECK(state.retryIfDue(23));
    CHECK(state.kind() ==
          WindowOutputStateKind::surface_lost);

    const auto second =
        state.takeSurfacePreparationRequest(
            key(6));
    REQUIRE(second);
    CHECK(second->attempt == 3);
    CHECK(state.attempt() == 3);
    state.preparationSucceeded(9, key(6));
    CHECK(state.kind() ==
          WindowOutputStateKind::ready);
}

TEST_CASE(
    "WP217 a new framebuffer revision bypasses surface retry without changing domains",
    "[wp217][wsi][surface][retry]") {
    WindowOutputRecoveryStateMachine state{
        5, key(1)};
    state.markSurfaceLost(5);
    REQUIRE(
        state.takeSurfacePreparationRequest(
            key(2)));
    state.preparationFailed(10, 30);
    CHECK_FALSE(state.requestRefresh(
        0, key(2),
        WindowOutputRecoveryReason::
            framebuffer_changed,
        11));
    CHECK(state.requestRefresh(
        0, key(3),
        WindowOutputRecoveryReason::
            framebuffer_changed,
        11));
    CHECK(state.kind() ==
          WindowOutputStateKind::surface_lost);
    const auto request =
        state.takeSurfacePreparationRequest(
            key(3));
    REQUIRE(request);
    CHECK(request->preparation_kind ==
          WindowOutputPreparationKind::surface);
    CHECK(request->attempt == 2);
}

TEST_CASE(
    "WP217 fresh surfaces prefer the current presentation queue then a created alternate",
    "[wp217][wsi][surface][queues]") {
    constexpr std::array created{2u, 5u, 7u};

    std::array<std::uint8_t, 8> support{};
    support[5] = 1;
    auto selection =
        selectSurfacePresentationQueue(
            5, created, support);
    CHECK(selection.kind ==
          SurfaceQueueBindingKind::compatible);
    CHECK(selection.presentation_queue_family == 5);

    support.fill(0);
    support[2] = 1;
    selection = selectSurfacePresentationQueue(
        5, created, support);
    CHECK(selection.kind ==
          SurfaceQueueBindingKind::compatible);
    CHECK(selection.presentation_queue_family == 2);
}

TEST_CASE(
    "WP217 successor reacquire evidence never crosses surface epochs",
    "[wp217][wsi][surface][retirement]") {
    BasePresentRetirementTracker lost_surface;
    BasePresentRetirementTracker fresh_surface;
    fresh_surface.noteSuccessorImageReacquired(
        9, true);

    CHECK_FALSE(
        lost_surface.mayRetire(4, true));
    CHECK(fresh_surface.mayRetire(4, true));
}

TEST_CASE(
    "WP217 fresh surfaces require device rebuild when presentation needs an uncreated queue",
    "[wp217][wsi][surface][queues]") {
    constexpr std::array created{1u, 3u};
    std::array<std::uint8_t, 6> support{};
    support[4] = 1;

    auto selection =
        selectSurfacePresentationQueue(
            3, created, support);
    CHECK(selection.kind ==
          SurfaceQueueBindingKind::
              device_rebuild_required);
    CHECK(selection.rebuild_reason ==
          SurfaceDeviceRebuildReason::
              presentation_queue_not_created);

    support.fill(0);
    selection = selectSurfacePresentationQueue(
        3, created, support);
    CHECK(selection.rebuild_reason ==
          SurfaceDeviceRebuildReason::
              no_presentation_support);
}

TEST_CASE(
    "WP217 zero extent preserves a pending surface replacement",
    "[wp217][wsi][surface][zero-extent]") {
    WindowOutputRecoveryStateMachine state{
        3, key(1)};
    state.markSurfaceLost(3);
    state.observeZeroExtent(
        3,
        FramebufferExtentSnapshot{
            .extent = {0, 0},
            .revision = 2});
    CHECK(state.kind() ==
          WindowOutputStateKind::
              suspended_zero_extent);
    CHECK(state.resumeFromPositiveExtent(
        0, key(3), 7));
    CHECK(state.kind() ==
          WindowOutputStateKind::surface_lost);
    const auto request =
        state.takeSurfacePreparationRequest(
            key(3));
    REQUIRE(request);
    CHECK(request->preparation_kind ==
          WindowOutputPreparationKind::surface);
}
