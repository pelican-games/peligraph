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

} // namespace Pelican::phys
