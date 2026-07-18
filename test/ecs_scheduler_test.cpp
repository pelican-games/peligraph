#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <string>
#include <tuple>
#include <vector>

namespace Pelican {
namespace {

using Catch::Matchers::ContainsSubstring;

struct SchedulerProbeComponent {
    int value = 0;
};

struct SchedulerWriter {
    std::vector<std::string> *events = nullptr;

    void process(std::tuple<SchedulerProbeComponent *> components, size_t count) {
        events->push_back("writer");
        if (count != 0) {
            std::get<0>(components)[0].value = 42;
        }
    }
};

struct SchedulerReader {
    std::vector<std::string> *events = nullptr;
    bool saw_write = false;

    void process(std::tuple<const SchedulerProbeComponent *> components, size_t count) {
        events->push_back("reader");
        saw_write = count != 0 && std::get<0>(components)[0].value == 42;
    }
};

internal::ECSSystemGraphNode graphNode(
    SystemId id, std::string name, std::vector<SystemId> dependencies,
    std::initializer_list<internal::ECSSystemComponentAccess> accesses) {
    return internal::ECSSystemGraphNode{
        .id = id,
        .name = std::move(name),
        .dependencies = std::move(dependencies),
        .component_accesses = accesses,
    };
}

} // namespace

DECLARE_COMPONENT(SchedulerProbeComponent, 49);

namespace {

void registerSchedulerComponents() {
    static const bool logger_initialized = [] {
        setupLogger();
        return true;
    }();
    (void)logger_initialized;
    auto &registerer = internal::getComponentRegisterer();
    registerer.registerComponent<EntityId>("eid");
    registerer.registerComponent<SchedulerProbeComponent>("scheduler_probe");
}

TEST_CASE("ECS scheduler keeps read read systems parallel", "[ecs][scheduler][hazard]") {
    const std::array nodes{
        graphNode(1, "ReaderA", {}, {{7, "position", false}}),
        graphNode(2, "ReaderB", {}, {{7, "position", false}}),
    };

    const auto plan = internal::buildECSExecutionPlan(
        nodes, internal::ECSHazardPolicy::strict);
    REQUIRE(plan.levels == std::vector<std::vector<SystemId>>{{1, 2}});
    REQUIRE(plan.automatic_serializations.empty());
}

TEST_CASE("ECS scheduler accepts hazards ordered by a transitive dependency",
          "[ecs][scheduler][hazard]") {
    const std::array nodes{
        graphNode(1, "Writer", {}, {{7, "position", true}}),
        graphNode(2, "Middle", {1}, {}),
        graphNode(3, "Reader", {2}, {{7, "position", false}}),
    };

    const auto plan = internal::buildECSExecutionPlan(
        nodes, internal::ECSHazardPolicy::strict);
    REQUIRE(plan.levels == std::vector<std::vector<SystemId>>{{1}, {2}, {3}});
}

TEST_CASE("ECS strict hazard error names both systems and the component",
          "[ecs][scheduler][hazard]") {
    const std::array nodes{
        graphNode(1, "Writer", {}, {{7, "position", true}}),
        graphNode(2, "Reader", {}, {{7, "position", false}}),
    };

    REQUIRE_THROWS_WITH(
        internal::buildECSExecutionPlan(nodes, internal::ECSHazardPolicy::strict),
        ContainsSubstring("Writer") && ContainsSubstring("Reader") &&
            ContainsSubstring("position"));
}

TEST_CASE("ECS auto serialization uses registration order", "[ecs][scheduler][hazard]") {
    const std::array nodes{
        graphNode(10, "Writer", {}, {{7, "position", true}}),
        graphNode(20, "Reader", {}, {{7, "position", false}}),
    };

    const auto plan = internal::buildECSExecutionPlan(
        nodes, internal::ECSHazardPolicy::automatic_serialization);
    REQUIRE(plan.levels == std::vector<std::vector<SystemId>>{{10}, {20}});
    REQUIRE(plan.automatic_serializations.size() == 1);
    CHECK(plan.automatic_serializations[0].before == 10);
    CHECK(plan.automatic_serializations[0].after == 20);
    CHECK(plan.automatic_serializations[0].component_names ==
          std::vector<std::string>{"position"});
}

TEST_CASE("ECS scheduler rejects missing dependencies", "[ecs][scheduler][missing]") {
    const std::array nodes{
        graphNode(1, "Orphan", {99}, {}),
    };

    REQUIRE_THROWS_WITH(
        internal::buildECSExecutionPlan(nodes, internal::ECSHazardPolicy::strict),
        ContainsSubstring("Orphan") && ContainsSubstring("missing system id 99") &&
            ContainsSubstring("unexecuted"));
}

TEST_CASE("ECS scheduler rejects dependency cycles and names unexecuted nodes",
          "[ecs][scheduler][cycle]") {
    const std::array nodes{
        graphNode(1, "CycleA", {2}, {}),
        graphNode(2, "CycleB", {1}, {}),
    };

    REQUIRE_THROWS_WITH(
        internal::buildECSExecutionPlan(nodes, internal::ECSHazardPolicy::strict),
        ContainsSubstring("cycle") && ContainsSubstring("unexecuted systems") &&
            ContainsSubstring("CycleA") && ContainsSubstring("CycleB"));
}

TEST_CASE("ECS scheduler rejects duplicate dependency unexecuted fixtures",
          "[ecs][scheduler][unexecuted]") {
    const std::array nodes{
        graphNode(1, "Producer", {}, {}),
        graphNode(2, "NeverRun", {1, 1}, {}),
    };

    REQUIRE_THROWS_WITH(
        internal::buildECSExecutionPlan(nodes, internal::ECSHazardPolicy::strict),
        ContainsSubstring("NeverRun") && ContainsSubstring("more than once") &&
            ContainsSubstring("unexecuted"));
}

TEST_CASE("ECS runtime applies auto serialization before worker scheduling",
          "[ecs][scheduler][hazard]") {
    FastModuleContainer modules;
    registerSchedulerComponents();
    GET_MODULE(EngineLaunchConfig).strict_assets = false;

    ECSCoreTemplatePublic core;
    std::vector<std::string> events;
    SchedulerWriter writer{&events};
    SchedulerReader reader{&events};
    core.registerSystem<SchedulerWriter, SchedulerProbeComponent>(writer, {}, true);
    core.registerSystem<SchedulerReader, const SchedulerProbeComponent>(reader, {}, true);
    const std::array components{ComponentIdByType<SchedulerProbeComponent>::value};
    (void)core.createEntity(components);

    core.update();

    CHECK(events == std::vector<std::string>{"writer", "reader"});
    CHECK(reader.saw_write);
    core.clearEntities();
}

TEST_CASE("ECS runtime strict launch rejects an unordered hazard",
          "[ecs][scheduler][hazard][strict]") {
    FastModuleContainer modules;
    registerSchedulerComponents();
    GET_MODULE(EngineLaunchConfig).strict_assets = true;

    ECSCoreTemplatePublic core;
    std::vector<std::string> events;
    SchedulerWriter writer{&events};
    SchedulerReader reader{&events};
    core.registerSystem<SchedulerWriter, SchedulerProbeComponent>(writer, {}, true);
    core.registerSystem<SchedulerReader, const SchedulerProbeComponent>(reader, {}, true);

    REQUIRE_THROWS_WITH(core.update(),
                        ContainsSubstring("SchedulerWriter") &&
                            ContainsSubstring("SchedulerReader") &&
                            ContainsSubstring("scheduler_probe"));
}

TEST_CASE("ECS runtime rejects a dependency removed before update",
          "[ecs][scheduler][missing][unexecuted]") {
    FastModuleContainer modules;
    registerSchedulerComponents();
    GET_MODULE(EngineLaunchConfig).strict_assets = false;

    ECSCoreTemplatePublic core;
    std::vector<std::string> events;
    SchedulerWriter producer{&events};
    SchedulerReader consumer{&events};
    const auto producer_id =
        core.registerSystem<SchedulerWriter, SchedulerProbeComponent>(producer, {}, true);
    core.registerSystem<SchedulerReader, const SchedulerProbeComponent>(
        consumer, {producer_id}, true);
    core.unregisterSystem(producer_id);

    REQUIRE_THROWS_WITH(core.update(),
                        ContainsSubstring("SchedulerReader") &&
                            ContainsSubstring("missing system id") &&
                            ContainsSubstring("unexecuted"));
}

} // namespace
} // namespace Pelican
