#include "../src/core/phys/trianglemesh.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <stdexcept>

namespace Pelican::phys {
namespace {

constexpr std::array<vec3, 3> triangle{{{0, 0, 2}, {2, 0, 2}, {0, 2, 2}}};
constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};

} // namespace

TEST_CASE("Triangle mesh raycast tests triangle surfaces, both sides and parameter bounds",
          "[physics][trianglemesh]") {
    const TriangleMesh mesh(triangle, indices);
    const auto hit = mesh.raycast(Ray{{0.25F, 0.25F, 0}, {0, 0, 2}, 1});
    REQUIRE(hit);
    REQUIRE(hit->distance == Catch::Approx(1));
    REQUIRE(hit->position.z == Catch::Approx(2));
    REQUIRE(hit->normal.z == Catch::Approx(1));
    REQUIRE_FALSE(mesh.raycast(Ray{{0.25F, 0.25F, 0}, {0, 0, 2}, 0.99F}));
    // This ray intersects the triangle's enclosing box, but not its surface.
    REQUIRE_FALSE(mesh.raycast(Ray{{1.5F, 1.5F, 0}, {0, 0, 1}, 10}));
    const auto back = mesh.raycast(Ray{{0.25F, 0.25F, 3}, {0, 0, -1}, 1});
    REQUIRE(back);
    REQUIRE(back->normal.z == Catch::Approx(1));
    REQUIRE(mesh.raycast(Ray{{0, 0, 2}, {0, 0, 1}, 0}));
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 2}, {1, 0, 0}, 10}));
}

TEST_CASE("Triangle mesh BVH finds nearest surfaces and resolves source-order ties",
          "[physics][trianglemesh]") {
    std::vector<vec3> vertices;
    std::vector<std::uint32_t> triangles;
    for (int layer = 8; layer >= 1; --layer) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({0, 0, static_cast<float>(layer)});
        vertices.push_back({2, 0, static_cast<float>(layer)});
        vertices.push_back({0, 2, static_cast<float>(layer)});
        triangles.insert(triangles.end(), {base, base + 1, base + 2});
    }
    // The final triangle occupies the closest surface with reverse winding.
    triangles.insert(triangles.end(), {21, 23, 22});
    const TriangleMesh mesh(vertices, triangles);
    for (int iteration = 0; iteration < 3; ++iteration) {
        const auto hit = mesh.raycast(Ray{{0.25F, 0.25F, 0}, {0, 0, 1}, 20});
        REQUIRE(hit);
        REQUIRE(hit->distance == Catch::Approx(1));
        REQUIRE(hit->normal.z == Catch::Approx(1));
    }
    const auto inside = mesh.raycast(Ray{{0.25F, 0.25F, 3.5F}, {0, 0, 1}, 20});
    REQUIRE(inside);
    REQUIRE(inside->distance == Catch::Approx(0.5F));
}

TEST_CASE("Triangle mesh validates topology and handles empty or degenerate geometry",
          "[physics][trianglemesh]") {
    REQUIRE_NOTHROW(TriangleMesh({}, {}));
    REQUIRE_FALSE(TriangleMesh({}, {}).raycast(Ray{}));
    REQUIRE_FALSE(TriangleMesh(triangle, {}).raycast(Ray{}));
    const std::array<std::uint32_t, 2> incomplete{0, 1};
    const std::array<std::uint32_t, 3> invalid{0, 1, 3};
    REQUIRE_THROWS_AS(TriangleMesh(triangle, incomplete), std::invalid_argument);
    REQUIRE_THROWS_AS(TriangleMesh(triangle, invalid), std::invalid_argument);
    auto nonfinite = triangle;
    nonfinite[1].x = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(TriangleMesh(nonfinite, indices), std::invalid_argument);
    const std::array<vec3, 3> collinear{{{0, 0, 2}, {1, 0, 2}, {2, 0, 2}}};
    REQUIRE_FALSE(TriangleMesh(collinear, indices).raycast(Ray{{1, 0, 0}, {0, 0, 1}, 4}));
}

TEST_CASE("Triangle mesh rejects malformed rays without hits", "[physics][trianglemesh]") {
    const TriangleMesh mesh(triangle, indices);
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 0}, {0, 0, 0}, 10}));
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 0}, {0, 0, 1}, -1}));
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 0}, {0, 0, 1},
                                    std::numeric_limits<float>::infinity()}));
    REQUIRE_FALSE(mesh.raycast(Ray{{std::numeric_limits<float>::quiet_NaN(), 0, 0},
                                    {0, 0, 1}, 10}));
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 0},
                                    {0, 0, std::numeric_limits<float>::infinity()}, 10}));
}

} // namespace Pelican::phys
