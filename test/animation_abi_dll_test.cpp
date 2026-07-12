#include "../src/core/userpublic/animation/abi_v1.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/model/skeletalanimation.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "fixtures/animation_abi/fixture_protocol.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <windows.h>

namespace Pelican::Animation {
namespace {

using LayoutFn = void (*)(AnimationAbiFixtureLayout *);
using WriteAdvanceFn = void (*)(void *, std::uint32_t);
using OldEngineFn = std::uint32_t (*)(void *, std::uint32_t);
using AcceptApiFn = std::uint32_t (*)(const void *, std::uint32_t);
using ServiceAvailableFn = std::uint32_t (*)(const void *, std::uint32_t);
using EvaluatorFn = std::uint32_t (*)(const ApiV1 *, AnimationEvaluatorFixtureResult *);

struct FixtureDll {
    HMODULE module{};
    explicit FixtureDll(const char *path) : module(LoadLibraryA(path)) { REQUIRE(module != nullptr); }
    ~FixtureDll() { if (module) FreeLibrary(module); }
    template <class T> T symbol(const char *name) const {
        const auto address = GetProcAddress(module, name);
        REQUIRE(address != nullptr);
        return reinterpret_cast<T>(address);
    }
};

AnimationAbiFixtureLayout layout(const FixtureDll &dll) {
    AnimationAbiFixtureLayout value{};
    dll.symbol<LayoutFn>("pelican_animation_fixture_layout")(&value);
    return value;
}

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

SkeletalModelData evaluatorAsset() {
    SkeletalModelData model;
    model.nodes = {
        {.parent = 1, .translation = {0.0f, 1.0f, 0.0f}, .name = "Tip"},
        {.parent = -1, .name = "Root"},
    };
    model.joint_nodes = {0};
    model.inverse_bind_matrices = {glm::mat4{1.0f}};
    model.skin_bindings = {{"Body", 0, 1}};
    model.clips.push_back({
        .name = "MoveA", .start = 0.0f, .end = 1.0f,
        .channels = {{.node = 0, .path = AnimationPath::translation,
                      .times = {0.0f, 1.0f}, .values = {{0, 1, 0, 0}, {2, 1, 0, 0}}}},
    });
    model.clips.push_back({
        .name = "MoveB", .start = 0.0f, .end = 1.0f,
        .channels = {{.node = 0, .path = AnimationPath::translation,
                      .times = {0.0f, 1.0f}, .values = {{4, 1, 0, 0}, {8, 1, 0, 0}}}},
    });
    return model;
}

} // namespace

TEST_CASE("old and new animation header DLLs preserve the frozen prefix", "[animation][a0][abi][dll]") {
    FixtureDll old_dll{PELICAN_ANIM_OLD_DLL};
    FixtureDll new_dll{PELICAN_ANIM_NEW_DLL};
    const auto old_layout = layout(old_dll);
    const auto new_layout = layout(new_dll);
    REQUIRE(old_layout.fixture_generation == 1);
    REQUIRE(new_layout.fixture_generation == 2);
    REQUIRE(old_layout.advance_size == new_layout.advance_size);
    REQUIRE(old_layout.advance_cursor_offset == new_layout.advance_cursor_offset);
    REQUIRE(old_layout.interval_crossings_offset == new_layout.interval_crossings_offset);
    REQUIRE(old_layout.api_context_offset == new_layout.api_context_offset);
    REQUIRE(old_layout.interval_size < new_layout.interval_size);
    REQUIRE(old_layout.api_size < new_layout.api_size);

    alignas(AdvanceDescV1) std::array<std::byte, sizeof(AdvanceDescV1) + 32> bytes{};
    bytes.fill(std::byte{0x5a});
    old_dll.symbol<WriteAdvanceFn>("pelican_animation_fixture_write_advance")(
        bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    const auto *old_desc = reinterpret_cast<const AdvanceDescV1 *>(bytes.data());
    REQUIRE(old_desc->struct_size == old_layout.advance_size);
    REQUIRE(old_desc->cursor.generation == 1);
    REQUIRE(std::to_integer<unsigned>(bytes[sizeof(AdvanceDescV1)]) == 0x5a);
    bytes.fill(std::byte{0x5a});
    new_dll.symbol<WriteAdvanceFn>("pelican_animation_fixture_write_advance")(
        bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    const auto *new_desc = reinterpret_cast<const AdvanceDescV1 *>(bytes.data());
    REQUIRE(new_desc->cursor.generation == 2);
    REQUIRE(std::to_integer<unsigned>(bytes[sizeof(AdvanceDescV1)]) == 0x5a);
}

TEST_CASE("old-client new-engine and new-client old-engine negotiate additively", "[animation][a0][abi][dll]") {
    FixtureDll old_dll{PELICAN_ANIM_OLD_DLL};
    FixtureDll new_dll{PELICAN_ANIM_NEW_DLL};
    const auto old_layout = layout(old_dll);

    alignas(ApiV1) std::array<std::byte, sizeof(ApiV1) + 32> storage{};
    storage.fill(std::byte{0x5a});
    auto *api = reinterpret_cast<ApiV1 *>(storage.data());
    api->struct_size = old_layout.api_size;
    api->version = 1;
    api->reserved0 = 0;
    api->reserved1 = 0;
    REQUIRE(getApiV1(1, api) == Status::ok);
    REQUIRE(old_dll.symbol<AcceptApiFn>("pelican_animation_fixture_accept_api")(api, old_layout.api_size) == 1);
    REQUIRE(std::to_integer<unsigned>(storage[old_layout.api_size]) == 0x5a);

    storage.fill(std::byte{0x5a});
    api = reinterpret_cast<ApiV1 *>(storage.data());
    api->struct_size = sizeof(ApiV1);
    api->version = 1;
    const auto written = old_dll.symbol<OldEngineFn>("pelican_animation_fixture_old_engine")(api, sizeof(ApiV1));
    REQUIRE(written == old_layout.api_size);
    REQUIRE(new_dll.symbol<AcceptApiFn>("pelican_animation_fixture_accept_api")(api, sizeof(ApiV1)) == 1);
    REQUIRE(std::to_integer<unsigned>(storage[old_layout.api_size]) == 0x5a);
    REQUIRE(new_dll.symbol<ServiceAvailableFn>("pelican_animation_fixture_service_available")(
                api, old_layout.api_size) == 0);
}

TEST_CASE("A1.1 service table negotiates without changing the frozen ApiV1 prefix",
          "[animation][a1.1][abi][dll]") {
    FixtureDll new_dll{PELICAN_ANIM_NEW_DLL};
    auto api = descriptor<ApiV1>();
    REQUIRE(getApiV1(1, &api) == Status::ok);
    REQUIRE(new_dll.symbol<ServiceAvailableFn>("pelican_animation_fixture_service_available")(
                &api, sizeof(api)) == 1);
    REQUIRE(api.get_animation_service != nullptr);

    alignas(AnimationServiceV1) std::array<std::byte, sizeof(AnimationServiceV1) + 32> storage{};
    storage.fill(std::byte{0x5a});
    auto *service = reinterpret_cast<AnimationServiceV1 *>(storage.data());
    service->struct_size = sizeof(AnimationServiceV1);
    service->version = descriptorVersionV1;
    service->reserved0 = 0;
    service->reserved1 = 0;
    REQUIRE(api.get_animation_service(api.context, animationServiceVersionV1, service) == Status::ok);
    REQUIRE(service->struct_size == sizeof(AnimationServiceV1));
    REQUIRE((service->capability_bits & animationServiceCapabilitiesV1) == animationServiceCapabilitiesV1);
    REQUIRE(service->resolve_sink != nullptr);
    REQUIRE(service->publish_animation_frame_from_source != nullptr);
    REQUIRE(std::to_integer<unsigned>(storage[sizeof(AnimationServiceV1)]) == 0x5a);

    auto unsupported = descriptor<AnimationServiceV1>();
    REQUIRE(api.get_animation_service(api.context, animationServiceVersionV1 + 1, &unsupported) ==
            Status::unsupported_version);
}

TEST_CASE("third-party DLL drives clip blend palette and authority commit through public animation headers only",
          "[animation][a1.1][abi][dll][adversarial]") {
    auto model = evaluatorAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset();
    runtime.registerObject("Hero", model);

    auto api = descriptor<ApiV1>();
    REQUIRE(getApiV1(1, &api) == Status::ok);
    FixtureDll evaluator{PELICAN_ANIM_EVALUATOR_DLL};
    AnimationEvaluatorFixtureResult result{};
    constexpr internal::RegistrationOwner evaluator_owner = 7001;
    {
        internal::ScopedRegistrationOwner owner_scope{evaluator_owner};
        REQUIRE(evaluator.symbol<EvaluatorFn>("pelican_animation_public_evaluator")(&api, &result) ==
                static_cast<std::uint32_t>(Status::ok));
    }
    REQUIRE(result.status == static_cast<std::uint32_t>(Status::ok));
    REQUIRE(result.cursor_advance_status == static_cast<std::uint32_t>(Status::ok));
    REQUIRE(result.single_clip_translation_x == Catch::Approx(1.0f));
    REQUIRE(result.blended_translation_x == Catch::Approx(3.875f));
    REQUIRE(result.palette_translation_x == Catch::Approx(3.875f));

    const AnimationSinkHandle sink{result.sink_identity, result.sink_generation, 0};
    REQUIRE(runtime.runPhases(sink, 43) == Status::ok);
    REQUIRE(result.phase_callback_count == 1);

    auto service = descriptor<AnimationServiceV1>();
    REQUIRE(api.get_animation_service(api.context, animationServiceVersionV1, &service) == Status::ok);
    CurrentAnimationOwnerDescV1 next_owner = descriptor<CurrentAnimationOwnerDescV1>();
    constexpr internal::RegistrationOwner replacement_owner = 7002;
    {
        internal::ScopedRegistrationOwner owner_scope{replacement_owner};
        REQUIRE(service.get_current_owner(service.context, &next_owner) == Status::ok);
    }
    HandoffAnimationSourceDescV1 handoff = descriptor<HandoffAnimationSourceDescV1>();
    handoff.current_source = {result.source_identity, result.source_generation, 0};
    handoff.next_owner = next_owner.owner;
    handoff.next_source_ordinal = 8;
    REQUIRE(service.handoff_source(service.context, &handoff) == Status::ok);

    for (std::uint32_t i = 0; i < 4; ++i) {
        AnimationNotificationDescV1 notification = descriptor<AnimationNotificationDescV1>();
        notification.kind = static_cast<AnimationNotificationKind>(i + 1);
        notification.sink = sink;
        notification.source = handoff.next_source;
        notification.time_seconds = 1.0 + i;
        notification.notification_revision = i + 2;
        REQUIRE(service.notify(service.context, &notification) == Status::ok);
    }
    ReleaseAnimationSourceDescV1 release = descriptor<ReleaseAnimationSourceDescV1>();
    release.owner = next_owner.owner;
    release.source = handoff.next_source;
    REQUIRE(service.release_source(service.context, &release) == Status::ok);

    releaseAnimationOwner(evaluator_owner);
    REQUIRE(runtime.runPhases(sink, 44) == Status::ok);
    REQUIRE(result.phase_callback_count == 1);
    runtime.reset();
}

} // namespace Pelican::Animation
