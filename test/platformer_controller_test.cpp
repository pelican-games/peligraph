#include <details/event/registerer.hpp>
#include <gamesystem.hpp>
#include <platformer/charactercontroller2d.hpp>

#include "../src/core/phys/physquery.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using Pelican::GameContext;
using Pelican::vec3;
using namespace Pelican::phys;
using namespace Pelican::platformer;

struct PlatformerLandedFixtureEvent {
    std::uint64_t step = 0;
    std::uint64_t collider = 0;

    template <class T> void ref(T &archive) {
        archive.prop("step", step);
        archive.prop("collider", collider);
    }
};

std::vector<std::pair<std::uint64_t, std::uint64_t>> delivered_landings;

struct PlatformerLandedProbeSystem {
    void onEvent(const PlatformerLandedFixtureEvent &event, GameContext &) {
        delivered_landings.emplace_back(event.step, event.collider);
    }
};

Collider collider(std::string name, Shape shape, std::uint64_t id,
                  ColliderQueryMetadata metadata = {}) {
    const auto identity_name = name;
    return Collider{
        std::move(name),
        std::move(shape),
        ColliderIdentity{
            .collider_id = ColliderId{id},
            .name = identity_name,
        },
        metadata,
    };
}

ShapeCastAll2DQuery queryFor(const std::vector<Collider> &colliders) {
    return [&colliders](const Shape &shape, vec3 delta, const QueryFilter &filter) {
        return shapeCastAll(shape, delta, colliders, filter);
    };
}

vec3 centerOf(const Shape &shape) {
    return std::visit([](const auto &value) { return value.center; }, shape);
}

Capsule playerAt(float x, float y) {
    return Capsule{
        .center = {x, y, 0.0F},
        .rotation = {0.0F, 0.0F, 0.0F, 1.0F},
        .half_height = 0.4F,
        .radius = 0.2F,
    };
}

const CharacterContact2D *groundContact(const MoveAndSlide2DResult &result) {
    for (const auto &contact : result.contacts) {
        if (contact.kind == ContactKind2D::ground) return &contact;
    }
    return nullptr;
}

void requireNear(float actual, float expected, float margin = 2.0e-3F) {
    REQUIRE(actual == Catch::Approx(expected).margin(margin));
}

struct ReplayResult {
    std::vector<std::uint32_t> bytes;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> landings;
    bool touched_one_way = false;
    bool touched_wall = false;
};

void appendFloatBits(std::vector<std::uint32_t> &trace, float value) {
    trace.push_back(std::bit_cast<std::uint32_t>(value));
}

ReplayResult runReplay() {
    const std::vector<Collider> world{
        collider("floor", Box{{0.0F, -0.1F, 0.0F}, {}, {8.0F, 0.1F, 1.0F}}, 10),
        collider("one-way", Box{{0.0F, 1.8F, 0.0F}, {}, {1.2F, 0.05F, 1.0F}}, 20,
                 ColliderQueryMetadata{.one_way = true}),
        collider("wall", Box{{4.0F, 2.0F, 0.0F}, {}, {0.1F, 2.0F, 1.0F}}, 30),
    };
    const auto query = queryFor(world);
    MoveAndSlide2DSettings settings;
    settings.skin_width = 1.0e-4F;

    Shape body = playerAt(-3.0F, 0.6001F);
    vec3 velocity{};
    bool was_grounded = false;
    bool grounded = false;
    constexpr float fixed_dt = 1.0F / 60.0F;

    Pelican::internal::clearPendingEvents();
    delivered_landings.clear();
    GameContext context;
    ReplayResult replay;
    replay.bytes.reserve(240 * 7);

    for (std::uint64_t step = 0; step < 240; ++step) {
        Pelican::internal::freezePendingEventsForFrame();
        Pelican::internal::dispatchFrozenEvents(context);

        const float horizontal = step < 210 ? 1.0F : 0.0F;
        const bool jump_pressed = step == 30;
        velocity.x = horizontal * 3.0F;
        if (jump_pressed && grounded) velocity.y = 9.0F;
        velocity.y -= 18.0F * fixed_dt;

        const auto motion = moveAndSlide(body, {velocity.x * fixed_dt,
                                                velocity.y * fixed_dt, 0.0F},
                                         query, {}, settings);
        body = motion.shape;
        grounded = motion.grounded;
        if (motion.grounded && velocity.y < 0.0F) velocity.y = 0.0F;
        if (motion.hit_ceiling && velocity.y > 0.0F) velocity.y = 0.0F;
        if (motion.hit_wall) replay.touched_wall = true;

        const auto *ground = groundContact(motion);
        if (ground != nullptr && ground->hit.identity.collider_id == ColliderId{20}) {
            replay.touched_one_way = true;
        }
        if (grounded && !was_grounded && ground != nullptr) {
            context.emit(PlatformerLandedFixtureEvent{
                .step = step,
                .collider = ground->hit.identity.collider_id.value,
            });
        }
        was_grounded = grounded;

        const auto center = centerOf(body);
        appendFloatBits(replay.bytes, center.x);
        appendFloatBits(replay.bytes, center.y);
        appendFloatBits(replay.bytes, velocity.x);
        appendFloatBits(replay.bytes, velocity.y);
        replay.bytes.push_back(grounded ? 1U : 0U);
        replay.bytes.push_back(motion.hit_wall ? 1U : 0U);
        replay.bytes.push_back(ground != nullptr
                                   ? static_cast<std::uint32_t>(ground->hit.identity.collider_id.value)
                                   : 0U);
    }

    Pelican::internal::freezePendingEventsForFrame();
    Pelican::internal::dispatchFrozenEvents(context);
    replay.landings = delivered_landings;
    Pelican::internal::clearPendingEvents();
    return replay;
}

} // namespace

PELICAN_REGISTER_EVENT(PlatformerLandedFixtureEvent);
PELICAN_REGISTER_SYSTEM(PlatformerLandedProbeSystem, 50);

TEST_CASE("moveAndSlide catches a high-speed capsule on a thin floor",
          "[platformer][tunneling]") {
    const std::vector<Collider> world{
        collider("thin-floor", Box{{0.0F, -0.025F, 0.0F}, {}, {4.0F, 0.025F, 1.0F}}, 1),
    };

    const auto result = moveAndSlide(Shape{playerAt(0.0F, 3.0F)}, {0.0F, -20.0F, 0.0F},
                                     queryFor(world));
    REQUIRE(result.grounded);
    REQUIRE_FALSE(result.iteration_limit_reached);
    REQUIRE(result.contacts.size() == 1);
    REQUIRE(result.contacts.front().kind == ContactKind2D::ground);
    REQUIRE(result.contacts.front().hit.identity.collider_id == ColliderId{1});
    requireNear(centerOf(result.shape).y, 0.6001F);
}

TEST_CASE("moveAndSlide preserves downward motion while sliding along a wall",
          "[platformer][wall]") {
    const std::vector<Collider> world{
        collider("wall", Box{{2.0F, 2.0F, 0.0F}, {}, {0.1F, 3.0F, 1.0F}}, 2),
    };

    const auto approach = moveAndSlide(Shape{playerAt(0.0F, 3.0F)},
                                       {5.0F, 0.0F, 0.0F}, queryFor(world));
    REQUIRE(approach.hit_wall);
    const auto result = moveAndSlide(approach.shape, {0.0F, -2.0F, 0.0F}, queryFor(world));
    REQUIRE_FALSE(result.grounded);
    const auto center = centerOf(result.shape);
    requireNear(center.x, 1.6999F, 3.0e-3F);
    requireNear(center.y, 1.0F, 3.0e-3F);
}

TEST_CASE("walkable slopes climb while steep slopes are treated as walls",
          "[platformer][slope]") {
    std::uint32_t calls = 0;
    const ShapeCastAll2DQuery walkable_query = [&calls](const Shape &, vec3,
                                                        const QueryFilter &) {
        if (calls++ != 0) return std::vector<ShapeCastQueryHit>{};
        return std::vector<ShapeCastQueryHit>{ShapeCastQueryHit{
            .id = "walkable",
            .time_of_impact = 0.25F,
            .position = {0.5F, 0.0F, 0.0F},
            .normal = {-0.5F, 0.8660254F, 0.0F},
            .identity = ColliderIdentity{.collider_id = ColliderId{3}, .name = "walkable"},
        }};
    };
    auto walkable = moveAndSlide(Shape{playerAt(0.0F, 1.0F)}, {2.0F, -0.25F, 0.0F},
                                 walkable_query);
    REQUIRE(walkable.grounded);
    REQUIRE(centerOf(walkable.shape).y > 1.0F);

    calls = 0;
    const ShapeCastAll2DQuery steep_query = [&calls](const Shape &, vec3,
                                                     const QueryFilter &) {
        if (calls++ != 0) return std::vector<ShapeCastQueryHit>{};
        return std::vector<ShapeCastQueryHit>{ShapeCastQueryHit{
            .id = "steep",
            .time_of_impact = 0.25F,
            .position = {0.5F, 0.0F, 0.0F},
            .normal = {-0.8660254F, 0.5F, 0.0F},
            .identity = ColliderIdentity{.collider_id = ColliderId{4}, .name = "steep"},
        }};
    };
    auto steep = moveAndSlide(Shape{playerAt(0.0F, 1.0F)}, {2.0F, -0.25F, 0.0F},
                              steep_query);
    REQUIRE(steep.hit_wall);
    REQUIRE_FALSE(steep.grounded);
    REQUIRE(centerOf(steep.shape).y <= 1.0F);
}

TEST_CASE("one-way floors pass from below, catch from above, and can be disabled",
          "[platformer][one-way]") {
    const std::vector<Collider> world{
        collider("one-way", Box{{0.0F, 0.0F, 0.0F}, {}, {4.0F, 0.05F, 1.0F}}, 5,
                 ColliderQueryMetadata{.one_way = true}),
    };
    const auto query = queryFor(world);

    const auto upward = moveAndSlide(Shape{playerAt(0.0F, -2.0F)}, {0.0F, 4.0F, 0.0F}, query);
    REQUIRE_FALSE(upward.grounded);
    requireNear(centerOf(upward.shape).y, 2.0F);

    const auto downward = moveAndSlide(Shape{playerAt(0.0F, 2.0F)}, {0.0F, -4.0F, 0.0F}, query);
    REQUIRE(downward.grounded);
    REQUIRE(groundContact(downward)->hit.identity.collider_id == ColliderId{5});
    requireNear(centerOf(downward.shape).y, 0.6501F);

    MoveAndSlide2DSettings drop_through;
    drop_through.collide_with_one_way = false;
    const auto dropped = moveAndSlide(Shape{playerAt(0.0F, 2.0F)}, {0.0F, -4.0F, 0.0F},
                                      query, {}, drop_through);
    REQUIRE_FALSE(dropped.grounded);
    requireNear(centerOf(dropped.shape).y, -2.0F);
}

TEST_CASE("initial overlap recovery is deterministic and reports ground",
          "[platformer][overlap]") {
    const std::vector<Collider> world{
        collider("floor", Box{{0.0F, -0.1F, 0.0F}, {}, {4.0F, 0.1F, 1.0F}}, 6),
    };

    const auto result = moveAndSlide(Shape{playerAt(0.0F, 0.5F)}, {}, queryFor(world));
    REQUIRE(result.grounded);
    REQUIRE(result.contacts.front().hit.initial_overlap);
    REQUIRE(result.applied_translation.y > 0.09F);
    requireNear(centerOf(result.shape).y, 0.6001F, 4.0e-3F);
}

TEST_CASE("fixed-step side-scroller replay and landing events are byte exact",
          "[platformer][replay][event]") {
    const auto first = runReplay();
    const auto second = runReplay();

    REQUIRE(first.bytes == second.bytes);
    REQUIRE(first.landings == second.landings);
    REQUIRE(first.touched_one_way);
    REQUIRE(first.touched_wall);
    REQUIRE(first.landings.size() >= 2);
    REQUIRE(first.landings.front().second == 10);
    REQUIRE(std::any_of(first.landings.begin(), first.landings.end(), [](const auto &landing) {
        return landing.second == 20;
    }));
}

TEST_CASE("moveAndSlide validates the side-scroller plane and callback contract",
          "[platformer][contract]") {
    const ShapeCastAll2DQuery empty_query = [](const Shape &, vec3, const QueryFilter &) {
        return std::vector<ShapeCastQueryHit>{};
    };
    REQUIRE_THROWS(moveAndSlide(Shape{playerAt(0.0F, 1.0F)}, {0.0F, 0.0F, 1.0F},
                                empty_query));
    REQUIRE_THROWS(moveAndSlide(Shape{playerAt(0.0F, 1.0F)}, {}, ShapeCastAll2DQuery{}));

    MoveAndSlide2DSettings invalid;
    invalid.max_slope_degrees = 90.0F;
    REQUIRE_THROWS(moveAndSlide(Shape{playerAt(0.0F, 1.0F)}, {}, empty_query, {}, invalid));
}
