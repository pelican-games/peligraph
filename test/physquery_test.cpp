#include "../src/core/phys/physquery.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Pelican::phys {
namespace {

constexpr float kPi = 3.14159265358979323846f;

quat rotationZ(float radians) {
    return {0.0f, 0.0f, std::sin(radians * 0.5f), std::cos(radians * 0.5f)};
}

void requireVec(vec3 actual, vec3 expected, float epsilon = 1.0e-4f) {
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(epsilon));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(epsilon));
    REQUIRE(actual.z == Catch::Approx(expected.z).margin(epsilon));
}

Shape recentered(Shape shape, vec3 center) {
    std::visit([&](auto &typed) { typed.center = center; }, shape);
    return shape;
}

Shape translatedForTest(Shape shape, vec3 offset) {
    std::visit([&](auto &typed) {
        typed.center.x += offset.x;
        typed.center.y += offset.y;
        typed.center.z += offset.z;
    }, shape);
    return shape;
}

struct CorpusRng {
    std::uint32_t state = 0x7f4a7c15U;

    std::uint32_t next() {
        state = state * 1664525U + 1013904223U;
        return state;
    }

    float signedUnit() {
        return static_cast<float>(next() & 0xffffU) / 32767.5f - 1.0f;
    }
};

float corpusExtent(CorpusRng &rng) {
    constexpr std::array values{0.0f, 1.0e-7f, 1.0e-5f, 1.0e-3f,
                                0.05f, 0.25f, 0.75f, 2.0f};
    return values[rng.next() % values.size()];
}

Shape corpusShape(CorpusRng &rng, vec3 center) {
    const quat rotation{rng.signedUnit(), rng.signedUnit(), rng.signedUnit(),
                        rng.signedUnit()};
    switch (rng.next() % 3U) {
    case 0:
        return Sphere{center, corpusExtent(rng)};
    case 1:
        return Box{center, rotation,
                   {corpusExtent(rng), corpusExtent(rng), corpusExtent(rng)}};
    default:
        return Capsule{center, rotation, corpusExtent(rng), corpusExtent(rng)};
    }
}

void requireValidShapeCastHit(const std::optional<ShapeCastHit> &hit) {
    if (!hit) return;
    REQUIRE(std::isfinite(hit->time_of_impact));
    REQUIRE(std::isfinite(hit->penetration_depth));
    REQUIRE(std::isfinite(hit->position.x));
    REQUIRE(std::isfinite(hit->position.y));
    REQUIRE(std::isfinite(hit->position.z));
    REQUIRE(std::isfinite(hit->normal.x));
    REQUIRE(std::isfinite(hit->normal.y));
    REQUIRE(std::isfinite(hit->normal.z));
    REQUIRE(hit->time_of_impact >= 0.0f);
    REQUIRE(hit->time_of_impact <= 1.0f);
    REQUIRE(hit->penetration_depth >= 0.0f);
    const float normal_length_squared =
        hit->normal.x * hit->normal.x + hit->normal.y * hit->normal.y +
        hit->normal.z * hit->normal.z;
    REQUIRE(normal_length_squared == Catch::Approx(1.0f).margin(2.0e-3f));
}

Collider identifiedCollider(std::string name, Shape shape, std::uint64_t collider_id,
                            ColliderQueryMetadata metadata = {},
                            std::optional<GameObjectId> entity = std::nullopt,
                            std::uint32_t shape_ordinal = 0) {
    const std::string identity_name = name;
    return Collider{
        std::move(name),
        std::move(shape),
        ColliderIdentity{
            .collider_id = ColliderId{collider_id},
            .name = identity_name,
            .entity = entity,
            .shape_ordinal = shape_ordinal,
        },
        metadata,
    };
}

} // namespace

TEST_CASE("raycast hits sphere analytically including tangent and inside rays", "[physquery]") {
    const Sphere sphere{{0.0f, 0.0f, 0.0f}, 1.0f};

    const auto hit = raycast(Ray{{-3.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, sphere);
    REQUIRE(hit);
    REQUIRE(hit->distance == Catch::Approx(2.0f));
    requireVec(hit->position, {-1.0f, 0.0f, 0.0f});
    requireVec(hit->normal, {-1.0f, 0.0f, 0.0f});

    const auto tangent = raycast(Ray{{-1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, sphere);
    REQUIRE(tangent);
    REQUIRE(tangent->distance == Catch::Approx(1.0f));
    requireVec(tangent->position, {0.0f, 1.0f, 0.0f});
    requireVec(tangent->normal, {0.0f, 1.0f, 0.0f});

    const auto inside = raycast(Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, sphere);
    REQUIRE(inside);
    REQUIRE(inside->distance == Catch::Approx(1.0f));
    requireVec(inside->position, {1.0f, 0.0f, 0.0f});
    requireVec(inside->normal, {1.0f, 0.0f, 0.0f});
}

TEST_CASE("raycast keeps small sphere and capsule misses stable at long range",
          "[physquery][small-shape][precision]") {
    const Sphere sphere{{0.0f, 0.0f, 0.0f}, 1.0e-3f};
    const Ray near_miss{{-100000.0f, 2.0e-3f, 0.0f},
                        {1.0f, 0.0f, 0.0f}, 200000.0f};
    REQUIRE_FALSE(raycast(near_miss, sphere));
    REQUIRE(raycast(Ray{{-100000.0f, 5.0e-4f, 0.0f},
                        {1.0f, 0.0f, 0.0f}, 200000.0f},
                    sphere));

    const Capsule capsule{{0.0f, 0.0f, 0.0f},
                          {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 1.0e-3f};
    REQUIRE_FALSE(raycast(Ray{{-100000.0f, 0.0f, 2.0e-3f},
                              {1.0f, 0.0f, 0.0f}, 200000.0f},
                          capsule));
    REQUIRE(raycast(Ray{{-100000.0f, 0.0f, 5.0e-4f},
                        {1.0f, 0.0f, 0.0f}, 200000.0f},
                    capsule));
}

TEST_CASE("raycast hits rotated OBBs and rejects misses", "[physquery]") {
    const Box box{{0.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.5f), {1.0f, 2.0f, 1.0f}};

    const auto hit = raycast(Ray{{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, box);
    REQUIRE(hit);
    REQUIRE(hit->distance == Catch::Approx(3.0f).margin(1.0e-4f));
    requireVec(hit->position, {-2.0f, 0.0f, 0.0f});
    requireVec(hit->normal, {-1.0f, 0.0f, 0.0f});

    const auto inside = raycast(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, box);
    REQUIRE(inside);
    REQUIRE(inside->distance == Catch::Approx(1.0f).margin(1.0e-4f));
    requireVec(inside->position, {0.0f, 1.0f, 0.0f});
    requireVec(inside->normal, {0.0f, 1.0f, 0.0f});

    REQUIRE_FALSE(raycast(Ray{{-5.0f, 3.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, box));
}

TEST_CASE("raycast hits capsule sides caps and respects max distance", "[physquery]") {
    const Capsule capsule{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 0.5f};

    const auto side = raycast(Ray{{-2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, capsule);
    REQUIRE(side);
    REQUIRE(side->distance == Catch::Approx(1.5f));
    requireVec(side->position, {-0.5f, 0.0f, 0.0f});
    requireVec(side->normal, {-1.0f, 0.0f, 0.0f});

    const auto inside_to_cap = raycast(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, capsule);
    REQUIRE(inside_to_cap);
    REQUIRE(inside_to_cap->distance == Catch::Approx(1.5f));
    requireVec(inside_to_cap->position, {0.0f, 1.5f, 0.0f});
    requireVec(inside_to_cap->normal, {0.0f, 1.0f, 0.0f});

    REQUIRE_FALSE(raycast(Ray{{-2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 1.0f}, capsule));
}

TEST_CASE("raycast rejects degenerate finite query inputs", "[physquery]") {
    const Sphere sphere{{0.0f, 0.0f, 0.0f}, 1.0f};
    REQUIRE_FALSE(raycast(Ray{{-3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}, sphere));
    REQUIRE_FALSE(raycast(Ray{{-3.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, -1.0f}, sphere));
}

TEST_CASE("overlap tests include touching boundaries for all shape pairs", "[physquery]") {
    const Sphere unit_sphere{{0.0f, 0.0f, 0.0f}, 1.0f};
    REQUIRE(overlaps(unit_sphere, Sphere{{2.0f, 0.0f, 0.0f}, 1.0f}));
    REQUIRE_FALSE(overlaps(unit_sphere, Sphere{{2.01f, 0.0f, 0.0f}, 1.0f}));

    const Box unit_box{{0.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.5f), {1.0f, 1.0f, 1.0f}};
    REQUIRE(overlaps(Sphere{{2.0f, 0.0f, 0.0f}, 1.0f}, unit_box));
    REQUIRE_FALSE(overlaps(Sphere{{2.01f, 0.0f, 0.0f}, 1.0f}, unit_box));

    const Capsule vertical_capsule{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 0.5f};
    REQUIRE(overlaps(Sphere{{1.5f, 0.0f, 0.0f}, 1.0f}, vertical_capsule));
    REQUIRE_FALSE(overlaps(Sphere{{1.51f, 0.0f, 0.0f}, 1.0f}, vertical_capsule));

    REQUIRE(overlaps(unit_box, Box{{2.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.25f), {1.0f, 1.0f, 1.0f}}));
    REQUIRE_FALSE(overlaps(unit_box, Box{{3.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.25f), {0.5f, 0.5f, 0.5f}}));

    REQUIRE(overlaps(vertical_capsule, Capsule{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 0.5f}));
    REQUIRE_FALSE(overlaps(vertical_capsule, Capsule{{1.01f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 0.5f}));

    REQUIRE(overlaps(unit_box, Capsule{{2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 1.0f}));
    REQUIRE_FALSE(overlaps(unit_box, Capsule{{2.01f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f, 1.0f}));
}

TEST_CASE("raycastClosest resolves equal-distance ties by lexicographic id", "[physquery]") {
    const std::vector<Collider> colliders{
        {"z", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}},
        {"a", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}},
    };

    const auto hit = raycastClosest(Ray{{-3.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, colliders);
    REQUIRE(hit);
    REQUIRE(hit->id == "a");
    REQUIRE(hit->distance == Catch::Approx(2.0f));
    requireVec(hit->normal, {-1.0f, 0.0f, 0.0f});
}

TEST_CASE("raycastAll carries stable identity and derives closest from deterministic order",
          "[physquery]") {
    const EntityId entity{7, 3};
    const std::vector<Collider> colliders{
        identifiedCollider("z", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 10,
                           ColliderQueryMetadata{}, entity),
        identifiedCollider("z-subshape", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 10,
                           ColliderQueryMetadata{}, entity, 1),
        identifiedCollider("a", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 20),
    };

    const Ray ray{{-3.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    const auto hits = raycastAll(ray, colliders);
    REQUIRE(hits.size() == 3);
    REQUIRE(hits[0].id == "z");
    REQUIRE(hits[0].identity.collider_id == ColliderId{10});
    REQUIRE(hits[0].identity.entity == entity);
    REQUIRE(hits[1].id == "z-subshape");
    REQUIRE(hits[1].identity.collider_id == ColliderId{10});
    REQUIRE(hits[1].identity.shape_ordinal == 1);
    REQUIRE(hits[2].id == "a");
    REQUIRE(hits[2].identity.collider_id == ColliderId{20});

    const auto closest = raycastClosest(ray, colliders, QueryFilter{});
    REQUIRE(closest);
    REQUIRE(closest->identity == hits.front().identity);
    const auto legacy_closest = raycastClosest(ray, colliders);
    REQUIRE(legacy_closest);
    REQUIRE(legacy_closest->id == "a");
}

TEST_CASE("query filter applies reciprocal layers metadata and stable ignore ids",
          "[physquery]") {
    const ColliderQueryMetadata solid_metadata{
        .layer = 0b0010U,
        .mask = 0b0100U,
    };
    const ColliderQueryMetadata trigger_metadata{
        .layer = 0b0010U,
        .mask = 0b0100U,
        .trigger = true,
    };
    const ColliderQueryMetadata one_way_metadata{
        .layer = 0b0010U,
        .mask = 0b0100U,
        .one_way = true,
    };
    const ColliderQueryMetadata other_layer_metadata{
        .layer = 0b1000U,
        .mask = 0b0100U,
    };
    const std::vector<Collider> colliders{
        identifiedCollider("solid", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 1, solid_metadata),
        identifiedCollider("trigger", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 2, trigger_metadata),
        identifiedCollider("one-way", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 3, one_way_metadata),
        identifiedCollider("other", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 4, other_layer_metadata),
    };
    const QueryFilter filter{
        .layer = 0b0100U,
        .mask = 0b0010U,
        .include_triggers = false,
        .include_one_way = false,
    };

    const Ray ray{{-3.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    const auto hits = raycastAll(ray, colliders, filter);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().id == "solid");
    REQUIRE(hits.front().metadata == solid_metadata);
    const auto overlap_hits = overlapAllHits(
        Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, colliders, filter);
    REQUIRE(overlap_hits.size() == 1);
    REQUIRE(overlap_hits.front().id == "solid");

    const std::array ignored{ColliderId{1}};
    auto ignored_filter = filter;
    ignored_filter.ignored = ignored;
    REQUIRE(raycastAll(ray, colliders, ignored_filter).empty());

    auto self_filter = filter;
    self_filter.self = ColliderId{1};
    REQUIRE(raycastAll(ray, colliders, self_filter).empty());

    const EntityId entity{4, 9};
    const std::vector<Collider> entity_collider{
        identifiedCollider("entity", Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, 5,
                           solid_metadata, entity),
    };
    auto entity_filter = filter;
    entity_filter.self_entity = entity;
    REQUIRE(raycastAll(ray, entity_collider, entity_filter).empty());

    const std::array ignored_entities{entity};
    entity_filter.self_entity.reset();
    entity_filter.ignored_entities = ignored_entities;
    REQUIRE(raycastAll(ray, entity_collider, entity_filter).empty());
}

TEST_CASE("overlapAll returns deterministic sorted ids", "[physquery]") {
    const std::vector<Collider> colliders{
        {"z", Sphere{{0.0f, 0.0f, 0.0f}, 0.25f}},
        {"miss", Sphere{{10.0f, 0.0f, 0.0f}, 0.25f}},
        {"a", Box{{0.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.25f), {0.25f, 0.25f, 0.25f}}},
    };

    const std::vector<std::string> ids = overlapAll(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, colliders);
    REQUIRE(ids == std::vector<std::string>{"a", "z"});

    const auto hits = overlapAllHits(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, colliders);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].identity.collider_id.valid());
    REQUIRE(hits[1].identity.collider_id.valid());
    REQUIRE(colliderIdentityLess(hits[0].identity, hits[1].identity));
    std::vector<std::string> detailed_ids{hits[0].id, hits[1].id};
    std::sort(detailed_ids.begin(), detailed_ids.end());
    REQUIRE(detailed_ids == std::vector<std::string>{"a", "z"});
}

TEST_CASE("shapeCast prevents thin-collider tunneling for every standard shape pair",
          "[physquery][shape-cast]") {
    const std::array<Shape, 3> moving_shapes{
        Sphere{{0.0f, 0.0f, 0.0f}, 0.5f},
        Box{{0.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.125f),
            {0.5f, 0.4f, 0.3f}},
        Capsule{{0.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.125f), 0.6f, 0.3f},
    };
    const std::array<Shape, 3> collider_shapes{
        Sphere{{5.0f, 0.0f, 0.0f}, 0.5f},
        Box{{5.0f, 0.0f, 0.0f}, rotationZ(-kPi * 0.1f),
            {0.05f, 1.0f, 1.0f}},
        Capsule{{5.0f, 0.0f, 0.0f}, rotationZ(-kPi * 0.15f), 0.8f, 0.35f},
    };

    for (const auto &moving : moving_shapes) {
        for (const auto &collider : collider_shapes) {
            const auto hit = shapeCast(moving, {10.0f, 0.0f, 0.0f}, collider);
            INFO("moving=" << moving.index() << " collider=" << collider.index());
            REQUIRE(hit);
            REQUIRE_FALSE(hit->initial_overlap);
            REQUIRE(hit->penetration_depth == Catch::Approx(0.0f));
            REQUIRE(hit->time_of_impact >= 0.0f);
            REQUIRE(hit->time_of_impact <= 1.0f);
            REQUIRE(hit->normal.x < -0.5f);
        }
    }

    const auto exact = shapeCast(
        Shape{Sphere{{0.0f, 0.0f, 0.0f}, 0.5f}}, {10.0f, 0.0f, 0.0f},
        Shape{Sphere{{5.0f, 0.0f, 0.0f}, 0.5f}});
    REQUIRE(exact);
    REQUIRE(exact->time_of_impact == Catch::Approx(0.4f).margin(2.0e-4f));
    requireVec(exact->normal, {-1.0f, 0.0f, 0.0f});
    requireVec(exact->position, {4.5f, 0.0f, 0.0f}, 2.0e-4f);
}

TEST_CASE("shapeCast preserves exact box face normals at contact",
          "[physquery][shape-cast][normal]") {
    for (const float angle : std::array{0.0f, 0.01f}) {
        CAPTURE(angle);
        const vec3 face_normal{-std::sin(angle), std::cos(angle), 0.0f};
        const Capsule moving{
            {face_normal.x * 3.0f, face_normal.y * 3.0f, 0.0f},
            {},
            0.4f,
            0.2f,
        };
        const Box floor{{}, rotationZ(angle), {4.0f, 0.025f, 1.0f}};
        const vec3 delta{-face_normal.x * 20.0f,
                         -face_normal.y * 20.0f, 0.0f};

        const auto hit = shapeCast(Shape{moving}, delta, Shape{floor});
        REQUIRE(hit);
        REQUIRE_FALSE(hit->initial_overlap);
        requireVec(hit->normal, face_normal, 2.0e-5f);
    }
}

TEST_CASE("shapeCast keeps GJK bounded across degenerate convex dimensions",
          "[physquery][shape-cast][degenerate][gjk]") {
    const quat tilted{0.31f, -0.17f, 0.23f, 0.89f};
    const std::array<Shape, 13> structures{
        Sphere{{}, 0.0f},
        Sphere{{}, 1.0e-6f},
        Sphere{{}, 0.5f},
        Box{{}, tilted, {0.0f, 0.0f, 0.0f}},
        Box{{}, tilted, {1.0f, 0.0f, 0.0f}},
        Box{{}, tilted, {1.0f, 0.75f, 0.0f}},
        Box{{}, tilted, {1.0f, 0.75f, 1.0e-6f}},
        Box{{}, tilted, {1.0f, 0.75f, 0.25f}},
        Capsule{{}, tilted, 0.0f, 0.0f},
        Capsule{{}, tilted, 1.0f, 0.0f},
        Capsule{{}, tilted, 0.0f, 0.5f},
        Capsule{{}, tilted, 1.0f, 1.0e-6f},
        Capsule{{}, tilted, 1.0f, 0.25f},
    };
    struct CastLayout {
        vec3 moving_center;
        vec3 collider_center;
        vec3 delta;
    };
    const std::array layouts{
        CastLayout{{-3.0f, 0.125f, 2.0e-6f}, {},
                   {6.0f, -0.25f, -4.0e-6f}},
        CastLayout{{0.0f, 0.0f, 4.0e-5f}, {}, {0.0f, 0.0f, -8.0e-5f}},
        CastLayout{{-2.0f, -1.5f, 0.75f}, {0.25f, 0.5f, -0.25f},
                   {4.5f, 4.0f, -2.0f}},
        CastLayout{{}, {}, {}},
    };

    for (std::size_t moving_index = 0; moving_index < structures.size();
         ++moving_index) {
        for (std::size_t collider_index = 0;
             collider_index < structures.size(); ++collider_index) {
            for (std::size_t layout_index = 0; layout_index < layouts.size();
                 ++layout_index) {
                CAPTURE(moving_index, collider_index, layout_index);
                const auto &layout = layouts[layout_index];
                const auto moving = recentered(
                    structures[moving_index], layout.moving_center);
                const auto collider = recentered(
                    structures[collider_index], layout.collider_center);
                const auto forward = shapeCast(moving, layout.delta, collider);
                const auto reverse = shapeCast(
                    collider,
                    {-layout.delta.x, -layout.delta.y, -layout.delta.z}, moving);
                requireValidShapeCastHit(forward);
                requireValidShapeCastHit(reverse);
                CAPTURE(overlaps(moving, collider), overlaps(collider, moving));
                const float forward_toi =
                    forward ? forward->time_of_impact : -1.0f;
                const float reverse_toi =
                    reverse ? reverse->time_of_impact : -1.0f;
                const float forward_depth =
                    forward ? forward->penetration_depth : -1.0f;
                const float reverse_depth =
                    reverse ? reverse->penetration_depth : -1.0f;
                CAPTURE(forward_toi, reverse_toi, forward_depth, reverse_depth);
                REQUIRE(forward.has_value() == reverse.has_value());
                if (forward && reverse) {
                    REQUIRE(forward->time_of_impact ==
                            Catch::Approx(reverse->time_of_impact).margin(5.0e-4f));
                    REQUIRE(forward->initial_overlap == reverse->initial_overlap);
                    REQUIRE(forward->penetration_depth ==
                            Catch::Approx(reverse->penetration_depth).margin(5.0e-3f));
                }
            }
        }
    }

    const Shape grazing_box = recentered(structures[5], layouts[0].moving_center);
    const Shape grazing_capsule = recentered(structures[12], {});
    const auto grazing_hit = shapeCast(grazing_box, layouts[0].delta,
                                       grazing_capsule);
    REQUIRE_FALSE(overlaps(
        translatedForTest(grazing_box, {layouts[0].delta.x * 0.2643f,
                                        layouts[0].delta.y * 0.2643f,
                                        layouts[0].delta.z * 0.2643f}),
        grazing_capsule));
    REQUIRE(overlaps(
        translatedForTest(grazing_box, {layouts[0].delta.x * 0.2645f,
                                        layouts[0].delta.y * 0.2645f,
                                        layouts[0].delta.z * 0.2645f}),
        grazing_capsule));
    REQUIRE(grazing_hit);
    REQUIRE(grazing_hit->time_of_impact >= 0.2643f);
    REQUIRE(grazing_hit->time_of_impact <= 0.2645f);

    CorpusRng rng;
    for (std::size_t sample = 0; sample < 10'000; ++sample) {
        const auto sample_state = rng.state;
        CAPTURE(sample, sample_state);
        const vec3 lateral{0.0f, rng.signedUnit() * 1.5f,
                           rng.signedUnit() * 1.5f};
        const vec3 moving_center{-6.0f, lateral.y, lateral.z};
        const vec3 collider_center{rng.signedUnit() * 0.25f,
                                   -lateral.y * 0.25f,
                                   -lateral.z * 0.25f};
        const vec3 delta{12.0f, -lateral.y * 1.5f,
                         -lateral.z * 1.5f};
        const auto moving = corpusShape(rng, moving_center);
        const auto collider = corpusShape(rng, collider_center);
        try {
            const auto forward = shapeCast(moving, delta, collider);
            const auto reverse = shapeCast(
                collider, {-delta.x, -delta.y, -delta.z}, moving);
            requireValidShapeCastHit(forward);
            requireValidShapeCastHit(reverse);
            REQUIRE(forward.has_value() == reverse.has_value());
        } catch (const std::exception &error) {
            FAIL("sample=" << sample << " state=" << sample_state
                            << " moving_kind=" << moving.index()
                            << " collider_kind=" << collider.index()
                            << " error=" << error.what());
        }
    }
}

TEST_CASE("shapeCast reports deterministic initial-overlap minimum translation",
          "[physquery][shape-cast]") {
    const Shape moving = Sphere{{0.5f, 0.0f, 0.0f}, 1.0f};
    const Shape collider = Sphere{{0.0f, 0.0f, 0.0f}, 1.0f};

    const auto hit = shapeCast(moving, {0.0f, 0.0f, 0.0f}, collider);
    REQUIRE(hit);
    REQUIRE(hit->initial_overlap);
    REQUIRE(hit->time_of_impact == Catch::Approx(0.0f));
    REQUIRE(hit->penetration_depth == Catch::Approx(1.5f).margin(2.0e-3f));
    requireVec(hit->normal, {1.0f, 0.0f, 0.0f}, 2.0e-3f);

    const Shape touching = Sphere{{2.0f, 0.0f, 0.0f}, 1.0f};
    REQUIRE_FALSE(shapeCast(touching, {1.0f, 0.0f, 0.0f}, collider));
    const auto approaching = shapeCast(touching, {-1.0f, 0.0f, 0.0f}, collider);
    REQUIRE(approaching);
    REQUIRE_FALSE(approaching->initial_overlap);
    REQUIRE(approaching->time_of_impact == Catch::Approx(0.0f));

    const std::array<Shape, 2> symmetric_shapes{
        Shape{Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}},
        Shape{Box{{0.0f, 0.0f, 0.0f}, {}, {1.0f, 1.0f, 1.0f}}},
    };
    for (const auto &symmetric : symmetric_shapes) {
        const auto symmetric_hit = shapeCast(symmetric, {}, symmetric);
        INFO("shape=" << symmetric.index());
        REQUIRE(symmetric_hit);
        REQUIRE(symmetric_hit->initial_overlap);
        REQUIRE(symmetric_hit->penetration_depth ==
                Catch::Approx(2.0f).margin(2.0e-3f));
        requireVec(symmetric_hit->normal, {1.0f, 0.0f, 0.0f}, 2.0e-3f);
    }

    const auto movement_tie = shapeCast(symmetric_shapes[1], {0.0f, 1.0f, 0.0f},
                                        symmetric_shapes[1]);
    REQUIRE(movement_tie);
    requireVec(movement_tie->normal, {0.0f, -1.0f, 0.0f}, 2.0e-3f);
}

TEST_CASE("shapeCast rejects non-finite and invalid shape input",
          "[physquery][shape-cast]") {
    const Shape valid = Sphere{{0.0f, 0.0f, 0.0f}, 1.0f};
    REQUIRE_FALSE(shapeCast(
        valid, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, valid));
    REQUIRE_FALSE(shapeCast(
        Box{{0.0f, 0.0f, 0.0f}, {},
             {std::numeric_limits<float>::infinity(), 1.0f, 1.0f}},
        {}, valid));
    REQUIRE_FALSE(shapeCast(Sphere{{}, -1.0f}, {}, valid));
}

TEST_CASE("shapeCastAll shares filters and canonical TOI identity ordering",
          "[physquery][shape-cast]") {
    const ColliderQueryMetadata accepted{
        .layer = 0b0010U,
        .mask = 0b0100U,
    };
    const std::vector<Collider> colliders{
        identifiedCollider("later-id", Sphere{{5.0f, 0.0f, 0.0f}, 0.5f}, 20,
                           accepted),
        identifiedCollider("first-id", Sphere{{5.00005f, 0.0f, 0.0f}, 0.5f}, 10,
                           accepted),
        identifiedCollider("trigger", Sphere{{3.0f, 0.0f, 0.0f}, 0.5f}, 1,
                           ColliderQueryMetadata{.layer = 0b0010U,
                                                 .mask = 0b0100U,
                                                 .trigger = true}),
        identifiedCollider("one-way", Sphere{{3.0f, 0.0f, 0.0f}, 0.5f}, 2,
                           ColliderQueryMetadata{.layer = 0b0010U,
                                                 .mask = 0b0100U,
                                                 .one_way = true}),
    };
    const QueryFilter filter{
        .layer = 0b0100U,
        .mask = 0b0010U,
        .include_triggers = false,
        .include_one_way = false,
    };

    const Shape moving = Sphere{{0.0f, 0.0f, 0.0f}, 0.5f};
    const auto hits = shapeCastAll(moving, {10.0f, 0.0f, 0.0f}, colliders, filter);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].identity.collider_id == ColliderId{10});
    REQUIRE(hits[1].identity.collider_id == ColliderId{20});
    REQUIRE(hits[0].time_of_impact ==
            Catch::Approx(hits[1].time_of_impact).margin(shapeCastTieEpsilon));
    REQUIRE(hits[0].time_of_impact > hits[1].time_of_impact);

    const auto closest = shapeCastClosest(
        moving, {10.0f, 0.0f, 0.0f}, colliders, filter);
    REQUIRE(closest);
    REQUIRE(closest->identity == hits.front().identity);

    const std::array ignored{ColliderId{10}};
    auto continued_filter = filter;
    continued_filter.ignored = ignored;
    const auto continued = shapeCastClosest(
        moving, {10.0f, 0.0f, 0.0f}, colliders, continued_filter);
    REQUIRE(continued);
    REQUIRE(continued->identity.collider_id == ColliderId{20});
}

} // namespace Pelican::phys
