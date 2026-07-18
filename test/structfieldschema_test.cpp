#include "../src/core/userpublic/details/event/registerer.hpp"
#include "../src/core/userpublic/details/schema/structfieldjson.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

using namespace Pelican;
using namespace Pelican::internal;

namespace {

using Json = nlohmann::json;

enum class MotionMode { Idle, Run };

struct BehaviorParamsFixture {
    bool enabled = false;
    MotionMode mode = MotionMode::Idle;
    float speed = -1.0f;
    std::string label = "constructor";

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&BehaviorParamsFixture::enabled>("enabled"), true),
        defaulted(field<&BehaviorParamsFixture::mode>("mode"), MotionMode::Idle,
                  enumValues(enumValue("idle", MotionMode::Idle), enumValue("run", MotionMode::Run))),
        defaulted(field<&BehaviorParamsFixture::speed>("speed", frange(0.0, 10.0), "m/s"), 2.0f),
        defaulted(field<&BehaviorParamsFixture::label>("label"), "default"));
};

struct ComponentSchemaFixture {
    float required_value = 0.0f;
    float defaulted_value = 1.0f;

    static constexpr auto schema = structFields(
        componentPolicy("fixture_component"),
        required(field<&ComponentSchemaFixture::required_value>("required_value")),
        defaulted(field<&ComponentSchemaFixture::defaulted_value>("defaulted_value"), 1.0f));
};

int event_constructor_count = 0;
int event_ref_count = 0;

struct EventRegressionFixture {
    std::int32_t first = 0;
    std::int32_t second = 0;

    EventRegressionFixture() noexcept {
        ++event_constructor_count;
    }

    static constexpr auto pelican_payload = payloadFields(
        field<&EventRegressionFixture::first>("first"),
        field<&EventRegressionFixture::second>("second"));

    template <class Archive> void ref(Archive &archive) {
        ++event_ref_count;
        archive.prop("first", first);
        archive.prop("second", second);
    }
};

struct PayloadlessRegressionFixture {};

struct OpaqueRegressionFixture {
    std::int32_t value = 0;
    template <class Archive> void ref(Archive &archive) {
        archive.prop("value", value);
    }
};

template <class Function>
void requireStructError(Function &&function, StructFieldErrorCode code, std::string_view path) {
    try {
        function();
        FAIL("expected StructFieldValidationError");
    } catch (const StructFieldValidationError &error) {
        REQUIRE(error.code() == code);
        REQUIRE(static_cast<bool>(error.path() == path));
    }
}

template <class Function>
void requireEventError(Function &&function, EventPayloadErrorCode code) {
    try {
        function();
        FAIL("expected EventPayloadValidationError");
    } catch (const EventPayloadValidationError &error) {
        REQUIRE(error.code() == code);
    }
}

} // namespace

TEST_CASE("Struct field type information is separate from all three use-site policies",
          "[struct-schema][policy]") {
    static_assert(std::same_as<typename decltype(BehaviorParamsFixture::schema)::policy_type,
                               BehaviorParamsPolicy>);
    static_assert(std::same_as<typename decltype(ComponentSchemaFixture::schema)::policy_type,
                               ComponentPolicy>);
    static_assert(std::same_as<typename decltype(EventRegressionFixture::pelican_payload)::policy_type,
                               EventPayloadPolicy>);

    REQUIRE(BehaviorParamsFixture::schema.fields[0].type == StructFieldType::Bool);
    REQUIRE(BehaviorParamsFixture::schema.fields[1].type == StructFieldType::Enum);
    REQUIRE(BehaviorParamsFixture::schema.presence[0] == StructFieldPresence::Defaulted);
    REQUIRE(static_cast<bool>(ComponentSchemaFixture::schema.policy.codecName() == "fixture_component"));
    REQUIRE(ComponentSchemaFixture::schema.presence[0] == StructFieldPresence::Required);
    REQUIRE(ComponentSchemaFixture::schema.presence[1] == StructFieldPresence::Defaulted);
    REQUIRE(EventRegressionFixture::pelican_payload.presence[0] == StructFieldPresence::Required);
    REQUIRE(EventRegressionFixture::pelican_payload.presence[1] == StructFieldPresence::Required);
}

TEST_CASE("BehaviorParams decode applies defaults and present Bool Enum and scalar keys atomically",
          "[struct-schema][behavior][atomic]") {
    BehaviorParamsFixture params;
    params.enabled = false;
    params.mode = MotionMode::Idle;
    params.speed = 9.0f;
    params.label = "old";

    decodeBehaviorParams(Json{{"mode", "run"}, {"speed", 6.0}}, params,
                         BehaviorParamsFixture::schema);
    REQUIRE(params.enabled);
    REQUIRE(params.mode == MotionMode::Run);
    REQUIRE(params.speed == 6.0f);
    REQUIRE(params.label == "default");

    const auto first = encodeBehaviorParams(params, BehaviorParamsFixture::schema);
    const auto second = encodeBehaviorParams(params, BehaviorParamsFixture::schema);
    REQUIRE(first.dump() == second.dump());
    REQUIRE(first.dump().find("enabled") < first.dump().find("mode"));
    REQUIRE(first.dump().find("mode") < first.dump().find("speed"));
    REQUIRE(first.dump().find("speed") < first.dump().find("label"));

    BehaviorParamsFixture roundtrip;
    decodeBehaviorParams(Json::parse(first.dump()), roundtrip, BehaviorParamsFixture::schema);
    REQUIRE(roundtrip.enabled == params.enabled);
    REQUIRE(roundtrip.mode == params.mode);
    REQUIRE(roundtrip.speed == params.speed);
    REQUIRE(roundtrip.label == params.label);
}

TEST_CASE("BehaviorParams validation reports stable code and path without partial publication",
          "[struct-schema][behavior][atomic][errors]") {
    BehaviorParamsFixture params;
    params.enabled = false;
    params.mode = MotionMode::Run;
    params.speed = 4.0f;
    params.label = "unchanged";

    const auto require_unchanged = [&] {
        REQUIRE_FALSE(params.enabled);
        REQUIRE(params.mode == MotionMode::Run);
        REQUIRE(params.speed == 4.0f);
        REQUIRE(params.label == "unchanged");
    };

    requireStructError(
        [&] {
            decodeBehaviorParams(Json{{"enabled", true}, {"speed", 11.0}}, params,
                                 BehaviorParamsFixture::schema);
        },
        StructFieldErrorCode::OutOfRange, "params.speed");
    require_unchanged();

    requireStructError(
        [&] {
            decodeBehaviorParams(Json{{"enabled", true}, {"mode", "fly"}}, params,
                                 BehaviorParamsFixture::schema);
        },
        StructFieldErrorCode::InvalidEnum, "params.mode");
    require_unchanged();

    requireStructError(
        [&] {
            decodeBehaviorParams(Json{{"unknown", 1}, {"speed", 11.0}}, params,
                                 BehaviorParamsFixture::schema);
        },
        StructFieldErrorCode::UnknownField, "params.unknown");
    require_unchanged();

    requireStructError(
        [&] { decodeBehaviorParams(Json::array(), params, BehaviorParamsFixture::schema); },
        StructFieldErrorCode::PayloadMustBeObject, "params");
    require_unchanged();

    params.speed = 11.0f;
    requireStructError([&] { (void)encodeBehaviorParams(params, BehaviorParamsFixture::schema); },
                       StructFieldErrorCode::OutOfRange, "params.speed");
}

TEST_CASE("EventPayload compatibility keeps unknown-before-missing and one loader call per accepted payload",
          "[struct-schema][event][regression]") {
    event_constructor_count = 0;
    event_ref_count = 0;
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<EventRegressionFixture>("Typed");
    registry.registerEvent<PayloadlessRegressionFixture>("Payloadless");
    registry.registerEvent<OpaqueRegressionFixture>("Opaque");

    REQUIRE(registry.findEventSchema("Unknown").state == EventSchemaState::UnknownEvent);
    REQUIRE(registry.findEventSchema("Typed").state == EventSchemaState::Typed);
    REQUIRE(registry.findEventSchema("Payloadless").state == EventSchemaState::Payloadless);
    REQUIRE(registry.findEventSchema("Opaque").state == EventSchemaState::Opaque);
    REQUIRE(event_constructor_count == 0);
    REQUIRE(event_ref_count == 0);
    REQUIRE(registry.payloadLoadCallCount() == 0);

    const Json missing{{"first", 1}};
    requireEventError([&] { registry.emitByName("Typed", &missing); },
                      EventPayloadErrorCode::MissingField);
    const Json unknown_and_missing{{"first", 1}, {"extra", 2}};
    requireEventError([&] { registry.emitByName("Typed", &unknown_and_missing); },
                      EventPayloadErrorCode::UnknownField);
    REQUIRE(event_constructor_count == 0);
    REQUIRE(event_ref_count == 0);
    REQUIRE(registry.payloadLoadCallCount() == 0);
    REQUIRE(registry.pendingEventCount() == 0);

    const Json valid{{"first", 1}, {"second", 2}};
    REQUIRE(registry.emitByName("Typed", &valid) == 1);
    REQUIRE(event_constructor_count == 1);
    REQUIRE(event_ref_count == 1);
    REQUIRE(registry.payloadLoadCallCount() == 1);
    REQUIRE(registry.pendingEventCount() == 1);

    REQUIRE(registry.emitByName("Typed", &valid) == 1);
    REQUIRE(event_constructor_count == 2);
    REQUIRE(event_ref_count == 2);
    REQUIRE(registry.payloadLoadCallCount() == 2);
    REQUIRE(registry.pendingEventCount() == 2);
    REQUIRE_NOTHROW(registry.validateCatalogAndFreeze());
}
