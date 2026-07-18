#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/details/system/registerer.hpp"
#include "../src/core/userpublic/gameobjects.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {
namespace {

std::vector<std::string> trace;
bool order_probe_enabled = false;
int declaration_only_constructor_count = 0;

struct BehaviorProbeEvent {
    std::int32_t value = 0;
};

struct OtherBehaviorProbeEvent {};

struct OwnerQueuedEvent {};

} // namespace

PELICAN_REGISTER_EVENT(BehaviorProbeEvent);
PELICAN_REGISTER_EVENT(OtherBehaviorProbeEvent);

namespace {

struct ABehaviorOrderSystem {
    void update(GameContext &) {
        if (order_probe_enabled) trace.push_back("A:update");
    }
    void onEvent(const BehaviorProbeEvent &, GameContext &) {
        if (order_probe_enabled) trace.push_back("A:event");
    }
};

struct ZBehaviorOrderSystem {
    void update(GameContext &) {
        if (order_probe_enabled) trace.push_back("Z:update");
    }
    void onEvent(const BehaviorProbeEvent &, GameContext &) {
        if (order_probe_enabled) trace.push_back("Z:event");
    }
};

PELICAN_REGISTER_SYSTEM(ABehaviorOrderSystem, 50);
PELICAN_REGISTER_SYSTEM(ZBehaviorOrderSystem, 50);

struct LabelParams {
    std::string label;

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&LabelParams::label>("label"), "default"));
};

class DeclarationOnlyBehavior final : public Behavior {
  public:
    using Params = LabelParams;
    DeclarationOnlyBehavior() { ++declaration_only_constructor_count; }
};

class OrderBehavior final : public Behavior {
  public:
    using Params = LabelParams;

    void onInit(BehaviorContext &ctx) override {
        trace.push_back("init:" + ctx.params<Params>().label);
    }
    void onUpdate(BehaviorContext &ctx) override {
        trace.push_back("B:update:" + ctx.params<Params>().label);
    }
    void onEvent(const BehaviorProbeEvent &, BehaviorContext &ctx) {
        trace.push_back("B:event:" + ctx.params<Params>().label);
    }
    void onDestroy(BehaviorContext &ctx) noexcept override {
        const auto alive = GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(ctx.self());
        trace.push_back("destroy:" + ctx.params<Params>().label + (alive ? ":alive" : ":dead"));
    }
};

class GoodInitBehavior final : public Behavior {
  public:
    using Params = LabelParams;
    void onInit(BehaviorContext &) override { trace.push_back("good:init"); }
    void onDestroy(BehaviorContext &) noexcept override { trace.push_back("good:destroy"); }
};

class ThrowInitBehavior final : public Behavior {
  public:
    using Params = LabelParams;
    void onInit(BehaviorContext &) override {
        trace.push_back("throw:init");
        throw std::runtime_error("wp155 activation fault");
    }
    void onDestroy(BehaviorContext &) noexcept override { trace.push_back("throw:destroy"); }
};

class RemoveSelfBehavior final : public Behavior {
  public:
    using Params = LabelParams;
    void onUpdate(BehaviorContext &ctx) override {
        trace.push_back("remove:update:" + ctx.params<Params>().label);
        REQUIRE(ctx.removeObject(ctx.self()));
    }
    void onDestroy(BehaviorContext &ctx) noexcept override {
        trace.push_back("remove:destroy:" + ctx.params<Params>().label);
    }
};

class OwnerBehavior final : public Behavior {
  public:
    using Params = LabelParams;
    void onInit(BehaviorContext &) override { trace.push_back("owner:init"); }
    void onDestroy(BehaviorContext &) noexcept override { trace.push_back("owner:destroy"); }
};

enum class ScalarMode { Idle, Run };

struct IntegerScalarParams {
    std::int8_t i8 = 0;
    std::int16_t i16 = 0;
    std::int32_t i32 = 0;
    std::int64_t i64 = 0;
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&IntegerScalarParams::i8>("i8"), std::int8_t{-8}),
        defaulted(field<&IntegerScalarParams::i16>("i16"), std::int16_t{-16}),
        defaulted(field<&IntegerScalarParams::i32>("i32"), std::int32_t{-32}),
        defaulted(field<&IntegerScalarParams::i64>("i64"), std::int64_t{-64}),
        defaulted(field<&IntegerScalarParams::u8>("u8"), std::uint8_t{8}),
        defaulted(field<&IntegerScalarParams::u16>("u16"), std::uint16_t{16}),
        defaulted(field<&IntegerScalarParams::u32>("u32"), std::uint32_t{32}),
        defaulted(field<&IntegerScalarParams::u64>("u64"), std::uint64_t{64}));
};

struct FloatingScalarParams {
    float f32 = 0.0f;
    double f64 = 0.0;

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&FloatingScalarParams::f32>("f32"), 1.25f),
        defaulted(field<&FloatingScalarParams::f64>("f64"), 2.5));
};

struct OtherScalarParams {
    std::string text;
    bool enabled = false;
    ScalarMode mode = ScalarMode::Idle;

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&OtherScalarParams::text>("text"), "defaults"),
        defaulted(field<&OtherScalarParams::enabled>("enabled"), true),
        defaulted(field<&OtherScalarParams::mode>("mode"), ScalarMode::Idle,
                  enumValues(enumValue("idle", ScalarMode::Idle),
                             enumValue("run", ScalarMode::Run))));
};

template <class ParamsType> class ScalarBehavior final : public Behavior {
  public:
    using Params = ParamsType;
};

using IntegerScalarBehavior = ScalarBehavior<IntegerScalarParams>;
using FloatingScalarBehavior = ScalarBehavior<FloatingScalarParams>;
using OtherScalarBehavior = ScalarBehavior<OtherScalarParams>;

PELICAN_REGISTER_BEHAVIOR(DeclarationOnlyBehavior, "wp155_declaration_only", 1);
PELICAN_REGISTER_BEHAVIOR(OrderBehavior, "wp155_order", 1);
PELICAN_REGISTER_BEHAVIOR(GoodInitBehavior, "wp155_good_init", 1);
PELICAN_REGISTER_BEHAVIOR(ThrowInitBehavior, "wp155_throw_init", 1);
PELICAN_REGISTER_BEHAVIOR(RemoveSelfBehavior, "wp155_remove_self", 1);
PELICAN_REGISTER_BEHAVIOR(IntegerScalarBehavior, "wp155_integer_scalar", 1);
PELICAN_REGISTER_BEHAVIOR(FloatingScalarBehavior, "wp155_floating_scalar", 1);
PELICAN_REGISTER_BEHAVIOR(OtherScalarBehavior, "wp155_other_scalar", 1);

GameObjectId createEmptyEntity() {
    return GameObjects::createWithComponents(std::span<const ComponentId>{}, {});
}

std::vector<BoundSceneBehaviorAttachment> bindPrepared(
    std::vector<PreparedSceneBehaviorAttachment> prepared) {
    std::vector<GameObjectId> object_entities;
    std::vector<BoundSceneBehaviorAttachment> bound;
    for (auto &attachment : prepared) {
        if (object_entities.size() <= attachment.object_index) {
            object_entities.resize(attachment.object_index + 1, invalidGameObjectId);
        }
        auto &entity = object_entities[attachment.object_index];
        if (entity == invalidGameObjectId) entity = createEmptyEntity();
        bound.push_back({.prepared = std::move(attachment), .entity = entity});
    }
    return bound;
}

nlohmann::json behaviorObjects(std::string_view first_type,
                               std::string_view second_type = {}) {
    nlohmann::json components = nlohmann::json::array(
        {{{"name", "behavior"}, {"type", first_type}, {"params", {{"label", "one"}}}}});
    if (!second_type.empty()) {
        components.push_back({{"name", "behavior"},
                              {"type", second_type},
                              {"params", {{"label", "two"}}}});
    }
    return nlohmann::json::array({{{"name", "BehaviorOnly"}, {"components", components}}});
}

void initializeBehaviorTestModules() {
    GET_MODULE(ECSPredefinedRegistration).reg();
    (void)GET_MODULE(BehaviorAttachmentArena);
}

} // namespace

TEST_CASE("Behavior registration is declaration-only and params omission applies every default",
          "[behavior][registration][params]") {
    REQUIRE(declaration_only_constructor_count == 0);
    const auto *registration =
        internal::getBehaviorRegisterer().findByName("wp155_declaration_only");
    REQUIRE(registration != nullptr);
    REQUIRE(registration->schema_version == 1);
    REQUIRE_FALSE(registration->params_schema.empty());
    REQUIRE(internal::canonicalizeBehaviorParams("wp155_declaration_only",
                                                 nlohmann::json::object()) ==
            R"({"label":"default"})");
    REQUIRE(declaration_only_constructor_count == 0);

    FastModuleContainer modules;
    initializeBehaviorTestModules();
    auto prepared = prepareSceneBehaviorAttachments(
        behaviorObjects("wp155_declaration_only"), BehaviorRegistryAvailability::active);
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(bindPrepared(std::move(prepared)));
    arena.activatePublished();
    REQUIRE(declaration_only_constructor_count == 1);
    arena.deactivateAll();
}

TEST_CASE("Behavior params canonical roundtrip covers every supported scalar kind",
          "[behavior][params][roundtrip]") {
    const auto integer_defaults = internal::canonicalizeBehaviorParams(
        "wp155_integer_scalar", nlohmann::json::object());
    const auto floating_defaults = internal::canonicalizeBehaviorParams(
        "wp155_floating_scalar", nlohmann::json::object());
    const auto other_defaults = internal::canonicalizeBehaviorParams(
        "wp155_other_scalar", nlohmann::json::object());
    REQUIRE(nlohmann::json::parse(integer_defaults).at("i8") == -8);
    REQUIRE(nlohmann::json::parse(integer_defaults).at("u64") == 64);
    REQUIRE(nlohmann::json::parse(other_defaults).at("enabled") == true);
    REQUIRE(nlohmann::json::parse(other_defaults).at("mode") == "idle");
    REQUIRE(nlohmann::json::parse(floating_defaults).at("f32") == 1.25);

    const nlohmann::json integer_authored{
        {"i8", -7}, {"i16", -15}, {"i32", -31}, {"i64", -63},
        {"u8", 7}, {"u16", 15}, {"u32", 31}, {"u64", 63}};
    const nlohmann::json floating_authored{{"f32", 3.5}, {"f64", 7.25}};
    const nlohmann::json other_authored{
        {"text", "roundtrip"}, {"enabled", false}, {"mode", "run"}};
    for (const auto &[name, authored] :
         std::vector<std::pair<std::string, nlohmann::json>>{
             {"wp155_integer_scalar", integer_authored},
             {"wp155_floating_scalar", floating_authored},
             {"wp155_other_scalar", other_authored}}) {
        const auto first = internal::canonicalizeBehaviorParams(name, authored);
        const auto second = internal::canonicalizeBehaviorParams(
            name, nlohmann::json::parse(first));
        REQUIRE(first == second);
    }
    REQUIRE(integer_defaults.find("\"i8\"") < integer_defaults.find("\"i16\""));
    REQUIRE(other_defaults.find("\"text\"") < other_defaults.find("\"enabled\""));
}

TEST_CASE("Scene behavior special binder preserves tuple order and separates unknown states",
          "[behavior][scene-binder]") {
    const auto objects = behaviorObjects("wp155_order", "wp155_order");
    const auto prepared = prepareSceneBehaviorAttachments(
        objects, BehaviorRegistryAvailability::active);
    REQUIRE(prepared.size() == 2);
    REQUIRE(prepared[0].object_index == 0);
    REQUIRE(prepared[0].component_index == 0);
    REQUIRE(prepared[1].component_index == 1);
    REQUIRE(sceneBehaviorAttachmentSeq(0, 0) < sceneBehaviorAttachmentSeq(0, 1));

    REQUIRE_THROWS_WITH(
        prepareSceneBehaviorAttachments(behaviorObjects("missing_behavior"),
                                        BehaviorRegistryAvailability::active),
        "Unknown behavior type 'missing_behavior' on object 'BehaviorOnly'");
    const auto pending = prepareSceneBehaviorAttachments(
        behaviorObjects("missing_behavior"), BehaviorRegistryAvailability::dll_unavailable);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].pending);
    REQUIRE(pending[0].raw_component.at("type") == "missing_behavior");

    FastModuleContainer modules;
    initializeBehaviorTestModules();
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(bindPrepared(pending));
    arena.activatePublished();
    const auto snapshot = arena.snapshot();
    REQUIRE(snapshot.size() == 1);
    REQUIRE(snapshot[0].pending);
    REQUIRE_FALSE(snapshot[0].active);
    REQUIRE(GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(snapshot[0].entity));
    arena.deactivateAll();
}

TEST_CASE("BehaviorSystem literal order and name fix event and update total order",
          "[behavior][order][event]") {
    static_assert(behaviorSystemOrder == 50);
    static_assert(behaviorSystemName == std::string_view{"BehaviorSystem"});

    FastModuleContainer modules;
    initializeBehaviorTestModules();
    auto prepared = prepareSceneBehaviorAttachments(
        behaviorObjects("wp155_order", "wp155_order"),
        BehaviorRegistryAvailability::active);
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(bindPrepared(std::move(prepared)));
    trace.clear();
    arena.activatePublished();
    REQUIRE(trace == std::vector<std::string>{"init:one", "init:two"});

    trace.clear();
    order_probe_enabled = true;
    GameContext ctx;
    internal::dispatchEventToRegisteredGameSystems(
        internal::QueuedEvent{.type = std::type_index{typeid(BehaviorProbeEvent)},
                              .name = "BehaviorProbeEvent",
                              .payload = std::make_shared<BehaviorProbeEvent>()},
        ctx);
    REQUIRE(trace == std::vector<std::string>{"A:event", "B:event:one",
                                               "B:event:two", "Z:event"});

    trace.clear();
    internal::updateRegisteredGameSystems(ctx);
    REQUIRE(trace == std::vector<std::string>{"A:update", "B:update:one",
                                               "B:update:two", "Z:update"});

    trace.clear();
    internal::dispatchEventToRegisteredGameSystems(
        internal::QueuedEvent{.type = std::type_index{typeid(OtherBehaviorProbeEvent)},
                              .name = "OtherBehaviorProbeEvent",
                              .payload = std::make_shared<OtherBehaviorProbeEvent>()},
        ctx);
    REQUIRE(trace.empty());
    order_probe_enabled = false;
    arena.deactivateAll();
}

TEST_CASE("Behavior activation fault rolls initialized attachments back in reverse order",
          "[behavior][activation][rollback]") {
    FastModuleContainer modules;
    initializeBehaviorTestModules();
    auto prepared = prepareSceneBehaviorAttachments(
        behaviorObjects("wp155_good_init", "wp155_throw_init"),
        BehaviorRegistryAvailability::active);
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(bindPrepared(std::move(prepared)));
    trace.clear();
    REQUIRE_THROWS_WITH(arena.activatePublished(), "wp155 activation fault");
    REQUIRE(trace == std::vector<std::string>{"good:init", "throw:init", "good:destroy"});
    REQUIRE(arena.snapshot().empty());
}

TEST_CASE("Behavior pre-destroy runs in reverse attachment order while entity is live",
          "[behavior][destroy][barrier]") {
    FastModuleContainer modules;
    initializeBehaviorTestModules();
    auto prepared = prepareSceneBehaviorAttachments(
        behaviorObjects("wp155_order", "wp155_order"),
        BehaviorRegistryAvailability::active);
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(bindPrepared(std::move(prepared)));
    arena.activatePublished();
    const auto entity = arena.snapshot().front().entity;
    trace.clear();
    REQUIRE(GameObjects::remove(entity));
    REQUIRE(trace == std::vector<std::string>{"destroy:two:alive", "destroy:one:alive"});
    REQUIRE(arena.snapshot().empty());
}

TEST_CASE("Structural removal during a behavior callback waits for the next boundary",
          "[behavior][mutation][snapshot]") {
    FastModuleContainer modules;
    initializeBehaviorTestModules();
    auto prepared = prepareSceneBehaviorAttachments(
        behaviorObjects("wp155_remove_self", "wp155_remove_self"),
        BehaviorRegistryAvailability::active);
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    arena.publishSceneAttachments(bindPrepared(std::move(prepared)));
    arena.activatePublished();
    const auto entity = arena.snapshot().front().entity;
    GameContext ctx;

    trace.clear();
    arena.update(ctx);
    REQUIRE(trace == std::vector<std::string>{"remove:update:one", "remove:update:two"});
    REQUIRE(GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(entity));

    trace.clear();
    arena.update(ctx);
    REQUIRE(trace == std::vector<std::string>{"remove:destroy:two", "remove:destroy:one"});
    REQUIRE_FALSE(GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(entity));
    REQUIRE(arena.snapshot().empty());
}

TEST_CASE("Every game-logic owner cleanup branch purges registry and live instances",
          "[behavior][owner][reload]") {
    FastModuleContainer modules;
    initializeBehaviorTestModules();
    const std::vector<std::pair<std::string, bool>> branches{
        {"load_failure", false},
        {"abi_failure", false},
        {"candidate_unload", false},
        {"active_unload", true},
        {"reload_failure", true},
        {"rollback_failure", true},
    };

    for (const auto &[branch, make_live] : branches) {
        DYNAMIC_SECTION(branch) {
            const auto owner = internal::allocateRegistrationOwner();
            {
                internal::ScopedRegistrationOwner owner_scope{owner};
                internal::getBehaviorRegisterer().registerBehavior<OwnerBehavior>(
                    "wp155_owner_" + branch, 1, {});
                internal::getEventRegisterer().registerEvent<OwnerQueuedEvent>(
                    "wp155_owner_queued_event");
            }
            REQUIRE(internal::behaviorRegistrationCount(owner) == 1);
            REQUIRE(internal::eventRegistrationCount(owner) == 1);
            internal::getEventRegisterer().emit(OwnerQueuedEvent{});
            if (branch == "candidate_unload" || branch == "rollback_failure") {
                internal::freezePendingEventsForFrame();
            }
            REQUIRE(internal::getEventRegisterer().pendingEventCount() == 1);
            if (make_live) {
                auto prepared = prepareSceneBehaviorAttachments(
                    behaviorObjects("wp155_owner_" + branch),
                    BehaviorRegistryAvailability::active);
                auto &arena = GET_MODULE(BehaviorAttachmentArena);
                arena.publishSceneAttachments(bindPrepared(std::move(prepared)));
                trace.clear();
                arena.activatePublished();
                REQUIRE(arena.liveInstanceCount(owner) == 1);
            }

            internal::releaseGameLogicRegistrations(owner);
            REQUIRE(internal::behaviorRegistrationCount(owner) == 0);
            REQUIRE(internal::eventRegistrationCount(owner) == 0);
            REQUIRE(internal::getEventRegisterer().pendingEventCount() == 0);
            REQUIRE(GET_MODULE(BehaviorAttachmentArena).liveInstanceCount(owner) == 0);
            if (make_live) {
                REQUIRE(trace == std::vector<std::string>{"owner:init", "owner:destroy"});
            }
        }
    }
}

} // namespace Pelican
