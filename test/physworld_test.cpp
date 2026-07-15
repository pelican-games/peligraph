#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined/transform.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/log.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#include "../src/core/userpublic/serialize/jsonarchive.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {
namespace {

ColliderComponent parseCollider(const nlohmann::json &json) {
    ColliderComponent collider;
    JsonArchiveLoader archive{static_cast<const void *>(&json)};
    collider.ref(archive);
    return collider;
}

void requireVec(vec3 actual, vec3 expected, float epsilon = 1.0e-4f) {
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(epsilon));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(epsilon));
    REQUIRE(actual.z == Catch::Approx(expected.z).margin(epsilon));
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

} // namespace

TEST_CASE("Collider component parses sphere box and capsule scene syntax", "[physworld]") {
    const auto sphere = parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "sphere"},
        {"radius", 1.25f},
    });
    REQUIRE(sphere.shape == "sphere");
    REQUIRE(sphere.radius == Catch::Approx(1.25f));
    requireVec(sphere.pos, {0.0f, 0.0f, 0.0f});

    const auto box = parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "box"},
        {"half_extents", {1.0f, 2.0f, 3.0f}},
    });
    REQUIRE(box.shape == "box");
    requireVec(box.half_extents, {1.0f, 2.0f, 3.0f});

    const auto capsule = parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "capsule"},
        {"radius", 0.5f},
        {"half_height", 2.0f},
    });
    REQUIRE(capsule.shape == "capsule");
    REQUIRE(capsule.radius == Catch::Approx(0.5f));
    REQUIRE(capsule.half_height == Catch::Approx(2.0f));
}

TEST_CASE("Collider component parses query metadata with stable defaults",
          "[physworld]") {
    const auto defaults = parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "sphere"},
        {"radius", 1.0f},
    });
    REQUIRE(defaults.layer == 1U);
    REQUIRE(defaults.mask == ~std::uint32_t{0});
    REQUIRE_FALSE(defaults.trigger);
    REQUIRE_FALSE(defaults.one_way);

    const auto configured = parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "sphere"},
        {"radius", 1.0f},
        {"layer", 0b0010U},
        {"mask", 0b0100U},
        {"trigger", true},
        {"one_way", true},
    });
    REQUIRE(configured.layer == 0b0010U);
    REQUIRE(configured.mask == 0b0100U);
    REQUIRE(configured.trigger);
    REQUIRE(configured.one_way);

    const auto colliders = buildPhysColliders(std::array{
        PhysWorldColliderInput{
            .name = "metadata-target",
            .collider = configured,
            .transform = PhysWorldTransform{.pos = {3.0f, 0.0f, 0.0f}},
        },
    });
    REQUIRE(colliders.size() == 1);
    REQUIRE(colliders[0].metadata.layer == configured.layer);
    REQUIRE(colliders[0].metadata.mask == configured.mask);
    REQUIRE(colliders[0].metadata.trigger);
    REQUIRE(colliders[0].metadata.one_way);

    const phys::QueryFilter excluded{
        .layer = 0b0100U,
        .mask = 0b0010U,
        .include_triggers = false,
    };
    REQUIRE(phys::raycastAll(
                phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
                colliders, excluded)
                .empty());
}

TEST_CASE("Collider component rejects malformed query metadata and non-finite dimensions",
          "[physworld]") {
    REQUIRE_THROWS(parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "sphere"},
        {"radius", 1.0f},
        {"layer", -1},
    }));
    REQUIRE_THROWS(parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "sphere"},
        {"radius", 1.0f},
        {"trigger", 1},
    }));
    auto invalid = ColliderComponent{};
    invalid.radius = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS(invalid.validate());

    REQUIRE_THROWS(buildPhysColliders(std::array{
        PhysWorldColliderInput{
            .name = "non-finite-transform",
            .collider = ColliderComponent{},
            .transform = PhysWorldTransform{
                .pos = {std::numeric_limits<float>::infinity(), 0.0f, 0.0f},
            },
        },
    }));
}

TEST_CASE("Collider component rejects size aliases with v1 replacement names", "[physworld]") {
    std::string box_message;
    try {
        (void)parseCollider(nlohmann::json{
            {"name", "collider"},
            {"shape", "box"},
            {"size", {2.0f, 4.0f, 6.0f}},
        });
    } catch (const std::exception &ex) {
        box_message = ex.what();
    }
    REQUIRE(contains(box_message, "size"));
    REQUIRE(contains(box_message, "half_extents"));

    std::string capsule_message;
    try {
        (void)parseCollider(nlohmann::json{
            {"name", "collider"},
            {"shape", "capsule"},
            {"radius", 0.5f},
            {"height", 4.0f},
        });
    } catch (const std::exception &ex) {
        capsule_message = ex.what();
    }
    REQUIRE(contains(capsule_message, "height"));
    REQUIRE(contains(capsule_message, "half_height"));
}

TEST_CASE("Collider component rejects unknown shapes and negative dimensions", "[physworld]") {
    std::string unknown_message;
    try {
        (void)parseCollider(nlohmann::json{
            {"name", "collider"},
            {"shape", "mesh"},
        });
    } catch (const std::exception &ex) {
        unknown_message = ex.what();
    }
    REQUIRE(contains(unknown_message, "Unknown collider shape"));

    std::string negative_message;
    try {
        (void)parseCollider(nlohmann::json{
            {"name", "collider"},
            {"shape", "box"},
            {"half_extents", {1.0f, -1.0f, 1.0f}},
        });
    } catch (const std::exception &ex) {
        negative_message = ex.what();
    }
    REQUIRE(contains(negative_message, "half_extents"));
}

TEST_CASE("PhysWorld pure build feeds deterministic raycast and overlap queries", "[physworld]") {
    const std::vector<PhysWorldColliderInput> inputs{
        PhysWorldColliderInput{
            .name = "SphereTarget",
            .collider = parseCollider(nlohmann::json{
                {"name", "collider"},
                {"shape", "sphere"},
                {"radius", 1.0f},
            }),
            .transform = PhysWorldTransform{
                .pos = vec3{3.0f, 0.0f, 0.0f},
                .rotation = quat{0.0f, 0.0f, 0.0f, 1.0f},
                .scale = vec3{1.0f, 1.0f, 1.0f},
            },
        },
        PhysWorldColliderInput{
            .name = "BoxTarget",
            .collider = parseCollider(nlohmann::json{
                {"name", "collider"},
                {"shape", "box"},
                {"half_extents", {0.5f, 0.5f, 0.5f}},
            }),
            .transform = PhysWorldTransform{
                .pos = vec3{5.0f, 0.0f, 0.0f},
                .rotation = quat{0.0f, 0.0f, 0.0f, 1.0f},
                .scale = vec3{1.0f, 1.0f, 1.0f},
            },
        },
    };
    const auto colliders = buildPhysColliders(inputs);

    REQUIRE(colliders.size() == 2);
    REQUIRE(colliders[0].id == "BoxTarget");
    REQUIRE(colliders[1].id == "SphereTarget");
    REQUIRE(colliders[0].identity.collider_id.valid());
    REQUIRE(colliders[1].identity.collider_id.valid());
    REQUIRE(colliders[0].identity.collider_id != colliders[1].identity.collider_id);

    auto reordered_inputs = inputs;
    std::reverse(reordered_inputs.begin(), reordered_inputs.end());
    const auto reordered_colliders = buildPhysColliders(reordered_inputs);
    REQUIRE(reordered_colliders[0].identity == colliders[0].identity);
    REQUIRE(reordered_colliders[1].identity == colliders[1].identity);

    const auto hit = phys::raycastClosest(phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, colliders);
    REQUIRE(hit);
    REQUIRE(hit->id == "SphereTarget");
    REQUIRE(hit->distance == Catch::Approx(2.0f));
    const auto detailed_hit = phys::raycastClosest(
        phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, colliders,
        phys::QueryFilter{});
    REQUIRE(detailed_hit);
    REQUIRE(detailed_hit->identity == colliders[1].identity);

    const auto overlaps = phys::overlapAll(phys::Sphere{{3.0f, 0.0f, 0.0f}, 1.1f}, colliders);
    REQUIRE(overlaps == std::vector<std::string>{"SphereTarget"});
}

TEST_CASE("PhysWorld module follows bound transforms and GameContext exposes queries", "[physworld]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);

    TransformComponent transform;
    transform.pos = glm::vec3{10.0f, 0.0f, 0.0f};
    transform.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    transform.scale = glm::vec3{1.0f, 1.0f, 1.0f};

    const auto collider = parseCollider(nlohmann::json{
        {"name", "collider"},
        {"shape", "sphere"},
        {"radius", 1.0f},
    });
    const auto object = GameObjects::add().addComponent<TransformComponent>(transform).finish();
    world.bindCollider("MovingSphere", collider, object);

    GameContext ctx;
    REQUIRE_FALSE(ctx.raycastClosest(phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f}));

    GET_MODULE(ECSCore).getTemplatePublicModule().component<TransformComponent>(object).pos =
        glm::vec3{3.0f, 0.0f, 0.0f};
    const auto hit = ctx.raycastClosest(phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f});
    REQUIRE(hit);
    REQUIRE(hit->id == "MovingSphere");
    REQUIRE(hit->distance == Catch::Approx(2.0f));
    const auto detailed_hit = ctx.raycastClosest(
        phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f},
        phys::QueryFilter{});
    REQUIRE(detailed_hit);
    REQUIRE(detailed_hit->identity.collider_id.valid());
    REQUIRE(detailed_hit->identity.entity == object);
    const auto original_collider_id = detailed_hit->identity.collider_id;

    const phys::QueryFilter ignore_original{.self = original_collider_id};
    REQUIRE_FALSE(ctx.raycastClosest(
        phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f},
        ignore_original));

    const auto overlaps = ctx.overlapAll(phys::Sphere{{3.0f, 0.0f, 0.0f}, 0.25f});
    REQUIRE(overlaps == std::vector<std::string>{"MovingSphere"});

    const auto cast_hit = ctx.shapeCastClosest(
        phys::Sphere{{0.0f, 0.0f, 0.0f}, 0.5f},
        {5.0f, 0.0f, 0.0f});
    REQUIRE(cast_hit);
    REQUIRE(cast_hit->id == "MovingSphere");
    REQUIRE(cast_hit->time_of_impact == Catch::Approx(0.3f).margin(2.0e-4f));

    REQUIRE(GameObjects::remove(object));
    REQUIRE_FALSE(ctx.raycastClosest(phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f}));

    transform.pos = glm::vec3{2.0f, 0.0f, 0.0f};
    const auto replacement = GameObjects::add().addComponent<TransformComponent>(transform).finish();
    REQUIRE_NOTHROW(world.bindCollider("MovingSphere", collider, replacement));
    REQUIRE(world.colliderCountForTesting() == 1);
    const auto replacement_hit = ctx.raycastClosest(
        phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f},
        phys::QueryFilter{});
    REQUIRE(replacement_hit);
    REQUIRE(replacement_hit->identity.collider_id != original_collider_id);
    REQUIRE(replacement_hit->identity.entity == replacement);
    REQUIRE(ctx.raycastClosest(
        phys::Ray{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 5.0f},
        ignore_original));
}

} // namespace Pelican
