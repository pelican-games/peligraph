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

TEST_CASE("Triangle mesh world queries preserve distance under affine transforms",
          "[physics][trianglemesh]") {
    const TriangleMesh mesh(triangle, indices);
    glm::mat4 transform(1.0F);
    transform[0][0] = 2.0F;
    transform[1][1] = 3.0F;
    transform[2][2] = 4.0F;
    transform[3] = glm::vec4(5.0F, 6.0F, 7.0F, 1.0F);
    const auto hit = mesh.raycast(Ray{{5.5F, 6.75F, 10}, {0, 0, 10}, 5}, transform);
    REQUIRE(hit);
    REQUIRE(hit->distance == Catch::Approx(5));
    REQUIRE(hit->position.x == Catch::Approx(5.5F));
    REQUIRE(hit->position.y == Catch::Approx(6.75F));
    REQUIRE(hit->position.z == Catch::Approx(15));
    REQUIRE(hit->normal.z == Catch::Approx(1));
    REQUIRE_FALSE(mesh.raycast(Ray{{5.5F, 6.75F, 10}, {0, 0, 10}, 4.99F}, transform));

    transform[0][0] = -2.0F;
    const auto reflected = mesh.raycast(Ray{{4.5F, 6.75F, 10}, {0, 0, 1}, 5}, transform);
    REQUIRE(reflected);
    REQUIRE(reflected->normal.z == Catch::Approx(-1));
    REQUIRE(reflected->distance == Catch::Approx(5));
    const auto original = mesh.raycast(Ray{{0.25F, 0.25F, 0}, {0, 0, 1}, 2});
    REQUIRE(original);
    REQUIRE(original->normal.z == Catch::Approx(1));
    REQUIRE(original->distance == Catch::Approx(2));
    REQUIRE(mesh.vertices()[1].x == triangle[1].x);
    REQUIRE(mesh.indices()[1] == indices[1]);
}

TEST_CASE("Triangle mesh transforms sloped normals with inverse transpose",
          "[physics][trianglemesh]") {
    const std::array<vec3, 3> slope{{{0, 0, 0}, {1, 0, 0}, {0, 1, 1}}};
    const TriangleMesh mesh(slope, indices);
    glm::mat4 transform(1.0F);
    transform[1][1] = 2.0F;
    const auto hit = mesh.raycast(Ray{{0.25F, 0.5F, -1}, {0, 0, 1}, 2}, transform);
    REQUIRE(hit);
    REQUIRE(hit->distance == Catch::Approx(1.25F));
    REQUIRE(hit->normal.x == Catch::Approx(0).margin(1.0e-6));
    REQUIRE(hit->normal.y == Catch::Approx(-0.4472135955));
    REQUIRE(hit->normal.z == Catch::Approx(0.894427191));
}

TEST_CASE("Triangle mesh world queries reject invalid transforms and rays",
          "[physics][trianglemesh]") {
    const TriangleMesh mesh(triangle, indices);
    const Ray ray{{0.25F, 0.25F, 0}, {0, 0, 1}, 10};
    glm::mat4 transform(1.0F);
    transform[0][0] = 0.0F;
    REQUIRE_FALSE(mesh.raycast(ray, transform));
    transform = glm::mat4(1.0F);
    transform[3][1] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(mesh.raycast(ray, transform));
    transform = glm::mat4(1.0F);
    transform[0][3] = 0.5F;
    REQUIRE_FALSE(mesh.raycast(ray, transform));
    transform = glm::mat4(1.0F);
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 0}, {0, 0, 0}, 10}, transform));
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 0}, {0, 0, 1}, -1}, transform));
    REQUIRE_FALSE(mesh.raycast(Ray{{0, 0, 0},
                                    {0, 0, std::numeric_limits<float>::quiet_NaN()}, 10},
                                transform));
}

} // namespace Pelican::phys
