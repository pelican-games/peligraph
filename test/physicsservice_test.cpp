#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/log.hpp"
#include "../src/core/phys/physicsruntime.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "../src/core/userpublic/physics/abi_v2.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <limits>
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

Physics::ApiV2 physicsApi() {
    auto api = Physics::descriptor<Physics::ApiV2>();
    REQUIRE(Physics::getApiV2(Physics::abiVersionV2, &api) == Physics::Status::ok);
    return api;
}

Physics::ServiceV2 physicsService(const Physics::ApiV2 &api) {
    auto service = Physics::descriptor<Physics::ServiceV2>();
    REQUIRE(api.get_service(api.context, Physics::serviceVersionV2, &service) ==
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

Physics::ProviderV2 fakeProvider(FakeProviderState &state) {
    auto provider = Physics::descriptor<Physics::ProviderV2>();
    provider.capability_bits = Physics::query_raycast_all;
    provider.name_utf8 = "test.fake";
    provider.name_size = 9;
    provider.context = &state;
    provider.raycast_all = fakeRaycastAll;
    return provider;
}

struct FakeShapeProviderState {
    enum class Behavior {
        valid,
        duplicate_index,
        out_of_range_index,
        zero_normal,
        invalid_toi,
        initial_without_depth,
        initial_at_contact_depth,
        sweep_with_depth,
        unknown_flags,
        non_finite_position,
        unknown_status,
        oversized_count,
        unexpected_buffer_too_small,
    };

    std::uint32_t last_collider_count = 0;
    Physics::Vec3V1 last_delta{};
    Behavior behavior = Behavior::valid;
};

Physics::Status fakeShapeCastAll(
    void *context, const Physics::ProviderShapeCastQueryV2 *query,
    Physics::ProviderShapeCastHitV2 *hits, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    if (context == nullptr || query == nullptr || out_count == nullptr ||
        (capacity != 0 && hits == nullptr)) {
        return Physics::Status::invalid_argument;
    }
    auto &state = *static_cast<FakeShapeProviderState *>(context);
    state.last_collider_count = query->collider_count;
    state.last_delta = query->delta;

    if (state.behavior == FakeShapeProviderState::Behavior::unknown_status) {
        *out_count = 0;
        return static_cast<Physics::Status>(~std::uint32_t{0});
    }
    if (state.behavior == FakeShapeProviderState::Behavior::oversized_count) {
        *out_count = query->collider_count + 1;
        return Physics::Status::ok;
    }
    if (state.behavior ==
        FakeShapeProviderState::Behavior::unexpected_buffer_too_small) {
        *out_count = query->collider_count;
        return Physics::Status::buffer_too_small;
    }

    *out_count = query->collider_count;
    if (capacity < query->collider_count) return Physics::Status::buffer_too_small;
    for (std::uint32_t output = 0; output < query->collider_count; ++output) {
        hits[output] = Physics::ProviderShapeCastHitV2{
            .collider_index = query->collider_count - output - 1,
            .time_of_impact = 0.25F,
            .position = {2.5F, 0.0F, 0.0F},
            .normal = {-2.0F, 0.0F, 0.0F},
        };
    }
    if (query->collider_count == 0) return Physics::Status::ok;

    switch (state.behavior) {
    case FakeShapeProviderState::Behavior::duplicate_index:
        if (query->collider_count > 1) hits[1].collider_index = hits[0].collider_index;
        break;
    case FakeShapeProviderState::Behavior::out_of_range_index:
        hits[0].collider_index = query->collider_count;
        break;
    case FakeShapeProviderState::Behavior::zero_normal:
        hits[0].normal = {};
        break;
    case FakeShapeProviderState::Behavior::invalid_toi:
        hits[0].time_of_impact = 1.5F;
        break;
    case FakeShapeProviderState::Behavior::initial_without_depth:
        hits[0].flags = Physics::shape_cast_initial_overlap;
        hits[0].time_of_impact = 0.0F;
        break;
    case FakeShapeProviderState::Behavior::initial_at_contact_depth:
        hits[0].flags = Physics::shape_cast_initial_overlap;
        hits[0].time_of_impact = 0.0F;
        hits[0].penetration_depth = Physics::shapeCastContactEpsilonV2;
        break;
    case FakeShapeProviderState::Behavior::sweep_with_depth:
        hits[0].penetration_depth = 1.0F;
        break;
    case FakeShapeProviderState::Behavior::unknown_flags:
        hits[0].flags = 1U << 31U;
        break;
    case FakeShapeProviderState::Behavior::non_finite_position:
        hits[0].position.x = std::numeric_limits<float>::infinity();
        break;
    default:
        break;
    }
    return Physics::Status::ok;
}

Physics::ProviderV2 fakeShapeProvider(FakeShapeProviderState &state) {
    auto provider = Physics::descriptor<Physics::ProviderV2>();
    provider.capability_bits = Physics::query_shape_cast_all;
    provider.name_utf8 = "test.shape-v2";
    provider.name_size = 13;
    provider.context = &state;
    provider.shape_cast_all = fakeShapeCastAll;
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

Physics::ProviderV2 blockingProvider(BlockingProviderState &state) {
    auto provider = Physics::descriptor<Physics::ProviderV2>();
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
TEST_CASE("PhysicsServiceV2 negotiates and exposes the configured provider",
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
    REQUIRE((service.capability_bits & Physics::builtinQueryCapabilitiesV2) ==
            Physics::builtinQueryCapabilitiesV2);

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

TEST_CASE("configured provider shape-casts every standard convex pair",
          "[physics-service][shape-cast]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);

    const std::array<phys::Shape, 3> moving_shapes{
        phys::Sphere{{0.0F, 0.0F, 0.0F}, 0.5F},
        phys::Box{{0.0F, 0.0F, 0.0F}, {}, {0.4F, 0.6F, 0.5F}},
        phys::Capsule{{0.0F, 0.0F, 0.0F}, {}, 0.6F, 0.35F},
    };

    for (int target_kind = 0; target_kind < 3; ++target_kind) {
        world.clear();
        ColliderComponent target;
        if (target_kind == 0) {
            target.shape = "sphere";
            target.radius = 0.5F;
        } else if (target_kind == 1) {
            target.shape = "box";
            target.half_extents = {0.05F, 1.0F, 1.0F};
        } else {
            target.shape = "capsule";
            target.half_height = 0.8F;
            target.radius = 0.35F;
        }
        world.bindCollider("target", target,
                           PhysWorldTransform{.pos = {5.0F, 0.0F, 0.0F}});

        for (const auto &moving : moving_shapes) {
            const auto hit = world.shapeCastClosest(
                moving, {10.0F, 0.0F, 0.0F});
            INFO("moving=" << moving.index() << " target=" << target_kind);
            REQUIRE(hit);
            REQUIRE_FALSE(hit->initial_overlap);
            REQUIRE(hit->time_of_impact >= 0.0F);
            REQUIRE(hit->time_of_impact <= 1.0F);
            REQUIRE(hit->normal.x < -0.5F);
        }
    }

    world.clear();
    auto sphere = sphereCollider(0.5F);
    world.bindCollider("target", sphere,
                       PhysWorldTransform{.pos = {5.0F, 0.0F, 0.0F}});
    const auto exact = world.shapeCastClosest(
        phys::Sphere{{0.0F, 0.0F, 0.0F}, 0.5F},
        {10.0F, 0.0F, 0.0F});
    REQUIRE(exact);
    REQUIRE(exact->time_of_impact == Catch::Approx(0.4F).margin(2.0e-4F));
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

    world.clear();
    world.bindCollider("sphere", sphereCollider(), PhysWorldTransform{});
    const auto penetration = world.shapeCastClosest(
        phys::Sphere{{0.5F, 0.0F, 0.0F}, 1.0F}, {});
    REQUIRE(penetration);
    REQUIRE(penetration->initial_overlap);
    REQUIRE(penetration->penetration_depth ==
            Catch::Approx(1.5F).margin(2.0e-3F));
    REQUIRE(penetration->normal.x == Catch::Approx(1.0F).margin(2.0e-3F));

    const auto symmetric_penetration = world.shapeCastClosest(
        phys::Sphere{{0.0F, 0.0F, 0.0F}, 1.0F}, {});
    REQUIRE(symmetric_penetration);
    REQUIRE(symmetric_penetration->initial_overlap);
    REQUIRE(symmetric_penetration->penetration_depth ==
            Catch::Approx(2.0F).margin(2.0e-3F));
    REQUIRE(symmetric_penetration->normal.x ==
            Catch::Approx(1.0F).margin(2.0e-3F));

    REQUIRE_FALSE(world.shapeCastClosest(
        phys::Sphere{{2.0F, 0.0F, 0.0F}, 1.0F},
        {1.0F, 0.0F, 0.0F}));
    const auto touching_approach = world.shapeCastClosest(
        phys::Sphere{{2.0F, 0.0F, 0.0F}, 1.0F},
        {-1.0F, 0.0F, 0.0F});
    REQUIRE(touching_approach);
    REQUIRE_FALSE(touching_approach->initial_overlap);
    REQUIRE(touching_approach->time_of_impact == Catch::Approx(0.0F));

    world.clear();
    world.bindCollider("box", box, PhysWorldTransform{});
    const auto symmetric_box = world.shapeCastClosest(
        phys::Box{{0.0F, 0.0F, 0.0F}, {}, {0.5F, 0.5F, 0.5F}}, {});
    REQUIRE(symmetric_box);
    REQUIRE(symmetric_box->initial_overlap);
    REQUIRE(symmetric_box->penetration_depth ==
            Catch::Approx(1.0F).margin(2.0e-3F));
    REQUIRE(symmetric_box->normal.x == Catch::Approx(1.0F).margin(2.0e-3F));

    const auto moving_box_tie = world.shapeCastClosest(
        phys::Box{{0.0F, 0.0F, 0.0F}, {}, {0.5F, 0.5F, 0.5F}},
        {0.0F, 1.0F, 0.0F});
    REQUIRE(moving_box_tie);
    REQUIRE(moving_box_tie->normal.y == Catch::Approx(-1.0F).margin(2.0e-3F));
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
    Physics::ProviderHandleV2 handle{};
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) == Physics::Status::ok);
        Physics::ProviderHandleV2 duplicate_handle{};
        REQUIRE(api.register_provider(api.context, &provider, &duplicate_handle) ==
                Physics::Status::duplicate_provider);
    }

    REQUIRE(physics_internal::activeProviderName() == configuredProviderName());
    physics_internal::activateProviderOwner(owner);
    REQUIRE(physics_internal::activeProviderName() == "test.fake");

    const auto active_service = physicsService(api);
    REQUIRE((active_service.capability_bits & Physics::builtinQueryCapabilitiesV2) ==
            Physics::builtinQueryCapabilitiesV2);

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

    const auto fallback_shape_cast = world.shapeCastClosest(
        phys::Sphere{{0.0F, 0.0F, 0.0F}, 0.5F},
        {10.0F, 0.0F, 0.0F});
#if PELICAN_WITH_JOLT_PHYSICS || PELICAN_WITH_BUILTIN_PHYSICS
    REQUIRE(fallback_shape_cast);
    REQUIRE(fallback_shape_cast->id == "first");
#else
    REQUIRE_FALSE(fallback_shape_cast);
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


TEST_CASE("PhysicsServiceV2 routes shape casts and contains malformed V2 results",
          "[physics-service][shape-cast]") {
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
    REQUIRE((service.capability_bits & Physics::builtinQueryCapabilitiesV2) ==
            Physics::builtinQueryCapabilitiesV2);
    REQUIRE(service.shape_cast_all != nullptr);

    FakeShapeProviderState state;
    auto provider = fakeShapeProvider(state);
    const auto owner = internal::allocateRegistrationOwner();
    Physics::ProviderHandleV2 handle{};
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) ==
                Physics::Status::ok);
    }
    physics_internal::activateProviderOwner(owner);
    REQUIRE(physics_internal::activeProviderName() == "test.shape-v2");

    const auto routed = world.shapeCastAll(
        phys::Sphere{{0.0F, 0.0F, 0.0F}, 0.5F},
        {10.0F, 0.0F, 0.0F});
    REQUIRE(state.last_collider_count == 2);
    REQUIRE(state.last_delta.x == Catch::Approx(10.0F));
    REQUIRE(routed.size() == 2);
    REQUIRE(routed[0].id == "first");
    REQUIRE(routed[1].id == "second");
    REQUIRE(routed[0].time_of_impact == Catch::Approx(0.25F));
    REQUIRE(routed[0].normal.x == Catch::Approx(-1.0F));

    auto query = Physics::descriptor<Physics::ShapeCastQueryV2>();
    query.shape.kind = Physics::ShapeKindV1::sphere;
    query.shape.radius = 0.5F;
    query.delta = {10.0F, 0.0F, 0.0F};
    std::array<Physics::ShapeCastHitV2, 2> hits{};
    std::uint32_t hit_count = 0;
    REQUIRE(service.shape_cast_all(
                service.context, &query, hits.data(),
                static_cast<std::uint32_t>(hits.size()), &hit_count) ==
            Physics::Status::ok);
    REQUIRE(hit_count == 2);
    REQUIRE(hits[0].identity.collider_id != 0);

    constexpr std::array malformed_behaviors{
        FakeShapeProviderState::Behavior::duplicate_index,
        FakeShapeProviderState::Behavior::out_of_range_index,
        FakeShapeProviderState::Behavior::zero_normal,
        FakeShapeProviderState::Behavior::invalid_toi,
        FakeShapeProviderState::Behavior::initial_without_depth,
        FakeShapeProviderState::Behavior::initial_at_contact_depth,
        FakeShapeProviderState::Behavior::sweep_with_depth,
        FakeShapeProviderState::Behavior::unknown_flags,
        FakeShapeProviderState::Behavior::non_finite_position,
        FakeShapeProviderState::Behavior::unknown_status,
        FakeShapeProviderState::Behavior::oversized_count,
        FakeShapeProviderState::Behavior::unexpected_buffer_too_small,
    };
    for (const auto behavior : malformed_behaviors) {
        state.behavior = behavior;
        REQUIRE(service.shape_cast_all(
                    service.context, &query, hits.data(),
                    static_cast<std::uint32_t>(hits.size()), &hit_count) ==
                Physics::Status::provider_error);
    }

    physics_internal::releaseProviderOwner(owner);
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
    Physics::ProviderHandleV2 handle{};
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

TEST_CASE("PhysicsServiceV2 contains malformed provider results",
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
    Physics::ProviderHandleV2 handle{};
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

TEST_CASE("PhysicsServiceV2 preserves version checks and rejects malformed providers",
          "[physics-service]") {
    auto api = Physics::descriptor<Physics::ApiV2>();
    api.reserved0 = 1;
    REQUIRE(Physics::getApiV2(Physics::abiVersionV2, &api) ==
            Physics::Status::reserved_not_zero);

    api = Physics::descriptor<Physics::ApiV2>();
    REQUIRE(Physics::getApiV2(Physics::abiVersionV2 + 1, &api) ==
            Physics::Status::unsupported_version);

    api = physicsApi();
    auto service = Physics::descriptor<Physics::ServiceV2>();
    REQUIRE(api.get_service(api.context, Physics::serviceVersionV2 + 1, &service) ==
            Physics::Status::unsupported_version);

    FakeProviderState state;
    auto provider = fakeProvider(state);
    Physics::ProviderHandleV2 handle{};
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

TEST_CASE("PhysicsServiceV2 rejects malformed descriptors and unknown bits",
          "[physics-service]") {
    auto api = Physics::descriptor<Physics::ApiV2>();
    api.reserved1 = 1;
    REQUIRE(Physics::getApiV2(Physics::abiVersionV2, &api) ==
            Physics::Status::reserved_not_zero);

    api = Physics::descriptor<Physics::ApiV2>();
    REQUIRE(Physics::getApiV2(Physics::abiVersionV2 + 1, &api) ==
            Physics::Status::unsupported_version);

    api = physicsApi();
    const auto service = physicsService(api);
    auto query = Physics::descriptor<Physics::ShapeCastQueryV2>();
    query.filter.flags |= 1U << 31U;
    std::uint32_t hit_count = 0;
    REQUIRE(service.shape_cast_all(service.context, &query, nullptr, 0,
                                   &hit_count) ==
            Physics::Status::invalid_argument);

    FakeShapeProviderState state;
    auto provider = fakeShapeProvider(state);
    provider.capability_bits |= 1ULL << 63U;
    Physics::ProviderHandleV2 handle{};
    const auto owner = internal::allocateRegistrationOwner();
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) ==
                Physics::Status::invalid_argument);
    }

    provider = fakeShapeProvider(state);
    provider.struct_size = offsetof(Physics::ProviderV2, shape_cast_all);
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &provider, &handle) ==
                Physics::Status::invalid_argument);
    }
}

} // namespace Pelican
