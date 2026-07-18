#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"
#include "../src/core/userpublic/details/event/registerer.hpp"
#include "../src/core/userpublic/details/system/registerer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <string>
#include <tuple>
#include <typeindex>
#include <vector>

namespace Pelican {
namespace {

using Catch::Matchers::ContainsSubstring;

struct TokenComponent {
    std::int32_t value = 0;
};
struct ValidLimitComponent {};
struct DuplicateIdComponent {};
struct DuplicateNameComponent {};
struct OverLimitComponent {};

struct TokenEvent {};
struct NextGenerationEvent {};
struct StaleGenerationEvent {};

struct TokenGameSystem {
    void update(GameContext &) {}
};

struct DependentECSSystem {
    void process(std::tuple<TokenComponent *>, std::size_t) {}
};

struct TokenBehaviorParams {
    std::int32_t value = 0;

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&TokenBehaviorParams::value>("value"), std::int32_t{0}));
};

class TokenBehavior final : public Behavior {
  public:
    using Params = TokenBehaviorParams;
};

std::vector<internal::GameSystemEventHandlerRegistration> systemEventDependency() {
    return {{.event_type = std::type_index{typeid(TokenEvent)},
             .dispatch = nullptr}};
}

std::vector<internal::BehaviorEventHandlerRegistration> behaviorEventDependency() {
    return {{.event_type = std::type_index{typeid(TokenEvent)},
             .dispatch = nullptr}};
}

} // namespace

DECLARE_COMPONENT(TokenComponent, 52);
DECLARE_COMPONENT(ValidLimitComponent, 60);
DECLARE_COMPONENT(DuplicateIdComponent, 60);
DECLARE_COMPONENT(DuplicateNameComponent, 61);
DECLARE_COMPONENT(OverLimitComponent, 64);

TEST_CASE("Registration tokens drive deterministic owner purge",
          "[registration][token][owner]") {
    FastModuleContainer modules;
    const auto owner = internal::allocateRegistrationOwner();

    internal::RegistrationToken component_token;
    internal::RegistrationToken event_token;
    internal::RegistrationToken system_token;
    internal::RegistrationToken behavior_token;
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        component_token = internal::getComponentRegisterer()
                              .registerComponent<TokenComponent>(
                                  "wp163_token_component");
        event_token = internal::getEventRegisterer().registerEvent<TokenEvent>(
            "wp163_token_event");
        system_token = internal::getGameSystemRegisterer()
                           .registerSystem<TokenGameSystem>(
                               "Wp163TokenSystem", 10, systemEventDependency());
        behavior_token = internal::getBehaviorRegisterer()
                             .registerBehavior<TokenBehavior>(
                                 "wp163_token_behavior", 1,
                                 behaviorEventDependency());
    }

    REQUIRE(static_cast<bool>(component_token));
    REQUIRE(static_cast<bool>(event_token));
    REQUIRE(static_cast<bool>(system_token));
    REQUIRE(static_cast<bool>(behavior_token));
    REQUIRE(internal::componentRegistrationCount(owner) == 1);
    REQUIRE(internal::eventRegistrationCount(owner) == 1);
    REQUIRE(internal::gameSystemRegistrationCount(owner) == 1);
    REQUIRE(internal::behaviorRegistrationCount(owner) == 1);

    internal::releaseGameLogicRegistrations(owner);

    CHECK(internal::componentRegistrationCount(owner) == 0);
    CHECK(internal::eventRegistrationCount(owner) == 0);
    CHECK(internal::gameSystemRegistrationCount(owner) == 0);
    CHECK(internal::behaviorRegistrationCount(owner) == 0);
    CHECK_FALSE(internal::isRegistrationOwnerCurrent(owner));
    CHECK_THROWS_WITH(internal::unregisterComponent(component_token),
                      "stale component registration token");
    CHECK_THROWS_WITH(internal::unregisterEvent(event_token),
                      "stale event registration token");
    CHECK_THROWS_WITH(internal::unregisterGameSystem(system_token),
                      "stale game system registration token");
    CHECK_THROWS_WITH(internal::unregisterBehavior(behavior_token),
                      "stale behavior registration token");
}

TEST_CASE("Registration dependencies reject event and component removal",
          "[registration][dependent]") {
    FastModuleContainer modules;
    const auto owner = internal::allocateRegistrationOwner();

    internal::RegistrationToken component_token;
    internal::RegistrationToken event_token;
    internal::RegistrationToken system_token;
    internal::RegistrationToken behavior_token;
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        component_token = internal::getComponentRegisterer()
                              .registerComponent<TokenComponent>(
                                  "wp163_dependent_component");
        event_token = internal::getEventRegisterer().registerEvent<TokenEvent>(
            "wp163_dependent_event");
        system_token = internal::getGameSystemRegisterer()
                           .registerSystem<TokenGameSystem>(
                               "Wp163DependentSystem", 10,
                               systemEventDependency());
        behavior_token = internal::getBehaviorRegisterer()
                             .registerBehavior<TokenBehavior>(
                                 "wp163_dependent_behavior", 1,
                                 behaviorEventDependency());
    }

    REQUIRE_THROWS_WITH(
        internal::unregisterEvent(event_token),
        ContainsSubstring("wp163_dependent_event") &&
            ContainsSubstring("Wp163DependentSystem"));
    internal::unregisterGameSystem(system_token);
    REQUIRE_THROWS_WITH(
        internal::unregisterEvent(event_token),
        ContainsSubstring("wp163_dependent_event") &&
            ContainsSubstring("wp163_dependent_behavior"));
    internal::unregisterBehavior(behavior_token);
    internal::unregisterEvent(event_token);

    ECSCoreTemplatePublic core;
    DependentECSSystem dependent;
    const auto system_id =
        core.registerSystem<DependentECSSystem, TokenComponent>(dependent, {}, true);
    REQUIRE_THROWS_WITH(
        internal::unregisterComponent(component_token),
        ContainsSubstring("wp163_dependent_component") &&
            ContainsSubstring("DependentECSSystem"));
    core.unregisterSystem(system_id);
    internal::unregisterComponent(component_token);
    internal::releaseRegistrationOwner(owner);
    CHECK_FALSE(internal::isRegistrationOwnerCurrent(owner));
}

TEST_CASE("Registration owner slot reuse rejects a stale generation",
          "[registration][owner][generation]") {
    const auto first = internal::allocateRegistrationOwner();
    internal::RegistrationToken first_token;
    {
        internal::ScopedRegistrationOwner owner_scope{first};
        first_token = internal::getEventRegisterer().registerEvent<TokenEvent>(
            "wp163_first_generation_event");
    }
    internal::unregisterEvent(first_token);
    internal::releaseRegistrationOwner(first);

    const auto second = internal::allocateRegistrationOwner();
    REQUIRE(internal::registrationOwnerIdentity(first) ==
            internal::registrationOwnerIdentity(second));
    REQUIRE(internal::registrationOwnerGeneration(first) !=
            internal::registrationOwnerGeneration(second));

    internal::RegistrationToken second_token;
    {
        internal::ScopedRegistrationOwner owner_scope{second};
        second_token = internal::getEventRegisterer()
                           .registerEvent<NextGenerationEvent>(
                               "wp163_second_generation_event");
    }
    internal::unregisterEvents(first);
    REQUIRE(internal::eventRegistrationCount(second) == 1);

    {
        internal::ScopedRegistrationOwner stale_scope{first};
        REQUIRE_THROWS_WITH(
            internal::getEventRegisterer().registerEvent<StaleGenerationEvent>(
                "wp163_stale_generation_event"),
            ContainsSubstring("stale registration owner") &&
                ContainsSubstring("wp163_stale_generation_event"));
    }

    internal::unregisterEvent(second_token);
    internal::releaseRegistrationOwner(second);
}

TEST_CASE("Component registration rejects capacity and duplicate aliases",
          "[registration][component]") {
    FastModuleContainer modules;
    auto &registerer = internal::getComponentRegisterer();
    const auto valid = registerer.registerComponent<ValidLimitComponent>(
        "wp163_valid_component");

    REQUIRE_THROWS_WITH(
        registerer.registerComponent<DuplicateIdComponent>(
            "wp163_duplicate_id"),
        ContainsSubstring("wp163_duplicate_id") &&
            ContainsSubstring("duplicates id 60") &&
            ContainsSubstring("wp163_valid_component"));
    REQUIRE_THROWS_WITH(
        registerer.registerComponent<DuplicateNameComponent>(
            "wp163_valid_component"),
        ContainsSubstring("wp163_valid_component") &&
            ContainsSubstring("duplicates registered id 60") &&
            ContainsSubstring("incoming id is 61"));
    REQUIRE_THROWS_WITH(
        registerer.registerComponent<OverLimitComponent>(
            "wp163_over_limit_component"),
        ContainsSubstring("wp163_over_limit_component") &&
            ContainsSubstring("id 64") && ContainsSubstring("<64"));

    internal::unregisterComponent(valid);
}

} // namespace Pelican
