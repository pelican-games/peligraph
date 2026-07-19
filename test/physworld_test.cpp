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
#include <typeindex>
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

struct TriggerEventRecord {
    bool enter = false;
    EntityId self = invalidEntityId;
    EntityId other = invalidEntityId;

    bool operator==(const TriggerEventRecord &) const = default;
};

std::vector<TriggerEventRecord> drainTriggerEvents() {
    internal::freezePendingEventsForFrame();
    const auto queued = internal::getEventRegisterer().drainFrozenEvents();
    std::vector<TriggerEventRecord> result;
    result.reserve(queued.size());
    for (const auto &event : queued) {
        if (event.type == std::type_index{typeid(OverlapEnter)}) {
            const auto &payload =
                *static_cast<const OverlapEnter *>(event.payload.get());
            result.push_back({true, payload.self, payload.other});
        } else if (event.type == std::type_index{typeid(OverlapExit)}) {
            const auto &payload =
                *static_cast<const OverlapExit *>(event.payload.get());
            result.push_back({false, payload.self, payload.other});
        } else {
            FAIL("unexpected event in physics trigger fixture: " << event.name);
        }
    }
    return result;
}

void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::vector<std::uint8_t>
encodeTriggerEvents(const std::vector<TriggerEventRecord> &events) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(events.size() * 17);
    for (const auto &event : events) {
        bytes.push_back(event.enter ? 1U : 0U);
        appendU32(bytes, event.self.index);
        appendU32(bytes, event.self.generation);
        appendU32(bytes, event.other.index);
        appendU32(bytes, event.other.generation);
    }
    return bytes;
}

GameObjectId createColliderEntity(vec3 position) {
    TransformComponent transform;
    transform.pos = glm::vec3{position.x, position.y, position.z};
    transform.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    transform.scale = glm::vec3{1.0f, 1.0f, 1.0f};
    return GameObjects::add()
        .addComponent<TransformComponent>(transform)
        .finish();
}

std::vector<std::uint8_t> runTriggerReplay() {
    internal::clearPendingEvents();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    GameContext ctx;

    const auto trigger_entity = createColliderEntity({0.0f, 0.0f, 0.0f});
    const auto other_entity = createColliderEntity({4.0f, 0.0f, 0.0f});
    auto trigger = ColliderComponent{};
    trigger.radius = 1.0f;
    trigger.trigger = true;
    auto solid = ColliderComponent{};
    solid.radius = 1.0f;
    world.bindCollider("z-trigger", trigger, trigger_entity);
    world.bindCollider("a-solid", solid, other_entity);

    world.updateTriggers(ctx);
    REQUIRE(drainTriggerEvents().empty());

    auto &other_transform = GET_MODULE(ECSCore)
                                .getTemplatePublicModule()
                                .component<TransformComponent>(other_entity);
    other_transform.pos = glm::vec3{1.0f, 0.0f, 0.0f};
    world.updateTriggers(ctx);
    auto records = drainTriggerEvents();
    const std::vector<TriggerEventRecord> expected_enters{
        {true, trigger_entity, other_entity},
        {true, other_entity, trigger_entity},
    };
    REQUIRE(records == expected_enters);

    world.updateTriggers(ctx);
    REQUIRE(drainTriggerEvents().empty());

    other_transform.pos = glm::vec3{4.0f, 0.0f, 0.0f};
    world.updateTriggers(ctx);
    const auto exits = drainTriggerEvents();
    const std::vector<TriggerEventRecord> expected_exits{
        {false, trigger_entity, other_entity},
        {false, other_entity, trigger_entity},
    };
    REQUIRE(exits == expected_exits);
    records.insert(records.end(), exits.begin(), exits.end());

    const auto &trigger_transform = GET_MODULE(ECSCore)
                                        .getTemplatePublicModule()
                                        .component<TransformComponent>(trigger_entity);
    requireVec({trigger_transform.pos.x, trigger_transform.pos.y,
                trigger_transform.pos.z},
               {0.0f, 0.0f, 0.0f});
    requireVec({other_transform.pos.x, other_transform.pos.y,
                other_transform.pos.z},
               {4.0f, 0.0f, 0.0f});
    internal::clearPendingEvents();
    return encodeTriggerEvents(records);
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

TEST_CASE("PhysWorld trigger replay emits only symmetric Enter and Exit deltas",
          "[physworld][trigger][wp179][determinism]") {
    ensureLogger();
    const auto first = runTriggerReplay();
    const auto second = runTriggerReplay();
    REQUIRE_FALSE(first.empty());
    REQUIRE(first == second);
}

TEST_CASE("PhysWorld trigger changes use canonical EntityId pair order",
          "[physworld][trigger][wp179][order]") {
    ensureLogger();
    internal::clearPendingEvents();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    GameContext ctx;

    const auto trigger_entity = createColliderEntity({0.0f, 0.0f, 0.0f});
    const auto second_entity = createColliderEntity({0.0f, 0.0f, 0.0f});
    const auto third_entity = createColliderEntity({0.0f, 0.0f, 0.0f});
    auto trigger = ColliderComponent{};
    trigger.trigger = true;
    world.bindCollider("m-trigger", trigger, trigger_entity);
    world.bindCollider("z-second", ColliderComponent{}, second_entity);
    world.bindCollider("a-third", ColliderComponent{}, third_entity);

    world.updateTriggers(ctx);
    const std::vector<TriggerEventRecord> expected{
        {true, trigger_entity, second_entity},
        {true, second_entity, trigger_entity},
        {true, trigger_entity, third_entity},
        {true, third_entity, trigger_entity},
    };
    REQUIRE(drainTriggerEvents() == expected);
    internal::clearPendingEvents();
}

TEST_CASE("PhysWorld guarantees Exit for destroy and collider removal but not scene reset",
          "[physworld][trigger][wp179][exit]") {
    ensureLogger();
    internal::clearPendingEvents();
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &world = GET_MODULE(PhysWorld);
    GameContext ctx;

    const auto trigger_entity = createColliderEntity({0.0f, 0.0f, 0.0f});
    const auto other_entity = createColliderEntity({0.0f, 0.0f, 0.0f});
    auto trigger = ColliderComponent{};
    trigger.trigger = true;
    world.bindCollider("trigger", trigger, trigger_entity);
    world.bindCollider("other", ColliderComponent{}, other_entity);
    world.updateTriggers(ctx);
    (void)drainTriggerEvents();

    SECTION("entity destroy emits the pending symmetric Exit") {
        REQUIRE(GameObjects::remove(other_entity));
        world.updateTriggers(ctx);
        const std::vector<TriggerEventRecord> expected{
            {false, trigger_entity, other_entity},
            {false, other_entity, trigger_entity},
        };
        REQUIRE(drainTriggerEvents() == expected);
    }

    SECTION("collider removal emits the pending symmetric Exit") {
        auto prepared = world.snapshotPrepared();
        std::erase_if(prepared.bindings, [&](const PhysWorld::Binding &binding) {
            return binding.identity.entity == other_entity;
        });
        world.publishPrepared(world.prepareBindings(std::move(prepared.bindings)));
        world.updateTriggers(ctx);
        const std::vector<TriggerEventRecord> expected{
            {false, trigger_entity, other_entity},
            {false, other_entity, trigger_entity},
        };
        REQUIRE(drainTriggerEvents() == expected);
    }

    SECTION("full scene reset clears pairs without Exit") {
        world.clear();
        world.updateTriggers(ctx);
        REQUIRE(drainTriggerEvents().empty());
    }
    internal::clearPendingEvents();
}

} // namespace Pelican
