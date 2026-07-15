#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/log.hpp"
#include "../src/core/phys/physicsruntime.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "../src/core/userpublic/physics/abi_v1.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <semaphore>
#include <thread>
#include <vector>

namespace Pelican {
namespace {

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

ColliderComponent sphereCollider(float radius = 1.0F) {
    ColliderComponent collider;
    collider.shape = "sphere";
    collider.radius = radius;
    return collider;
}

Physics::ApiV1 physicsApi() {
    auto api = Physics::descriptor<Physics::ApiV1>();
    REQUIRE(Physics::getApiV1(Physics::abiVersionV1, &api) == Physics::Status::ok);
    return api;
}

Physics::ServiceV1 physicsService(const Physics::ApiV1 &api) {
    auto service = Physics::descriptor<Physics::ServiceV1>();
    REQUIRE(api.get_service(api.context, Physics::serviceVersionV1, &service) ==
            Physics::Status::ok);
    return service;
}

struct FakeProviderState {
    enum class RaycastBehavior {
        valid,
        duplicate_index,
        out_of_range_index,
        zero_normal,
        unknown_status,
        oversized_count,
        unexpected_buffer_too_small,
    };

    std::uint32_t last_collider_count = 0;
    Physics::Vec3V1 last_direction{};
    RaycastBehavior raycast_behavior = RaycastBehavior::valid;
};

Physics::Status fakeRaycastAll(void *context,
                               const Physics::ProviderRaycastQueryV1 *query,
                               Physics::ProviderRaycastHitV1 *hits,
                               std::uint32_t capacity,
                               std::uint32_t *out_count) noexcept {
    if (context == nullptr || query == nullptr || out_count == nullptr ||
        (capacity != 0 && hits == nullptr)) {
        return Physics::Status::invalid_argument;
    }
    auto &state = *static_cast<FakeProviderState *>(context);
    state.last_collider_count = query->collider_count;
    state.last_direction = query->ray.direction;

    if (state.raycast_behavior == FakeProviderState::RaycastBehavior::unknown_status) {
        *out_count = 0;
        return static_cast<Physics::Status>(~std::uint32_t{0});
    }
    if (state.raycast_behavior == FakeProviderState::RaycastBehavior::oversized_count) {
        *out_count = query->collider_count + 1;
        return Physics::Status::ok;
    }
    if (state.raycast_behavior ==
        FakeProviderState::RaycastBehavior::unexpected_buffer_too_small) {
        *out_count = query->collider_count;
        return Physics::Status::buffer_too_small;
    }

    *out_count = query->collider_count;
    if (capacity < query->collider_count) return Physics::Status::buffer_too_small;
    for (std::uint32_t output = 0; output < query->collider_count; ++output) {
        const auto collider_index = query->collider_count - output - 1;
        hits[output] = Physics::ProviderRaycastHitV1{
            .collider_index = collider_index,
            .distance = 0.25F,
            // Pelican owns canonical hit positions and normalizes this value.
            .normal = {-2.0F, 0.0F, 0.0F},
        };
    }
    if (query->collider_count != 0) {
        if (state.raycast_behavior == FakeProviderState::RaycastBehavior::out_of_range_index) {
            hits[0].collider_index = query->collider_count;
        } else if (state.raycast_behavior == FakeProviderState::RaycastBehavior::zero_normal) {
            hits[0].normal = {};
        }
    }
    if (query->collider_count > 1 &&
        state.raycast_behavior == FakeProviderState::RaycastBehavior::duplicate_index) {
        hits[1].collider_index = hits[0].collider_index;
    }
    return Physics::Status::ok;
}

Physics::ProviderV1 fakeProvider(FakeProviderState &state) {
    auto provider = Physics::descriptor<Physics::ProviderV1>();
    provider.capability_bits = Physics::query_raycast_all;
    provider.name_utf8 = "test.fake";
    provider.name_size = 9;
    provider.context = &state;
    provider.raycast_all = fakeRaycastAll;
    return provider;
}

struct BlockingProviderState {
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
};

Physics::Status blockingRaycastAll(void *context,
                                   const Physics::ProviderRaycastQueryV1 *query,
                                   Physics::ProviderRaycastHitV1 *hits,
                                   std::uint32_t capacity,
                                   std::uint32_t *out_count) noexcept {
    if (context == nullptr || query == nullptr || out_count == nullptr ||
        query->collider_count != 1 || capacity != 1 || hits == nullptr) {
        return Physics::Status::invalid_argument;
    }
    auto &state = *static_cast<BlockingProviderState *>(context);
    state.entered.release();
    state.resume.acquire();
    hits[0] = Physics::ProviderRaycastHitV1{
        .collider_index = 0,
        .distance = 0.5F,
        .normal = {-1.0F, 0.0F, 0.0F},
    };
    *out_count = 1;
    return Physics::Status::ok;
}

Physics::ProviderV1 blockingProvider(BlockingProviderState &state) {
    auto provider = Physics::descriptor<Physics::ProviderV1>();
    provider.capability_bits = Physics::query_raycast_all;
    provider.name_utf8 = "test.blocking";
    provider.name_size = 13;
    provider.context = &state;
    provider.raycast_all = blockingRaycastAll;
    return provider;
}

constexpr const char *configuredProviderName() {
#if PELICAN_WITH_JOLT_PHYSICS
    return "pelican.jolt";
#elif PELICAN_WITH_BUILTIN_PHYSICS
    return "pelican.builtin";
#else
    return "";
#endif
}

} // namespace

#if PELICAN_WITH_JOLT_PHYSICS || PELICAN_WITH_BUILTIN_PHYSICS
TEST_CASE("PhysicsServiceV1 negotiates and exposes the configured provider",
          "[physics-service]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    world.bindCollider("target", sphereCollider(),
                       PhysWorldTransform{.pos = {3.0F, 0.0F, 0.0F}});

    const auto api = physicsApi();
    REQUIRE((api.capability_bits & Physics::api_query_service) != 0);
    REQUIRE((api.capability_bits & Physics::api_provider_registration) != 0);
    const auto service = physicsService(api);
    REQUIRE((service.capability_bits & Physics::builtinQueryCapabilitiesV1) ==
            Physics::builtinQueryCapabilitiesV1);

    auto query = Physics::descriptor<Physics::RaycastQueryV1>();
    query.ray.origin = {0.0F, 0.0F, 0.0F};
    query.ray.direction = {1.0F, 0.0F, 0.0F};
    query.ray.max_distance = 10.0F;

    std::uint32_t hit_count = 0;
    REQUIRE(service.raycast_all(service.context, &query, nullptr, 0, &hit_count) ==
            Physics::Status::buffer_too_small);
    REQUIRE(hit_count == 1);
    std::array<Physics::RaycastHitV1, 1> hits{};
    REQUIRE(service.raycast_all(service.context, &query, hits.data(),
                                static_cast<std::uint32_t>(hits.size()), &hit_count) ==
            Physics::Status::ok);
    REQUIRE(hit_count == 1);
    REQUIRE(hits[0].identity.collider_id != 0);
    REQUIRE(hits[0].distance == Catch::Approx(2.0F));

    query.ray.max_distance = 2.0F;
    REQUIRE(service.raycast_all(service.context, &query, nullptr, 0, &hit_count) ==
            Physics::Status::buffer_too_small);
    REQUIRE(hit_count == 1);
    query.ray.max_distance = 10.0F;

    query.filter.self_collider_id = hits[0].identity.collider_id;
    REQUIRE(service.raycast_all(service.context, &query, nullptr, 0, &hit_count) ==
            Physics::Status::ok);
    REQUIRE(hit_count == 0);

    auto overlap = Physics::descriptor<Physics::OverlapQueryV1>();
    overlap.shape.kind = Physics::ShapeKindV1::sphere;
    overlap.shape.center = {3.0F, 0.0F, 0.0F};
    overlap.shape.radius = 0.25F;
    REQUIRE(service.overlap_all(service.context, &overlap, nullptr, 0, &hit_count) ==
            Physics::Status::buffer_too_small);
    REQUIRE(hit_count == 1);

    overlap.shape.center = {1.75F, 0.0F, 0.0F};
    REQUIRE(service.overlap_all(service.context, &overlap, nullptr, 0, &hit_count) ==
            Physics::Status::buffer_too_small);
    REQUIRE(hit_count == 1);
}
#endif

#if PELICAN_WITH_JOLT_PHYSICS
TEST_CASE("Jolt provider handles box and capsule queries without exposing Jolt types",
          "[physics-service][jolt]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);

    ColliderComponent box;
    box.shape = "box";
    box.half_extents = {0.5F, 0.5F, 0.5F};
    world.bindCollider("box", box, PhysWorldTransform{.pos = {3.0F, 0.0F, 0.0F}});

    ColliderComponent capsule;
    capsule.shape = "capsule";
    capsule.radius = 0.5F;
    capsule.half_height = 1.0F;
    world.bindCollider("capsule", capsule,
                       PhysWorldTransform{.pos = {6.0F, 0.0F, 0.0F}});

    REQUIRE(physics_internal::activeProviderName() == "pelican.jolt");
    const auto hits = world.raycastAll(
        phys::Ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F});
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].id == "box");
    REQUIRE(hits[0].distance == Catch::Approx(2.5F));
    REQUIRE(hits[1].id == "capsule");
    REQUIRE(hits[1].distance == Catch::Approx(5.5F));

    const auto inside_hit = world.raycastClosest(
        phys::Ray{{3.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F});
    REQUIRE(inside_hit.has_value());
    REQUIRE(inside_hit->id == "box");
    REQUIRE(inside_hit->distance == Catch::Approx(0.5F));

    const auto overlaps = world.overlapAll(
        phys::Sphere{{6.0F, 1.25F, 0.0F}, 0.3F});
    REQUIRE(overlaps.size() == 1);
    REQUIRE(overlaps[0] == "capsule");
}
#endif

TEST_CASE("game-owned physics provider activates explicitly and is released by owner",
          "[physics-service]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    world.bindCollider("first", sphereCollider(),
                       PhysWorldTransform{.pos = {3.0F, 0.0F, 0.0F}});
    world.bindCollider("second", sphereCollider(),
                       PhysWorldTransform{.pos = {5.0F, 0.0F, 0.0F}});

    const auto api = physicsApi();
    FakeProviderState state;
    auto provider = fakeProvider(state);
    const auto owner = internal::allocateRegistrationOwner();
    Physics::ProviderHandleV1 handle{};
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) == Physics::Status::ok);
        Physics::ProviderHandleV1 duplicate_handle{};
        REQUIRE(api.register_provider(api.context, &provider, &duplicate_handle) ==
                Physics::Status::duplicate_provider);
    }

    REQUIRE(physics_internal::activeProviderName() == configuredProviderName());
    physics_internal::activateProviderOwner(owner);
    REQUIRE(physics_internal::activeProviderName() == "test.fake");

    const auto active_service = physicsService(api);
    REQUIRE((active_service.capability_bits & Physics::builtinQueryCapabilitiesV1) ==
            Physics::builtinQueryCapabilitiesV1);

    const phys::QueryFilter filter{};
    const auto hits = world.raycastAll(
        phys::Ray{{0.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F}, 10.0F}, filter);
    REQUIRE(state.last_collider_count == 2);
    REQUIRE(state.last_direction.x == Catch::Approx(1.0F));
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].id == "first");
    REQUIRE(hits[1].id == "second");
    REQUIRE(hits[0].distance == Catch::Approx(0.25F));
    REQUIRE(hits[0].position.x == Catch::Approx(0.25F));
    REQUIRE(hits[0].position.y == Catch::Approx(0.0F));
    REQUIRE(hits[0].normal.x == Catch::Approx(-1.0F));

    // The game provider implements raycast only. Other capabilities fall back
    // independently to the configured provider when one is compiled in.
    const auto fallback_overlap = world.overlapAll(
        phys::Sphere{{3.0F, 0.0F, 0.0F}, 0.25F});
#if PELICAN_WITH_JOLT_PHYSICS || PELICAN_WITH_BUILTIN_PHYSICS
    REQUIRE(fallback_overlap == std::vector<std::string>{"first"});
#else
    REQUIRE(fallback_overlap.empty());
#endif

    const phys::QueryFilter ignore_first{.self = hits[0].identity.collider_id};
    const auto filtered = world.raycastAll(
        phys::Ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F}, ignore_first);
    REQUIRE(state.last_collider_count == 1);
    REQUIRE(filtered.size() == 1);
    REQUIRE(filtered[0].id == "second");

    physics_internal::releaseProviderOwner(owner);
#if PELICAN_WITH_JOLT_PHYSICS || PELICAN_WITH_BUILTIN_PHYSICS
    REQUIRE(physics_internal::activeProviderName() == configuredProviderName());
    const auto configured_hit = world.raycastClosest(
        phys::Ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F});
    REQUIRE(configured_hit.has_value());
    REQUIRE(configured_hit->distance == Catch::Approx(2.0F));
#else
    REQUIRE(physics_internal::activeProviderName().empty());
    REQUIRE_FALSE(world.raycastClosest(
        phys::Ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F}).has_value());
#endif
}

TEST_CASE("provider owner release waits for in-flight callbacks",
          "[physics-service][concurrency]") {
    using namespace std::chrono_literals;

    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    world.bindCollider("target", sphereCollider(),
                       PhysWorldTransform{.pos = {3.0F, 0.0F, 0.0F}});

    const auto api = physicsApi();
    BlockingProviderState state;
    auto provider = blockingProvider(state);
    const auto owner = internal::allocateRegistrationOwner();
    Physics::ProviderHandleV1 handle{};
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) ==
                Physics::Status::ok);
    }
    physics_internal::activateProviderOwner(owner);

    std::vector<phys::RaycastQueryHit> query_hits;
    std::exception_ptr query_error;
    std::thread query_thread{[&] {
        try {
            query_hits = world.raycastAll(
                phys::Ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 10.0F});
        } catch (...) {
            query_error = std::current_exception();
        }
    }};

    const bool callback_entered = state.entered.try_acquire_for(2s);
    if (!callback_entered) {
        state.resume.release();
        query_thread.join();
        physics_internal::releaseProviderOwner(owner);
        REQUIRE(callback_entered);
    }

    std::binary_semaphore release_started{0};
    std::binary_semaphore release_finished{0};
    std::thread release_thread{[&] {
        release_started.release();
        physics_internal::releaseProviderOwner(owner);
        release_finished.release();
    }};
    release_started.acquire();
    const bool released_while_callback_blocked = release_finished.try_acquire_for(100ms);

    state.resume.release();
    query_thread.join();
    release_thread.join();

    REQUIRE_FALSE(released_while_callback_blocked);
    REQUIRE(query_error == nullptr);
    REQUIRE(query_hits.size() == 1);
    REQUIRE(query_hits[0].distance == Catch::Approx(0.5F));
}

TEST_CASE("PhysicsServiceV1 contains malformed provider results",
          "[physics-service]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    world.bindCollider("first", sphereCollider(),
                       PhysWorldTransform{.pos = {3.0F, 0.0F, 0.0F}});
    world.bindCollider("second", sphereCollider(),
                       PhysWorldTransform{.pos = {5.0F, 0.0F, 0.0F}});

    const auto api = physicsApi();
    const auto service = physicsService(api);
    FakeProviderState state;
    auto provider = fakeProvider(state);
    const auto owner = internal::allocateRegistrationOwner();
    Physics::ProviderHandleV1 handle{};
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) == Physics::Status::ok);
    }
    physics_internal::activateProviderOwner(owner);

    auto query = Physics::descriptor<Physics::RaycastQueryV1>();
    query.ray.origin = {0.0F, 0.0F, 0.0F};
    query.ray.direction = {1.0F, 0.0F, 0.0F};
    query.ray.max_distance = 10.0F;
    std::array<Physics::RaycastHitV1, 2> hits{};
    std::uint32_t hit_count = 0;

    constexpr std::array malformed_behaviors{
        FakeProviderState::RaycastBehavior::duplicate_index,
        FakeProviderState::RaycastBehavior::out_of_range_index,
        FakeProviderState::RaycastBehavior::zero_normal,
        FakeProviderState::RaycastBehavior::unknown_status,
        FakeProviderState::RaycastBehavior::oversized_count,
        FakeProviderState::RaycastBehavior::unexpected_buffer_too_small,
    };
    for (const auto behavior : malformed_behaviors) {
        state.raycast_behavior = behavior;
        REQUIRE(service.raycast_all(service.context, &query, hits.data(),
                                    static_cast<std::uint32_t>(hits.size()), &hit_count) ==
                Physics::Status::provider_error);
    }

    physics_internal::releaseProviderOwner(owner);
}

TEST_CASE("PhysicsServiceV1 rejects malformed descriptors and unknown provider bits",
          "[physics-service]") {
    auto api = Physics::descriptor<Physics::ApiV1>();
    api.reserved0 = 1;
    REQUIRE(Physics::getApiV1(Physics::abiVersionV1, &api) ==
            Physics::Status::reserved_not_zero);

    api = Physics::descriptor<Physics::ApiV1>();
    REQUIRE(Physics::getApiV1(Physics::abiVersionV1 + 1, &api) ==
            Physics::Status::unsupported_version);

    api = physicsApi();
    FakeProviderState state;
    auto provider = fakeProvider(state);
    Physics::ProviderHandleV1 handle{};
    REQUIRE(api.register_provider(api.context, &provider, &handle) ==
            Physics::Status::wrong_owner);

    provider.capability_bits |= 1ULL << 63U;
    const auto owner = internal::allocateRegistrationOwner();
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) ==
                Physics::Status::invalid_argument);
    }
}

} // namespace Pelican
