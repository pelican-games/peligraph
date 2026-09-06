#include "../src/core/phys/trianglemesh.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
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

TEST_CASE("Box mesh sweep uses continuous face and edge separation axes",
          "[physics][trianglemesh][box-mesh-sweep]") {
    const TriangleMesh mesh(triangle,indices);
    const glm::mat4 identity{1};
    const Box box{{0.5F,0.5F,0},{0,0,0,1},{0.25F,0.25F,0.25F}};
    const auto face = mesh.shapeCast(box,{0,0,10},identity);
    REQUIRE(face);
    REQUIRE(face->time_of_impact == Catch::Approx(0.175F));
    REQUIRE(face->position.z == Catch::Approx(2));
    REQUIRE(face->normal.z == Catch::Approx(-1));
    REQUIRE_FALSE(face->initial_overlap);
    REQUIRE_FALSE(mesh.shapeCast(box,{0,0,1.7F},identity));
    const auto endpoint = mesh.shapeCast(box,{0,0,1.75F},identity);
    REQUIRE(endpoint);
    REQUIRE(endpoint->time_of_impact == 1);
    auto backBox = box; backBox.center.z = 4;
    const auto back = mesh.shapeCast(backBox,{0,0,-4},identity);
    REQUIRE(back);
    REQUIRE(back->time_of_impact == Catch::Approx(0.4375F));
    REQUIRE(back->normal.z == Catch::Approx(1));
    // All coordinate projections overlap well before the slanted edge does.
    const Box edgeBox{{2,2,2},{0,0,0,1},{0.25F,0.25F,0.25F}};
    const auto edge = mesh.shapeCast(edgeBox,{-2,-2,0},identity);
    REQUIRE(edge);
    REQUIRE(edge->time_of_impact == Catch::Approx(0.375F));
    REQUIRE(edge->normal.x == Catch::Approx(1/std::sqrt(2.0F)));
    REQUIRE(edge->normal.y == Catch::Approx(1/std::sqrt(2.0F)));
    REQUIRE(edge->position.x+edge->position.y == Catch::Approx(2));
    auto miss = box; miss.center = {1.5F,1.5F,0};
    REQUIRE_FALSE(mesh.shapeCast(miss,{0,0,4},identity));
}

TEST_CASE("Box mesh sweep handles rotated boxes affine meshes and initial contact",
          "[physics][trianglemesh][box-mesh-sweep]") {
    const TriangleMesh mesh(triangle,indices);
    const glm::mat4 identity{1};
    const Box rotated{{0.5F,0.5F,0},{0.38268343F,0,0,0.92387953F},{0.1F,0.5F,0.25F}};
    const auto hit = mesh.shapeCast(rotated,{0,0,4},identity);
    REQUIRE(hit);
    REQUIRE(hit->time_of_impact == Catch::Approx((2-0.75F/std::sqrt(2.0F))/4));
    REQUIRE(hit->position.z == Catch::Approx(2));
    Box touching{{0.5F,0.5F,1.75F},{0,0,0,1},{0.25F,0.25F,0.25F}};
    const auto approach = mesh.shapeCast(touching,{0,0,1},identity);
    REQUIRE(approach);
    REQUIRE(approach->time_of_impact == 0);
    REQUIRE_FALSE(mesh.shapeCast(touching,{},identity));
    REQUIRE_FALSE(mesh.shapeCast(touching,{0,0,-1},identity));
    REQUIRE_FALSE(mesh.shapeCast(touching,{1,0,0},identity));
    // At a corner, approaching one constraint while leaving the other is a miss.
    touching.center = {-0.25F,-0.25F,2};
    REQUIRE_FALSE(mesh.shapeCast(touching,{1,-1,0},identity));
    REQUIRE(mesh.shapeCast(touching,{1,1,0},identity));
    touching.center = {0.5F,0.5F,2};
    REQUIRE_THROWS_AS(mesh.shapeCast(touching,{},identity),std::domain_error);
    const Box point{{0.5F,0.5F,2},{0,0,0,1},{0,0,0}};
    const auto pointContact = mesh.shapeCast(point,{0,0,1},identity);
    REQUIRE(pointContact);
    REQUIRE(pointContact->time_of_impact == 0);
    REQUIRE(pointContact->normal.z == Catch::Approx(-1));
    REQUIRE_FALSE(mesh.shapeCast(point,{},identity));
    glm::mat4 transform{1};
    transform[0][0] = -2; transform[1][0] = 0.5F;
    transform[2][2] = 3; transform[3][2] = 2;
    touching.center = {-0.75F,0.5F,10};
    const auto transformed = mesh.shapeCast(touching,{0,0,-4},transform);
    REQUIRE(transformed);
    REQUIRE(transformed->time_of_impact == Catch::Approx(0.4375F));
    REQUIRE(transformed->position.z == Catch::Approx(8));
    REQUIRE(transformed->normal.z == Catch::Approx(1));
    touching.rotation = {0,0,0,0};
    REQUIRE_FALSE(mesh.shapeCast(touching,{0,0,4},identity));
}

TEST_CASE("Sphere mesh sweep finds continuous face edge and vertex contacts",
          "[physics][trianglemesh][sphere-mesh-sweep]") {
    const TriangleMesh mesh(triangle,indices);
    const glm::mat4 identity{1};
    const auto face = mesh.shapeCast(Sphere{{0.5F,0.5F,0},0.25F},{0,0,10},identity);
    REQUIRE(face);
    REQUIRE(face->time_of_impact == Catch::Approx(0.175F));
    REQUIRE(face->position.z == Catch::Approx(2));
    REQUIRE(face->normal.z == Catch::Approx(-1));
    REQUIRE_FALSE(face->initial_overlap);
    REQUIRE_FALSE(mesh.shapeCast(Sphere{{0.5F,0.5F,0},0.25F},{0,0,1.7F},identity));
    const auto back = mesh.shapeCast(Sphere{{0.5F,0.5F,4},0.25F},{0,0,-4},identity);
    REQUIRE(back);
    REQUIRE(back->time_of_impact == Catch::Approx(0.4375F));
    REQUIRE(back->normal.z == Catch::Approx(1));
    // Edge and vertex paths lie outside the triangle's face projection.
    const auto edge = mesh.shapeCast(Sphere{{1,-1,2},0.25F},{0,2,0},identity);
    REQUIRE(edge);
    REQUIRE(edge->time_of_impact == Catch::Approx(0.375F));
    REQUIRE(edge->position.y == Catch::Approx(0));
    REQUIRE(edge->normal.y == Catch::Approx(-1));
    const auto vertex = mesh.shapeCast(Sphere{{-1,-1,2},0.25F},{2,2,0},identity);
    REQUIRE(vertex);
    REQUIRE(vertex->time_of_impact == Catch::Approx((1-0.25F/std::sqrt(2.0F))/2));
    REQUIRE(vertex->position.x == Catch::Approx(0));
    REQUIRE(vertex->position.y == Catch::Approx(0));
    const auto grazing = mesh.shapeCast(Sphere{{-1,-0.25F,2},0.25F},{2,0,0},identity);
    REQUIRE(grazing);
    REQUIRE(grazing->time_of_impact == Catch::Approx(0.5F));
    REQUIRE_FALSE(mesh.shapeCast(Sphere{{1.8F,1.8F,0},0.1F},{0,0,4},identity));
    const auto point = mesh.shapeCast(Sphere{{0.5F,0.5F,0},0},{0,0,4},identity);
    REQUIRE(point);
    REQUIRE(point->time_of_impact == Catch::Approx(0.5F));
}

TEST_CASE("Sphere mesh sweep preserves contact rules and transformed world radius",
          "[physics][trianglemesh][sphere-mesh-sweep]") {
    const TriangleMesh mesh(triangle,indices);
    const glm::mat4 identity{1};
    const Sphere touching{{0.5F,0.5F,1.75F},0.25F};
    const auto approach = mesh.shapeCast(touching,{0,0,1},identity);
    REQUIRE(approach);
    REQUIRE(approach->time_of_impact == 0);
    REQUIRE_FALSE(mesh.shapeCast(touching,{0,0,-1},identity));
    REQUIRE_FALSE(mesh.shapeCast(touching,{1,0,0},identity));
    REQUIRE_FALSE(mesh.shapeCast(touching,{},identity));
    REQUIRE_THROWS_AS(mesh.shapeCast(Sphere{{0.5F,0.5F,2},0.25F},{0,0,1},identity),std::domain_error);
    REQUIRE_THROWS_AS(mesh.shapeCast(Sphere{{0.5F,0.5F,2},0.25F},{},identity),std::domain_error);
    glm::mat4 transform{1};
    transform[0][0] = -2;
    transform[1][0] = 0.5F;
    transform[2][2] = 3;
    transform[3][2] = 2;
    const auto transformed = mesh.shapeCast(Sphere{{-0.75F,0.5F,10},0.25F},{0,0,-4},transform);
    REQUIRE(transformed);
    REQUIRE(transformed->time_of_impact == Catch::Approx(0.4375F));
    REQUIRE(transformed->position.z == Catch::Approx(8));
    REQUIRE(transformed->normal.z == Catch::Approx(1));
    REQUIRE_FALSE(mesh.shapeCast(Sphere{{0,0,0},-1},{0,0,4},identity));
    REQUIRE_FALSE(mesh.shapeCast(Sphere{{0,0,0},1},{0,0,std::numeric_limits<float>::infinity()},identity));
    transform[0][0] = 0;
    REQUIRE_FALSE(mesh.shapeCast(touching,{0,0,4},transform));
}

TEST_CASE("Sphere mesh sweep traverses BVH and does not hide later initial penetrations",
          "[physics][trianglemesh][sphere-mesh-sweep]") {
    std::vector<vec3> vertices;
    std::vector<std::uint32_t> triangles;
    for (int layer = 8; layer >= 1; --layer) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({0,0,static_cast<float>(layer)});
        vertices.push_back({2,0,static_cast<float>(layer)});
        vertices.push_back({0,2,static_cast<float>(layer)});
        triangles.insert(triangles.end(),{base,base+1,base+2});
    }
    const TriangleMesh mesh(vertices,triangles);
    const auto nearest = mesh.shapeCast(Sphere{{0.5F,0.5F,0},0.25F},{0,0,10},glm::mat4{1});
    REQUIRE(nearest);
    REQUIRE(nearest->time_of_impact == Catch::Approx(0.075F));
    REQUIRE(nearest->position.z == Catch::Approx(1));
    REQUIRE_THROWS_AS(mesh.shapeCast(Sphere{{0.5F,0.5F,7},1},{0,0,-10},glm::mat4{1}),std::domain_error);
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

TEST_CASE("Triangle mesh sphere overlap resolves faces edges and transformed surfaces",
          "[physics][trianglemesh][mesh-overlap]") {
    const TriangleMesh mesh(triangle, indices);
    const glm::mat4 identity{1};
    CHECK(mesh.overlaps(Sphere{{0.5F, 0.5F, 2.5F}, 0.5F}, identity));
    CHECK_FALSE(mesh.overlaps(Sphere{{0.5F, 0.5F, 2.51F}, 0.5F}, identity));
    CHECK(mesh.overlaps(Sphere{{1, -0.25F, 2}, 0.25F}, identity));
    CHECK(mesh.overlaps(Sphere{{-0.5F, 0, 2}, 0.5F}, identity));
    CHECK_FALSE(mesh.overlaps(Sphere{{1.8F, 1.8F, 2}, 0.1F}, identity));
    CHECK(mesh.overlaps(Sphere{{0.5F, 0.5F, 2}, 0}, identity));
    glm::mat4 transform{1};
    transform[0][0] = -2;
    transform[1][1] = 3;
    transform[2][2] = 0.5F;
    transform[3][2] = 7;
    CHECK(mesh.overlaps(Sphere{{-0.5F, 0.5F, 8.25F}, 0.25F}, transform));
    CHECK_FALSE(mesh.overlaps(Sphere{{-0.5F, 0.5F, 8.3F}, 0.25F}, transform));
    CHECK_FALSE(mesh.overlaps(Sphere{{0, 0, 2}, -1}, identity));
    CHECK_FALSE(mesh.overlaps(Sphere{{0, 0, 2}, std::numeric_limits<float>::infinity()}, identity));
    transform[0][0] = 0;
    CHECK_FALSE(mesh.overlaps(Sphere{{0, 0, 2}, 1}, transform));
    std::vector<vec3> vertices;
    std::vector<std::uint32_t> corners;
    for (std::uint32_t i = 0; i < 12; ++i) {
        for (auto v : triangle) { v.x += i * 5; vertices.push_back(v); }
        for (auto index : indices) corners.push_back(i * 3 + index);
    }
    const TriangleMesh tree(vertices, corners);
    CHECK(tree.overlaps(Sphere{{55.5F, 0.5F, 2.25F}, 0.25F}, identity));
    CHECK_FALSE(tree.overlaps(Sphere{{53, 0.5F, 2}, 0.1F}, identity));
}

TEST_CASE("Triangle mesh box overlap separates triangle surfaces from bounds", "[physics][trianglemesh][box-mesh]") {
    const TriangleMesh mesh(triangle, indices);
    const glm::mat4 identity{1};
    Box box{{0.5F,0.5F,2.25F}, {0,0,0,1}, {0.1F,0.1F,0.25F}};
    CHECK(mesh.overlaps(box, identity));
    box.center.z = 2.3F;
    CHECK_FALSE(mesh.overlaps(box, identity));
    box.center = {1.8F,1.8F,2};
    CHECK_FALSE(mesh.overlaps(box, identity)); // Inside triangle AABB, outside triangle.
    box.center = {1,1,2};
    box.half_extents = {0.5F,0.05F,0.1F};
    box.rotation = {0,0,0.38268343F,0.92387953F};
    CHECK(mesh.overlaps(box, identity));
    box.center = {1.4F,1.4F,2};
    CHECK_FALSE(mesh.overlaps(box, identity));
    glm::mat4 transform{1};
    transform[0][0] = -2; transform[1][0] = 0.5F; transform[2][2] = 3; transform[3][2] = 2;
    box = Box{{-0.75F,0.5F,8.25F}, {0,0,0,1}, {0.1F,0.1F,0.25F}};
    CHECK(mesh.overlaps(box, transform));
    box.center.z = 8.3F;
    CHECK_FALSE(mesh.overlaps(box, transform));
    box.rotation = {0,0,0,0};
    CHECK_FALSE(mesh.overlaps(box, identity));
    box.rotation = {0,0,0,1}; box.half_extents.x = -1;
    CHECK_FALSE(mesh.overlaps(box, identity));
}

TEST_CASE("Capsule mesh overlap covers segment interior caps edges and transformed triangles", "[physics][trianglemesh][capsule-mesh]") {
    const TriangleMesh mesh(triangle, indices);
    const glm::mat4 identity{1};
    Capsule capsule{{0.5F,0.5F,2.25F}, {0,0,0,1}, 3, 0.25F};
    CHECK(mesh.overlaps(capsule, identity)); // Parallel segment; both endpoints outside triangle.
    capsule.center.z = 2.3F;
    CHECK_FALSE(mesh.overlaps(capsule, identity));
    capsule = Capsule{{0.5F,0.5F,2}, {0.70710678F,0,0,0.70710678F}, 2, 0};
    CHECK(mesh.overlaps(capsule, identity)); // Segment crosses face, endpoints far away.
    capsule.center = {1.8F,1.8F,2};
    CHECK_FALSE(mesh.overlaps(capsule, identity)); // Inside broad bounds, outside triangle.
    capsule = Capsule{{0.5F,0.5F,2.75F}, {0.70710678F,0,0,0.70710678F}, 0.5F, 0.25F};
    CHECK(mesh.overlaps(capsule, identity)); // End cap touches face.
    capsule.center.z = 2.8F;
    CHECK_FALSE(mesh.overlaps(capsule, identity));
    capsule = Capsule{{1,-0.25F,2}, {0,0,0,1}, 0, 0.25F};
    CHECK(mesh.overlaps(capsule, identity)); // Degenerate segment touches edge.
    CHECK(mesh.overlaps(capsule, identity) == mesh.overlaps(Sphere{capsule.center,capsule.radius}, identity));
    glm::mat4 transform{1};
    transform[0][0] = -2; transform[1][0] = 0.5F; transform[2][2] = 3; transform[3][2] = 2;
    capsule = Capsule{{-0.75F,0.5F,8.25F}, {0,0,0,1}, 0.1F, 0.25F};
    CHECK(mesh.overlaps(capsule, transform));
    capsule.center.z = 8.3F;
    CHECK_FALSE(mesh.overlaps(capsule, transform));
    capsule.half_height = -1;
    CHECK_FALSE(mesh.overlaps(capsule, identity));
    capsule.half_height = 1; capsule.rotation = {0,0,0,0};
    CHECK_FALSE(mesh.overlaps(capsule, identity));
}

} // namespace Pelican::phys
