#include "../src/core/container.hpp"
#include "../src/core/appflow/teardown.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/archetypemigration.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/components/predefined.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

struct TrivialValueComponent {
    int value;
};

struct PrepareProbeComponent {
    int value = 0;
};

DECLARE_MODULE(PrepareProbeDependency) {
  public:
    int value = 42;
};

struct PrepareProbeSystem {
    PrepareProbeDependency *dependency = nullptr;
    std::thread::id prepare_thread;
    std::thread::id process_thread;
    bool saw_matching_chunks = false;
    bool process_saw_dependency = false;

    void prepareEcsWorkerDependencies(bool has_matching_chunks) {
        prepare_thread = std::this_thread::get_id();
        saw_matching_chunks = has_matching_chunks;
        dependency = &GET_MODULE(PrepareProbeDependency);
    }

    void process(std::tuple<PrepareProbeComponent *> components, size_t count) {
        process_thread = std::this_thread::get_id();
        process_saw_dependency = dependency != nullptr && dependency->value == 42;
        if (count != 0) std::get<0>(components)[0].value = dependency->value;
    }
};

struct RemovalReadProbeSystem {
    int calls = 0;
    std::vector<size_t> entity_counts;

    void process(std::tuple<const TrivialValueComponent *>, size_t count) {
        ++calls;
        entity_counts.push_back(count);
    }
};

struct alignas(64) LifecycleCanary {
    static inline std::vector<std::string> events;
    std::string value;

    LifecycleCanary() { events.emplace_back("construct"); }
    LifecycleCanary(const LifecycleCanary &) = delete;
    LifecycleCanary &operator=(const LifecycleCanary &) = delete;
    LifecycleCanary(LifecycleCanary &&other) noexcept : value{std::move(other.value)} {
        events.emplace_back("move");
    }
    LifecycleCanary &operator=(LifecycleCanary &&) noexcept = default;
    ~LifecycleCanary() { events.emplace_back("destroy"); }
    void init() { events.emplace_back("init"); }
    void deinit() noexcept { events.emplace_back("deinit"); }
};

struct InitFirstComponent {
    static inline std::vector<std::string> events;
    static inline int resources = 0;
    void init() {
        events.emplace_back("first init");
        ++resources;
    }
    void deinit() noexcept {
        events.emplace_back("first deinit");
        --resources;
    }
};

struct InitFaultComponent {
    static inline bool fail = false;
    void init() {
        InitFirstComponent::events.emplace_back("fault init");
        ++InitFirstComponent::resources;
        if (fail) {
            --InitFirstComponent::resources;
            throw std::runtime_error("injected init failure");
        }
    }
    void deinit() noexcept {
        InitFirstComponent::events.emplace_back("fault deinit");
        --InitFirstComponent::resources;
    }
};

struct ReentrantComponent {
    static inline ECSCoreTemplatePublic *core = nullptr;
    static inline bool rejected = false;
    static inline bool saw_unpublished = false;
    void init() {
        saw_unpublished = core->liveCount() == 0;
        try {
            (void)core->createEntity({});
        } catch (const std::logic_error &) {
            rejected = true;
        }
    }
};

struct MoveOnlyComponent {
    int value = 0;
    MoveOnlyComponent() = default;
    MoveOnlyComponent(const MoveOnlyComponent &) = delete;
    MoveOnlyComponent &operator=(const MoveOnlyComponent &) = delete;
    MoveOnlyComponent(MoveOnlyComponent &&) noexcept = default;
    MoveOnlyComponent &operator=(MoveOnlyComponent &&) noexcept = default;
};

struct TeardownComponent {
    static inline std::vector<std::string> events;
    TeardownComponent() = default;
    TeardownComponent(const TeardownComponent &) = delete;
    TeardownComponent &operator=(const TeardownComponent &) = delete;
    TeardownComponent(TeardownComponent &&) noexcept = default;
    TeardownComponent &operator=(TeardownComponent &&) noexcept = default;
    ~TeardownComponent() { events.emplace_back("destroy"); }
    void deinit() noexcept { events.emplace_back("deinit"); }
};

struct ModelViewGpuInstanceCanary {
    static inline int live_instances = 0;
    bool owns_instance = false;

    void init() {
        owns_instance = true;
        ++live_instances;
    }
    void deinit() noexcept {
        if (owns_instance) {
            owns_instance = false;
            --live_instances;
        }
    }
};

} // namespace

DECLARE_COMPONENT(TrivialValueComponent, 40);
DECLARE_COMPONENT(LifecycleCanary, 41);
DECLARE_COMPONENT(InitFirstComponent, 42);
DECLARE_COMPONENT(InitFaultComponent, 43);
DECLARE_COMPONENT(ReentrantComponent, 44);
DECLARE_COMPONENT(MoveOnlyComponent, 45);
DECLARE_COMPONENT(TeardownComponent, 46);
DECLARE_COMPONENT(ModelViewGpuInstanceCanary, 47);
DECLARE_COMPONENT(PrepareProbeComponent, 48);

DECLARE_MODULE(TeardownMarker) {
  public:
    ~TeardownMarker() { TeardownComponent::events.emplace_back("module destroy"); }
};

namespace {

template <class... Components>
void registerComponents() {
    static const bool logger_initialized = [] {
        setupLogger();
        return true;
    }();
    (void)logger_initialized;
    auto &registerer = internal::getComponentRegisterer();
    registerer.registerComponent<EntityId>("test_entity_id");
    (registerer.registerComponent<Components>(typeid(Components).name()), ...);
}

template <class Component>
constexpr std::array<ComponentId, 1> componentIds() {
    return {ComponentIdByType<Component>::value};
}

} // namespace

TEST_CASE("EntityId canonical representation and value construction are fixed", "[ecs][lifecycle]") {
    STATIC_REQUIRE(sizeof(EntityId) == 8);
    STATIC_REQUIRE(std::is_trivially_copyable_v<EntityId>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<MoveOnlyComponent>);
    REQUIRE(EntityId{} == invalidEntityId);
    REQUIRE(EntityId{}.index == std::numeric_limits<std::uint32_t>::max());
    REQUIRE(LocalTransformComponent{}.parent == invalidEntityId);

    FastModuleContainer modules;
    registerComponents<TrivialValueComponent>();
    ECSCoreTemplatePublic core;
    const auto id = core.createEntity(componentIds<TrivialValueComponent>());
    REQUIRE(core.component<TrivialValueComponent>(id).value == 0);
    core.clearEntities();
}

TEST_CASE("Unified modelview survives JSON population and non-trivial relocation", "[ecs][lifecycle][modelview]") {
    STATIC_REQUIRE(std::is_default_constructible_v<SimpleModelViewComponent>);
    STATIC_REQUIRE(std::is_copy_assignable_v<SimpleModelViewComponent>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<SimpleModelViewComponent>);
    STATIC_REQUIRE(std::is_nothrow_destructible_v<SimpleModelViewComponent>);

    FastModuleContainer modules;
    registerComponents<SimpleModelViewComponent>();
    ECSCoreTemplatePublic core;
    const std::array<std::string, 2> model_names{
        "model-name-longer-than-the-small-string-optimization-buffer-a",
        "model-name-longer-than-the-small-string-optimization-buffer-b",
    };
    const auto ids = core.createEntities(
        componentIds<SimpleModelViewComponent>(), model_names.size(),
        [&](std::span<const EntityId>, std::span<void *> ptrs, size_t count) {
            auto *models = static_cast<SimpleModelViewComponent *>(ptrs[0]);
            for (size_t i = 0; i < count; ++i) {
                const nlohmann::json component{
                    {"name", typeid(SimpleModelViewComponent).name()},
                    {"model", model_names[i]},
                };
                GET_MODULE(ComponentInfoManager).loadByJson(&models[i], component);
            }
        });

    REQUIRE(core.component<SimpleModelViewComponent>(ids[0]).model_name == model_names[0]);
    REQUIRE(core.component<SimpleModelViewComponent>(ids[1]).model_name == model_names[1]);
    REQUIRE(core.component<SimpleModelViewComponent>(ids[0]).dirty == 1);
    REQUIRE(core.component<SimpleModelViewComponent>(ids[1]).dirty == 1);

    REQUIRE(core.remove(ids[0]));
    REQUIRE(core.component<SimpleModelViewComponent>(ids[1]).model_name == model_names[1]);
    core.clearEntities();

    SimpleModelViewComponent transient_model_view;
    transient_model_view.model_instance_id = ModelInstanceId{7, 1, 1};
    transient_model_view.init();
    REQUIRE(transient_model_view.dirty == 0);
}

TEST_CASE("Modelview GPU instance deinit covers remove clear and teardown", "[ecs][lifecycle][modelview]") {
    SECTION("remove") {
        FastModuleContainer modules;
        registerComponents<ModelViewGpuInstanceCanary>();
        ECSCoreTemplatePublic core;
        ModelViewGpuInstanceCanary::live_instances = 0;
        const auto ids = core.createEntities(componentIds<ModelViewGpuInstanceCanary>(), 2);
        REQUIRE(ModelViewGpuInstanceCanary::live_instances == 2);
        REQUIRE(core.remove(ids[0]));
        REQUIRE(ModelViewGpuInstanceCanary::live_instances == 1);
        REQUIRE(core.remove(ids[1]));
        REQUIRE(ModelViewGpuInstanceCanary::live_instances == 0);
    }

    SECTION("clear") {
        FastModuleContainer modules;
        registerComponents<ModelViewGpuInstanceCanary>();
        ECSCoreTemplatePublic core;
        ModelViewGpuInstanceCanary::live_instances = 0;
        (void)core.createEntities(componentIds<ModelViewGpuInstanceCanary>(), 2);
        REQUIRE(ModelViewGpuInstanceCanary::live_instances == 2);
        core.clearEntities();
        REQUIRE(ModelViewGpuInstanceCanary::live_instances == 0);
    }

    SECTION("teardown") {
        FastModuleContainer modules;
        RuntimeTeardownGuard teardown;
        registerComponents<ModelViewGpuInstanceCanary>();
        ModelViewGpuInstanceCanary::live_instances = 0;
        (void)GET_MODULE(ECSCore).createEntities(componentIds<ModelViewGpuInstanceCanary>(), 2);
        REQUIRE(ModelViewGpuInstanceCanary::live_instances == 2);
        teardown.run();
        REQUIRE(ModelViewGpuInstanceCanary::live_instances == 0);
    }
}

TEST_CASE("Aligned non-trivial component observes lifecycle and relocation order", "[ecs][lifecycle]") {
    FastModuleContainer modules;
    registerComponents<LifecycleCanary>();
    ECSCoreTemplatePublic core;
    LifecycleCanary::events.clear();
    std::vector<std::uintptr_t> addresses;
    const auto ids = core.createEntities(
        componentIds<LifecycleCanary>(), 2,
        [&](std::span<const EntityId>, std::span<void *> ptrs, size_t count) {
            auto *values = static_cast<LifecycleCanary *>(ptrs[0]);
            for (size_t i = 0; i < count; ++i) {
                addresses.push_back(reinterpret_cast<std::uintptr_t>(&values[i]));
                values[i].value = std::to_string(i);
            }
        });
    REQUIRE(addresses.size() == 2);
    REQUIRE(addresses[0] % alignof(LifecycleCanary) == 0);
    REQUIRE(addresses[1] % alignof(LifecycleCanary) == 0);
    REQUIRE(LifecycleCanary::events ==
            std::vector<std::string>{"construct", "construct", "init", "init"});

    LifecycleCanary::events.clear();
    REQUIRE(core.remove(ids[0]));
    REQUIRE(LifecycleCanary::events ==
            std::vector<std::string>{"deinit", "destroy", "move", "destroy"});
    REQUIRE(core.component<LifecycleCanary>(ids[1]).value == "1");

    LifecycleCanary::events.clear();
    REQUIRE(core.remove(ids[1]));
    REQUIRE(LifecycleCanary::events == std::vector<std::string>{"deinit", "destroy"});

    const auto reused = core.createEntities(componentIds<LifecycleCanary>(), 2);
    REQUIRE(reused.size() == 2);
    REQUIRE(reinterpret_cast<std::uintptr_t>(&core.component<LifecycleCanary>(reused[0])) %
                alignof(LifecycleCanary) ==
            0);
    core.clearEntities();
}

TEST_CASE("Populate and init failures roll back storage IDs and resources", "[ecs][lifecycle]") {
    FastModuleContainer modules;
    registerComponents<LifecycleCanary, InitFirstComponent, InitFaultComponent>();
    ECSCoreTemplatePublic core;

    REQUIRE_THROWS_AS(core.createEntity(componentIds<LifecycleCanary>(), [](std::span<void *>) {
                          throw std::runtime_error("injected populate failure");
                      }),
                      std::runtime_error);
    REQUIRE(core.liveCount() == 0);
    const auto after_populate_failure = core.createEntity(componentIds<LifecycleCanary>());
    REQUIRE(after_populate_failure == EntityId{0, 0});
    REQUIRE(core.remove(after_populate_failure));

    InitFirstComponent::events.clear();
    InitFirstComponent::resources = 0;
    InitFaultComponent::fail = true;
    constexpr std::array init_ids{ComponentIdByType<InitFirstComponent>::value,
                                  ComponentIdByType<InitFaultComponent>::value};
    REQUIRE_THROWS_WITH(core.createEntity(init_ids),
                        Catch::Matchers::ContainsSubstring("injected init failure"));
    REQUIRE(InitFirstComponent::resources == 0);
    REQUIRE(InitFirstComponent::events ==
            std::vector<std::string>{"first init", "fault init", "first deinit"});
    REQUIRE(core.liveCount() == 0);
    InitFaultComponent::fail = false;
}

TEST_CASE("Structural callback reentry is rejected before nested state changes", "[ecs][lifecycle]") {
    FastModuleContainer modules;
    registerComponents<ReentrantComponent>();
    ECSCoreTemplatePublic core;
    ReentrantComponent::core = &core;
    ReentrantComponent::rejected = false;
    ReentrantComponent::saw_unpublished = false;
    (void)core.createEntity(componentIds<ReentrantComponent>());
    REQUIRE(ReentrantComponent::rejected);
    REQUIRE(ReentrantComponent::saw_unpublished);
    REQUIRE(core.liveCount() == 1);
    core.clearEntities();
}

TEST_CASE("Bulk creation splits chunks and faults atomically", "[ecs][lifecycle]") {
    FastModuleContainer modules;
    registerComponents<TrivialValueComponent>();
    ECSCoreTemplatePublic core;
    for (const auto count : {size_t{4095}, size_t{4096}, size_t{4097}, size_t{100'000}}) {
        size_t populated = 0;
        const auto ids = core.createEntities(
            componentIds<TrivialValueComponent>(), count,
            [&](std::span<const EntityId> batch_ids, std::span<void *>, size_t batch_count) {
                REQUIRE(batch_ids.size() == batch_count);
                REQUIRE(batch_count <= ECSComponentChunk::CHUNK_CAPACITY);
                populated += batch_count;
            });
        REQUIRE(ids.size() == count);
        REQUIRE(populated == count);
        core.clearEntities();
    }

    ECSCoreTemplatePublic mixed_core;
    const auto mixed_source = mixed_core.createEntities(componentIds<TrivialValueComponent>(), 4097);
    REQUIRE(mixed_core.remove(mixed_source.front()));
    REQUIRE(mixed_core.remove(mixed_source.back()));
    const auto mixed = mixed_core.createEntities(componentIds<TrivialValueComponent>(), 3);
    REQUIRE(mixed[0].index == mixed_source.back().index);
    REQUIRE(mixed[1].index == mixed_source.front().index);
    REQUIRE(mixed[2].index == 4097);
    mixed_core.clearEntities();

    size_t callback_index = 0;
    REQUIRE_THROWS_WITH(
        core.createEntities(componentIds<TrivialValueComponent>(), 4097,
                            [&](std::span<const EntityId>, std::span<void *>, size_t) {
                                if (++callback_index == 2) {
                                    throw std::runtime_error("bulk fault");
                                }
                            }),
        Catch::Matchers::ContainsSubstring("bulk fault"));
    REQUIRE(core.liveCount() == 0);
}

TEST_CASE("Generation table rejects every stale classification and retires max generation", "[ecs][lifecycle]") {
    FastModuleContainer modules;
    registerComponents<TrivialValueComponent>();
    ECSCoreTemplatePublic core;
    const auto first = core.createEntity(componentIds<TrivialValueComponent>());
    REQUIRE_FALSE(core.remove(invalidEntityId));
    REQUIRE(core.tryComponent<TrivialValueComponent>(invalidEntityId) == nullptr);
    REQUIRE_FALSE(core.remove(EntityId{999, 0}));
    REQUIRE(core.remove(first));
    REQUIRE_FALSE(core.remove(first));
    REQUIRE(core.tryComponent<TrivialValueComponent>(first) == nullptr);
    REQUIRE_FALSE(core.markComponentChanged(first, ComponentIdByType<TrivialValueComponent>::value));
    REQUIRE_THROWS_WITH(core.component<TrivialValueComponent>(first),
                        Catch::Matchers::ContainsSubstring("not found on entity 0:0"));

    const auto reused = core.createEntity(componentIds<TrivialValueComponent>());
    REQUIRE(reused.index == first.index);
    REQUIRE(reused.generation == first.generation + 1);
    const EntityId wrong_generation{reused.index, reused.generation + 1};
    REQUIRE(core.tryComponent<TrivialValueComponent>(wrong_generation) == nullptr);
    REQUIRE_FALSE(core.setComponent<TrivialValueComponent>(wrong_generation, TrivialValueComponent{7}));
    core.clearEntities();
    REQUIRE(core.tryComponent<TrivialValueComponent>(reused) == nullptr);
    const auto after_clear = core.createEntity(componentIds<TrivialValueComponent>());
    REQUIRE(after_clear.index == reused.index);
    REQUIRE(after_clear.generation == reused.generation + 1);

    const auto max_id = core.forceGenerationForTesting(after_clear, std::numeric_limits<std::uint32_t>::max());
    REQUIRE(core.remove(max_id));
    const auto after_retire = core.createEntity(componentIds<TrivialValueComponent>());
    REQUIRE(after_retire.index != max_id.index);

    const auto max_clear_id = core.forceGenerationForTesting(after_retire,
                                                              std::numeric_limits<std::uint32_t>::max());
    core.clearEntities();
    const auto after_max_clear = core.createEntity(componentIds<TrivialValueComponent>());
    REQUIRE(after_max_clear.index != max_clear_id.index);
    REQUIRE_THROWS_AS(ECSCoreTemplatePublic::validateFreshIndexCapacityForTesting(
                          std::numeric_limits<std::uint32_t>::max()),
                      std::length_error);
    core.clearEntities();
}

TEST_CASE("Free-list LIFO allocation is deterministic across runs", "[ecs][lifecycle]") {
    FastModuleContainer modules;
    registerComponents<TrivialValueComponent>();
    auto run = [] {
        ECSCoreTemplatePublic core;
        (void)core.createEntities(componentIds<TrivialValueComponent>(), 4);
        core.clearEntities();
        const auto ids = core.createEntities(componentIds<TrivialValueComponent>(), 4);
        std::vector<std::uint32_t> indices;
        for (const auto id : ids) {
            indices.push_back(id.index);
        }
        core.clearEntities();
        return indices;
    };
    REQUIRE(run() == std::vector<std::uint32_t>{3, 2, 1, 0});
    REQUIRE(run() == run());
}

TEST_CASE("Move-only components relocate without copy fallback", "[ecs][lifecycle]") {
    FastModuleContainer modules;
    registerComponents<MoveOnlyComponent>();
    ECSCoreTemplatePublic core;
    const auto ids = core.createEntities(
        componentIds<MoveOnlyComponent>(), 2,
        [](std::span<const EntityId>, std::span<void *> ptrs, size_t) {
            auto *values = static_cast<MoveOnlyComponent *>(ptrs[0]);
            values[0].value = 10;
            values[1].value = 20;
        });
    REQUIRE(core.remove(ids[0]));
    REQUIRE(core.component<MoveOnlyComponent>(ids[1]).value == 20);
    core.clearEntities();
}

TEST_CASE("Prepared component swaps follow entities across archetype migration",
          "[ecs][lifecycle][migration][prepared-token]") {
    FastModuleContainer modules;
    registerComponents<TrivialValueComponent, PrepareProbeComponent>();
    ECSCoreTemplatePublic core;
    const auto ids = core.createEntities(
        componentIds<TrivialValueComponent>(), 2,
        [](std::span<const EntityId>, std::span<void *> ptrs, size_t) {
            auto *values = static_cast<TrivialValueComponent *>(ptrs[0]);
            values[0].value = 10;
            values[1].value = 20;
        });

    auto swap = core.prepareComponentSwap(ids[1], TrivialValueComponent{99});
    auto migration = ECSArchetypeMigration::prepareAdd(
        core, ids[1], ComponentIdByType<PrepareProbeComponent>::value);
    migration.publish();
    swap.publish();

    REQUIRE(core.component<TrivialValueComponent>(ids[1]).value == 99);
    swap.rollback();
    migration.rollback();
    REQUIRE(core.component<TrivialValueComponent>(ids[1]).value == 20);
    REQUIRE(core.tryComponent<PrepareProbeComponent>(ids[1]) == nullptr);
    core.clearEntities();
}

TEST_CASE("Entity removal invalidates non-forced read systems",
          "[ecs][lifecycle][scheduler][version]") {
    FastModuleContainer modules;
    registerComponents<TrivialValueComponent>();
    ECSCoreTemplatePublic core;
    const auto ids = core.createEntities(componentIds<TrivialValueComponent>(), 2);
    RemovalReadProbeSystem system;
    core.registerSystem<RemovalReadProbeSystem, const TrivialValueComponent>(system, {});

    core.update();
    REQUIRE(system.calls == 1);
    REQUIRE(system.entity_counts.back() == 2);

    REQUIRE(core.remove(ids[0]));
    core.update();
    REQUIRE(system.calls == 2);
    REQUIRE(system.entity_counts.back() == 1);
    core.clearEntities();
}

TEST_CASE("ECS prepares worker dependencies on the owner thread before scheduling",
          "[ecs][module-boundary]") {
    FastModuleContainer modules;
    registerComponents<PrepareProbeComponent>();
    ECSCoreTemplatePublic core;
    PrepareProbeSystem system;
    core.registerSystem<PrepareProbeSystem, PrepareProbeComponent>(system, {}, true);
    const auto entity = core.createEntity(componentIds<PrepareProbeComponent>());
    const auto owner_thread = std::this_thread::get_id();

    core.update();

    REQUIRE(system.prepare_thread == owner_thread);
    REQUIRE(system.process_thread != std::thread::id{});
    REQUIRE(system.saw_matching_chunks);
    REQUIRE(system.process_saw_dependency);
    REQUIRE(core.component<PrepareProbeComponent>(entity).value == 42);
    core.clearEntities();
}

TEST_CASE("Runtime teardown precedes module destruction on normal and exceptional exits", "[ecs][lifecycle]") {
    auto exercise = [](bool inject_exception) {
        TeardownComponent::events.clear();
        try {
            FastModuleContainer modules;
            RuntimeTeardownGuard teardown;
            registerComponents<TeardownComponent>();
            (void)GET_MODULE(ECSCore).createEntity(componentIds<TeardownComponent>());
            (void)GET_MODULE(TeardownMarker);
            if (inject_exception) {
                throw std::runtime_error("loop failure");
            }
            teardown.run();
        } catch (const std::runtime_error &) {
        }
        return TeardownComponent::events;
    };

    const std::vector<std::string> expected{"deinit", "destroy", "module destroy"};
    REQUIRE(exercise(false) == expected);
    REQUIRE(exercise(true) == expected);
}

} // namespace Pelican
