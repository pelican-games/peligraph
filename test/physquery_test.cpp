#include "../src/core/phys/physquery.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

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

TEST_CASE("overlapAll returns deterministic sorted ids", "[physquery]") {
    const std::vector<Collider> colliders{
        {"z", Sphere{{0.0f, 0.0f, 0.0f}, 0.25f}},
        {"miss", Sphere{{10.0f, 0.0f, 0.0f}, 0.25f}},
        {"a", Box{{0.0f, 0.0f, 0.0f}, rotationZ(kPi * 0.25f), {0.25f, 0.25f, 0.25f}}},
    };

    const std::vector<std::string> ids = overlapAll(Sphere{{0.0f, 0.0f, 0.0f}, 1.0f}, colliders);
    REQUIRE(ids == std::vector<std::string>{"a", "z"});
}

} // namespace Pelican::phys
