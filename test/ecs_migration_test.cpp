#include "../src/core/container.hpp"
#include "../src/core/ecs/archetypemigration.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

struct alignas(128) MigrationMoveOnlyComponent {
    static inline int objects = 0;
    static inline int init_calls = 0;
    static inline int deinit_calls = 0;
    static inline int resources = 0;

    std::string value;
    bool initialized = false;

    MigrationMoveOnlyComponent() { ++objects; }
    MigrationMoveOnlyComponent(const MigrationMoveOnlyComponent &) = delete;
    MigrationMoveOnlyComponent &operator=(const MigrationMoveOnlyComponent &) = delete;
    MigrationMoveOnlyComponent(MigrationMoveOnlyComponent &&other) noexcept
        : value{std::move(other.value)}, initialized{std::exchange(other.initialized, false)} {
        ++objects;
    }
    MigrationMoveOnlyComponent &operator=(MigrationMoveOnlyComponent &&) = delete;
    ~MigrationMoveOnlyComponent() { --objects; }

    void init() {
        initialized = true;
        ++init_calls;
        ++resources;
    }
    void deinit() noexcept {
        if (initialized) {
            initialized = false;
            ++deinit_calls;
            --resources;
        }
    }

    static void reset() {
        objects = 0;
        init_calls = 0;
        deinit_calls = 0;
        resources = 0;
    }
};

struct MigrationAddedComponent {
    static inline bool fail_construct = false;
    static inline bool fail_init = false;
    static inline int objects = 0;
    static inline int init_calls = 0;
    static inline int deinit_calls = 0;
    static inline int destroy_calls = 0;
    static inline int resources = 0;

    int value = 0;
    bool initialized = false;

    MigrationAddedComponent() {
        if (fail_construct) {
            throw std::runtime_error("injected migration construct failure");
        }
        ++objects;
    }
    MigrationAddedComponent(const MigrationAddedComponent &) = delete;
    MigrationAddedComponent &operator=(const MigrationAddedComponent &) = delete;
    MigrationAddedComponent(MigrationAddedComponent &&other) noexcept
        : value{other.value}, initialized{std::exchange(other.initialized, false)} {
        ++objects;
    }
    MigrationAddedComponent &operator=(MigrationAddedComponent &&) = delete;
    ~MigrationAddedComponent() {
        --objects;
        ++destroy_calls;
    }

    void init() {
        ++init_calls;
        if (fail_init) {
            throw std::runtime_error("injected migration init failure");
        }
        initialized = true;
        ++resources;
    }
    void deinit() noexcept {
        if (initialized) {
            initialized = false;
            ++deinit_calls;
            --resources;
        }
    }

    static void reset() {
        fail_construct = false;
        fail_init = false;
        objects = 0;
        init_calls = 0;
        deinit_calls = 0;
        destroy_calls = 0;
        resources = 0;
    }
};

struct MigrationSystemProbe {
    int calls = 0;
    std::vector<size_t> entity_counts;
    std::vector<int> values;

    void process(std::span<ChunkView<MigrationMoveOnlyComponent, MigrationAddedComponent>> chunks) {
        ++calls;
        size_t entity_count = 0;
        for (const auto &chunk : chunks) {
            entity_count += chunk.count;
            auto *added = std::get<1>(chunk.components);
            for (size_t i = 0; i < chunk.count; ++i) {
                values.push_back(added[i].value);
            }
        }
        entity_counts.push_back(entity_count);
    }
};

struct BindingAdapter final : ECSArchetypeMigrationAdapter {
    bool fail_prepare = false;
    bool prepared = false;
    bool bound = false;
    int prepare_calls = 0;
    int rollback_calls = 0;
    int publish_calls = 0;

    void prepare(const ECSArchetypeMigrationPrepareContext &context) override {
        ++prepare_calls;
        prepared = true;
        if (context.kind == ECSArchetypeMigrationKind::add) {
            REQUIRE(context.live_component == nullptr);
            REQUIRE(context.staged_component != nullptr);
            REQUIRE(context.core.tryComponent<MigrationAddedComponent>(context.entity) == nullptr);
        } else {
            REQUIRE(context.live_component != nullptr);
            REQUIRE(context.staged_component == nullptr);
        }
        if (fail_prepare) {
            throw std::runtime_error("injected external binding prepare failure");
        }
    }

    void rollback(const ECSArchetypeMigrationPrepareContext &) noexcept override {
        ++rollback_calls;
        prepared = false;
    }

    void publish(const ECSArchetypeMigrationPublishContext &context) noexcept override {
        ++publish_calls;
        prepared = false;
        bound = context.kind == ECSArchetypeMigrationKind::add;
    }
};

} // namespace

DECLARE_COMPONENT(MigrationMoveOnlyComponent, 50);
DECLARE_COMPONENT(MigrationAddedComponent, 51);

namespace {

void registerMigrationComponents() {
    static const bool logger_initialized = [] {
        setupLogger();
        return true;
    }();
    (void)logger_initialized;
    auto &registerer = internal::getComponentRegisterer();
    registerer.registerComponent<EntityId>("migration_entity_id");
    registerer.registerComponent<MigrationMoveOnlyComponent>("migration_move_only");
    registerer.registerComponent<MigrationAddedComponent>("migration_added");
}

constexpr std::array<ComponentId, 1> migrationBaseComponents() {
    return {ComponentIdByType<MigrationMoveOnlyComponent>::value};
}

EntityId createMigrationEntity(ECSCoreTemplatePublic &core, std::string value) {
    return core.createEntity(migrationBaseComponents(), [value = std::move(value)](std::span<void *> ptrs) {
        static_cast<MigrationMoveOnlyComponent *>(ptrs[0])->value = value;
    });
}

void requireUnchanged(ECSCoreTemplatePublic &core, EntityId entity,
                      MigrationMoveOnlyComponent *original_address, const std::string &value) {
    REQUIRE(core.liveCount() == 1);
    REQUIRE(core.isAlive(entity));
    REQUIRE(core.tryComponent<MigrationMoveOnlyComponent>(entity) == original_address);
    REQUIRE(core.component<MigrationMoveOnlyComponent>(entity).value == value);
    REQUIRE(core.tryComponent<MigrationAddedComponent>(entity) == nullptr);
    REQUIRE(MigrationMoveOnlyComponent::resources == 1);
    REQUIRE(MigrationAddedComponent::objects == 0);
    REQUIRE(MigrationAddedComponent::resources == 0);
}

TEST_CASE("ECS migration preflight rejects invalid archetype edits without mutation",
          "[ecs][migration][preflight]") {
    FastModuleContainer modules;
    registerMigrationComponents();
    MigrationMoveOnlyComponent::reset();
    MigrationAddedComponent::reset();
    ECSCoreTemplatePublic core;
    const auto entity = createMigrationEntity(core, "preflight-source");
    auto *const original = core.tryComponent<MigrationMoveOnlyComponent>(entity);

    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::add(core, entity,
                                   ComponentIdByType<MigrationMoveOnlyComponent>::value),
        Catch::Matchers::ContainsSubstring("already exists"));
    requireUnchanged(core, entity, original, "preflight-source");

    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::remove(core, entity, ComponentIdByType<EntityId>::value),
        Catch::Matchers::ContainsSubstring("cannot be removed"));
    requireUnchanged(core, entity, original, "preflight-source");

    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::remove(core, entity,
                                      ComponentIdByType<MigrationAddedComponent>::value),
        Catch::Matchers::ContainsSubstring("does not exist"));
    requireUnchanged(core, entity, original, "preflight-source");

    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::add(
            core, EntityId{entity.index, entity.generation + 1},
            ComponentIdByType<MigrationAddedComponent>::value),
        Catch::Matchers::ContainsSubstring("not live"));
    requireUnchanged(core, entity, original, "preflight-source");

    BindingAdapter adapter;
    std::array<ECSArchetypeMigrationAdapter *, 2> duplicated{&adapter, &adapter};
    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::add(core, entity,
                                   ComponentIdByType<MigrationAddedComponent>::value, {}, duplicated),
        Catch::Matchers::ContainsSubstring("duplicated"));
    requireUnchanged(core, entity, original, "preflight-source");

    core.clearEntities();
    REQUIRE(MigrationMoveOnlyComponent::resources == 0);
}

TEST_CASE("ECS migration construct populate init and adapter faults restore exact live state",
          "[ecs][migration][fault-matrix][atomic]") {
    FastModuleContainer modules;
    registerMigrationComponents();
    MigrationMoveOnlyComponent::reset();
    MigrationAddedComponent::reset();
    ECSCoreTemplatePublic core;
    const auto entity = createMigrationEntity(core, "fault-source");
    const auto spare = createMigrationEntity(core, "free-list-spare");
    REQUIRE(core.remove(spare));
    auto *const original = core.tryComponent<MigrationMoveOnlyComponent>(entity);

    MigrationAddedComponent::fail_construct = true;
    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::add(core, entity,
                                   ComponentIdByType<MigrationAddedComponent>::value),
        Catch::Matchers::ContainsSubstring("construct failure"));
    MigrationAddedComponent::fail_construct = false;
    requireUnchanged(core, entity, original, "fault-source");

    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::add(
            core, entity, ComponentIdByType<MigrationAddedComponent>::value,
            [](void *) { throw std::runtime_error("injected migration populate failure"); }),
        Catch::Matchers::ContainsSubstring("populate failure"));
    requireUnchanged(core, entity, original, "fault-source");

    MigrationAddedComponent::fail_init = true;
    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::add(core, entity,
                                   ComponentIdByType<MigrationAddedComponent>::value),
        Catch::Matchers::ContainsSubstring("init failure"));
    MigrationAddedComponent::fail_init = false;
    requireUnchanged(core, entity, original, "fault-source");

    BindingAdapter prepared_binding;
    BindingAdapter failing_binding;
    failing_binding.fail_prepare = true;
    std::array<ECSArchetypeMigrationAdapter *, 2> adapters{&prepared_binding, &failing_binding};
    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::add(
            core, entity, ComponentIdByType<MigrationAddedComponent>::value,
            [](void *ptr) { static_cast<MigrationAddedComponent *>(ptr)->value = 71; }, adapters),
        Catch::Matchers::ContainsSubstring("external binding prepare failure"));
    requireUnchanged(core, entity, original, "fault-source");
    REQUIRE(prepared_binding.rollback_calls == 1);
    REQUIRE(failing_binding.rollback_calls == 1);
    REQUIRE_FALSE(prepared_binding.prepared);
    REQUIRE_FALSE(failing_binding.prepared);
    REQUIRE_FALSE(prepared_binding.bound);
    REQUIRE_FALSE(failing_binding.bound);
    REQUIRE(MigrationAddedComponent::deinit_calls == 1);

    const auto reused = createMigrationEntity(core, "free-list-reused");
    REQUIRE(reused.index == spare.index);
    REQUIRE(reused.generation == spare.generation + 1);
    REQUIRE(entity == EntityId{0, 0});
    core.clearEntities();
    REQUIRE(MigrationMoveOnlyComponent::resources == 0);
}

TEST_CASE("ECS migration commits move-only over-aligned values and lifecycle exactly once",
          "[ecs][migration][move-only][alignment][lifecycle]") {
    FastModuleContainer modules;
    registerMigrationComponents();
    MigrationMoveOnlyComponent::reset();
    MigrationAddedComponent::reset();
    ECSCoreTemplatePublic core;
    const auto first = createMigrationEntity(core, "first");
    const auto second = createMigrationEntity(core, "second-backfill");
    BindingAdapter binding;
    std::array<ECSArchetypeMigrationAdapter *, 1> adapters{&binding};

    ECSArchetypeMigration::add(
        core, first, ComponentIdByType<MigrationAddedComponent>::value,
        [](void *ptr) { static_cast<MigrationAddedComponent *>(ptr)->value = 91; }, adapters);

    REQUIRE(core.liveCount() == 2);
    REQUIRE(core.isAlive(first));
    REQUIRE(core.isAlive(second));
    REQUIRE(core.component<MigrationMoveOnlyComponent>(first).value == "first");
    REQUIRE(core.component<MigrationMoveOnlyComponent>(second).value == "second-backfill");
    REQUIRE(reinterpret_cast<std::uintptr_t>(
                core.tryComponent<MigrationMoveOnlyComponent>(first)) %
                alignof(MigrationMoveOnlyComponent) ==
            0);
    REQUIRE(core.component<MigrationAddedComponent>(first).value == 91);
    REQUIRE(MigrationMoveOnlyComponent::init_calls == 2);
    REQUIRE(MigrationMoveOnlyComponent::resources == 2);
    REQUIRE(MigrationAddedComponent::init_calls == 1);
    REQUIRE(MigrationAddedComponent::resources == 1);
    REQUIRE(binding.bound);
    REQUIRE(binding.publish_calls == 1);

    BindingAdapter failing_remove_binding;
    failing_remove_binding.fail_prepare = true;
    std::array<ECSArchetypeMigrationAdapter *, 2> failing_remove_adapters{
        &binding, &failing_remove_binding};
    auto *const added_before_remove_fault =
        core.tryComponent<MigrationAddedComponent>(first);
    REQUIRE_THROWS_WITH(
        ECSArchetypeMigration::remove(
            core, first, ComponentIdByType<MigrationAddedComponent>::value,
            failing_remove_adapters),
        Catch::Matchers::ContainsSubstring("external binding prepare failure"));
    REQUIRE(core.tryComponent<MigrationAddedComponent>(first) == added_before_remove_fault);
    REQUIRE(core.component<MigrationAddedComponent>(first).value == 91);
    REQUIRE(MigrationAddedComponent::resources == 1);
    REQUIRE(binding.bound);
    REQUIRE(binding.rollback_calls == 1);
    REQUIRE(failing_remove_binding.rollback_calls == 1);

    ECSArchetypeMigration::remove(
        core, first, ComponentIdByType<MigrationAddedComponent>::value, adapters);
    REQUIRE(core.isAlive(first));
    REQUIRE(core.component<MigrationMoveOnlyComponent>(first).value == "first");
    REQUIRE(core.tryComponent<MigrationAddedComponent>(first) == nullptr);
    REQUIRE(MigrationAddedComponent::resources == 0);
    REQUIRE(MigrationAddedComponent::deinit_calls == 1);
    REQUIRE(MigrationAddedComponent::objects == 0);
    REQUIRE_FALSE(binding.bound);
    REQUIRE(binding.publish_calls == 2);

    core.clearEntities();
    REQUIRE(MigrationMoveOnlyComponent::deinit_calls == 2);
    REQUIRE(MigrationMoveOnlyComponent::resources == 0);
    REQUIRE(MigrationMoveOnlyComponent::objects == 0);
}

TEST_CASE("ECS migration rolls back and commits system chunk cache and versions",
          "[ecs][migration][scheduler][version]") {
    FastModuleContainer modules;
    registerMigrationComponents();
    MigrationMoveOnlyComponent::reset();
    MigrationAddedComponent::reset();
    ECSCoreTemplatePublic core;
    const auto entity = createMigrationEntity(core, "system-source");
    MigrationSystemProbe system;
    core.registerSystem<MigrationSystemProbe, MigrationMoveOnlyComponent, MigrationAddedComponent>(
        system, {});

    core.update();
    REQUIRE(system.calls == 1);
    REQUIRE(system.entity_counts == std::vector<size_t>{0});

    MigrationAddedComponent::fail_construct = true;
    REQUIRE_THROWS(ECSArchetypeMigration::add(
        core, entity, ComponentIdByType<MigrationAddedComponent>::value));
    MigrationAddedComponent::fail_construct = false;
    core.update();
    REQUIRE(system.calls == 1);

    ECSArchetypeMigration::add(
        core, entity, ComponentIdByType<MigrationAddedComponent>::value,
        [](void *ptr) { static_cast<MigrationAddedComponent *>(ptr)->value = 123; });
    core.update();
    REQUIRE(system.calls == 2);
    REQUIRE(system.entity_counts.back() == 1);
    REQUIRE(system.values.back() == 123);

    ECSArchetypeMigration::remove(core, entity,
                                  ComponentIdByType<MigrationAddedComponent>::value);
    core.update();
    REQUIRE(system.calls == 3);
    REQUIRE(system.entity_counts.back() == 0);

    core.clearEntities();
    REQUIRE(MigrationMoveOnlyComponent::resources == 0);
}

TEST_CASE("ECS migration prepare tokens leave live state unchanged and invert publication",
          "[ecs][migration][prepared-token][atomic]") {
    FastModuleContainer modules;
    registerMigrationComponents();
    MigrationMoveOnlyComponent::reset();
    MigrationAddedComponent::reset();
    ECSCoreTemplatePublic core;
    const auto entity = createMigrationEntity(core, "prepared-source");
    auto *const original = core.tryComponent<MigrationMoveOnlyComponent>(entity);

    auto add = ECSArchetypeMigration::prepareAdd(
        core, entity, ComponentIdByType<MigrationAddedComponent>::value,
        [](void *ptr) {
            static_cast<MigrationAddedComponent *>(ptr)->value = 808;
        });
    REQUIRE(core.liveCount() == 1);
    REQUIRE(core.isAlive(entity));
    REQUIRE(core.tryComponent<MigrationMoveOnlyComponent>(entity) == original);
    REQUIRE(core.component<MigrationMoveOnlyComponent>(entity).value ==
            "prepared-source");
    REQUIRE(core.tryComponent<MigrationAddedComponent>(entity) == nullptr);
    REQUIRE(MigrationMoveOnlyComponent::resources == 1);
    REQUIRE(MigrationAddedComponent::resources == 1);
    REQUIRE(static_cast<MigrationAddedComponent *>(add.stagedComponent())->value ==
            808);
    add.publish();
    REQUIRE(core.component<MigrationAddedComponent>(entity).value == 808);
    add.rollback();
    requireUnchanged(core, entity, original, "prepared-source");

    auto committed_add = ECSArchetypeMigration::prepareAdd(
        core, entity, ComponentIdByType<MigrationAddedComponent>::value,
        [](void *ptr) {
            static_cast<MigrationAddedComponent *>(ptr)->value = 909;
        });
    committed_add.publish();
    committed_add.finish();
    REQUIRE(core.component<MigrationAddedComponent>(entity).value == 909);

    auto remove = ECSArchetypeMigration::prepareRemove(
        core, entity, ComponentIdByType<MigrationAddedComponent>::value);
    REQUIRE(core.component<MigrationAddedComponent>(entity).value == 909);
    remove.publish();
    REQUIRE(core.tryComponent<MigrationAddedComponent>(entity) == nullptr);
    remove.rollback();
    REQUIRE(core.component<MigrationAddedComponent>(entity).value == 909);
    core.clearEntities();
    REQUIRE(MigrationMoveOnlyComponent::resources == 0);
    REQUIRE(MigrationAddedComponent::resources == 0);
}

TEST_CASE("ECS entity create and destroy prepared tokens restore id generation and free list",
          "[ecs][entity-mutation][prepared-token][atomic]") {
    FastModuleContainer modules;
    registerMigrationComponents();
    MigrationMoveOnlyComponent::reset();
    MigrationAddedComponent::reset();
    ECSCoreTemplatePublic core;
    const auto existing = createMigrationEntity(core, "existing");
    const std::array components{
        ComponentIdByType<MigrationMoveOnlyComponent>::value};

    auto create = ECSEntityMutation::prepareCreate(
        core, components, [](std::span<void *> values) {
            static_cast<MigrationMoveOnlyComponent *>(values[0])->value =
                "prepared-create";
        });
    const auto candidate = create.entity();
    REQUIRE_FALSE(core.isAlive(candidate));
    REQUIRE(core.liveCount() == 1);
    create.publish();
    REQUIRE(core.isAlive(candidate));
    REQUIRE(core.component<MigrationMoveOnlyComponent>(candidate).value ==
            "prepared-create");
    create.rollback();
    REQUIRE_FALSE(core.isAlive(candidate));
    REQUIRE(core.liveCount() == 1);

    auto destroy = ECSEntityMutation::prepareDestroy(core, existing);
    REQUIRE(core.isAlive(existing));
    REQUIRE(core.component<MigrationMoveOnlyComponent>(existing).value ==
            "existing");
    destroy.publish();
    REQUIRE_FALSE(core.isAlive(existing));
    destroy.rollback();
    REQUIRE(core.isAlive(existing));
    REQUIRE(core.component<MigrationMoveOnlyComponent>(existing).value ==
            "existing");

    auto committed_destroy = ECSEntityMutation::prepareDestroy(core, existing);
    committed_destroy.publish();
    committed_destroy.finish();
    REQUIRE_FALSE(core.isAlive(existing));
    const auto reused = createMigrationEntity(core, "reused");
    REQUIRE(reused.index == existing.index);
    REQUIRE(reused.generation == existing.generation + 1);
    core.clearEntities();
    REQUIRE(MigrationMoveOnlyComponent::resources == 0);
}

} // namespace
} // namespace Pelican
