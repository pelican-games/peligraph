#include "../src/core/userpublic/details/event/registerer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace {

using Pelican::EventPayloadErrorCode;
using Pelican::EventPayloadValidationError;
using Pelican::EventSchemaState;
using Pelican::PayloadFieldType;
using Pelican::internal::UserEventRegistererTemplatePublic;
using Json = nlohmann::json;

int all_fields_constructor_count = 0;
int all_fields_ref_count = 0;

template <class Fn> void requireError(Fn &&fn, EventPayloadErrorCode expected) {
    try {
        fn();
        FAIL("expected EventPayloadValidationError");
    } catch (const EventPayloadValidationError &error) {
        REQUIRE(error.code() == expected);
    }
}

struct AllFieldsEvent {
    std::int8_t i8 = 0;
    std::int16_t i16 = 0;
    std::int32_t i32 = 0;
    std::int64_t i64 = 0;
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    float f32 = 0;
    double f64 = 0;
    Pelican::vec2 vec2{};
    Pelican::vec3 vec3{};
    Pelican::vec4 vec4{};
    Pelican::quat quat{};
    std::string string;

    AllFieldsEvent() noexcept {
        ++all_fields_constructor_count;
    }

    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&AllFieldsEvent::i8>("i8", Pelican::irange(-8, 8), "count"),
        Pelican::field<&AllFieldsEvent::i16>("i16"), Pelican::field<&AllFieldsEvent::i32>("i32"),
        Pelican::field<&AllFieldsEvent::i64>("i64"),
        Pelican::field<&AllFieldsEvent::u8>("u8", Pelican::urange(1, 8)),
        Pelican::field<&AllFieldsEvent::u16>("u16"), Pelican::field<&AllFieldsEvent::u32>("u32"),
        Pelican::field<&AllFieldsEvent::u64>("u64"),
        Pelican::field<&AllFieldsEvent::f32>("f32", Pelican::frange(-2.0, 2.0), "m"),
        Pelican::field<&AllFieldsEvent::f64>("f64", Pelican::frange(-4.0, 4.0)),
        Pelican::field<&AllFieldsEvent::vec2>("vec2", Pelican::frange(-5.0, 5.0)),
        Pelican::field<&AllFieldsEvent::vec3>("vec3"), Pelican::field<&AllFieldsEvent::vec4>("vec4"),
        Pelican::field<&AllFieldsEvent::quat>("quat"), Pelican::field<&AllFieldsEvent::string>("string"));

    template <class Archive> void ref(Archive &ar) {
        ++all_fields_ref_count;
        ar.prop("i8", i8);
        ar.prop("i16", i16);
        ar.prop("i32", i32);
        ar.prop("i64", i64);
        ar.prop("u8", u8);
        ar.prop("u16", u16);
        ar.prop("u32", u32);
        ar.prop("u64", u64);
        ar.prop("f32", f32);
        ar.prop("f64", f64);
        ar.prop("vec2", vec2);
        ar.prop("vec3", vec3);
        ar.prop("vec4", vec4);
        ar.prop("quat", quat);
        ar.prop("string", string);
    }
};

struct PayloadlessEvent {};

struct OpaqueEvent {
    int value = 0;
    template <class Archive> void ref(Archive &ar) {
        ar.prop("value", value);
    }
};

struct TypedEmptyEvent {
    static constexpr auto pelican_payload = Pelican::payloadFields();
    template <class Archive> void ref(Archive &) {}
};

struct TypedValueEvent {
    std::int32_t value = 0;
    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&TypedValueEvent::value>("value", Pelican::irange(0, 10)));
    template <class Archive> void ref(Archive &ar) {
        ar.prop("value", value);
    }
};

int constructor_count = 0;
int ref_count = 0;

struct CountingEvent {
    std::int32_t value = 0;

    CountingEvent() noexcept {
        ++constructor_count;
    }

    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&CountingEvent::value>("value"));

    template <class Archive> void ref(Archive &ar) {
        ++ref_count;
        ar.prop("value", value);
    }
};

Json validAllFields() {
    return Json{{"i8", 1},       {"i16", 2},       {"i32", 3},       {"i64", 4},
                {"u8", 1},       {"u16", 2},       {"u32", 3},       {"u64", 4},
                {"f32", 1.0},    {"f64", 2.0},     {"vec2", {1.0, 2.0}},
                {"vec3", {1.0, 2.0, 3.0}},          {"vec4", {1.0, 2.0, 3.0, 4.0}},
                {"quat", {0.0, 0.0, 0.0, 1.0}},    {"string", "value"}};
}

struct MissingDescriptorField {
    int a = 0;
    int b = 0;
    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&MissingDescriptorField::a>("a"));
    template <class Archive> void ref(Archive &ar) {
        ar.prop("a", a);
        ar.prop("b", b);
    }
};

struct DescriptorOnlyField {
    int a = 0;
    int b = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&DescriptorOnlyField::a>("a"), Pelican::field<&DescriptorOnlyField::b>("b"));
    template <class Archive> void ref(Archive &ar) {
        ar.prop("a", a);
    }
};

struct RenamedField {
    int a = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(Pelican::field<&RenamedField::a>("new_name"));
    template <class Archive> void ref(Archive &ar) {
        ar.prop("old_name", a);
    }
};

struct ReorderedFields {
    int a = 0;
    int b = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&ReorderedFields::a>("a"), Pelican::field<&ReorderedFields::b>("b"));
    template <class Archive> void ref(Archive &ar) {
        ar.prop("b", b);
        ar.prop("a", a);
    }
};

struct DuplicateRefField {
    int a = 0;
    int b = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&DuplicateRefField::a>("a"), Pelican::field<&DuplicateRefField::b>("b"));
    template <class Archive> void ref(Archive &ar) {
        ar.prop("a", a);
        ar.prop("a", b);
    }
};

struct ConditionalRefKnownLimit {
    int selector = 0;
    int a = 0;
    int b = 0;
    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&ConditionalRefKnownLimit::a>("a"));
    template <class Archive> void ref(Archive &ar) {
        if (selector == 0) {
            ar.prop("a", a);
        } else {
            ar.prop("b", b);
        }
    }
};

int global_catalog_constructor_count = 0;

struct GlobalCatalogEvent {
    int value = 0;

    GlobalCatalogEvent() noexcept {
        ++global_catalog_constructor_count;
    }

    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&GlobalCatalogEvent::value>("value"));

    template <class Archive> void ref(Archive &ar) {
        ar.prop("value", value);
    }
};

} // namespace

PELICAN_REGISTER_EVENT(GlobalCatalogEvent);

TEST_CASE("Event payload descriptor owns and exposes all loader field types", "[event-schema]") {
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<AllFieldsEvent>("AllFieldsEvent");
    const auto lookup = registry.findEventSchema("AllFieldsEvent");
    REQUIRE(lookup.state == EventSchemaState::Typed);
    REQUIRE(lookup.schema != nullptr);
    REQUIRE(lookup.schema->fields.size() == 15);

    constexpr std::array expected_types{
        PayloadFieldType::I8,   PayloadFieldType::I16, PayloadFieldType::I32, PayloadFieldType::I64,
        PayloadFieldType::U8,   PayloadFieldType::U16, PayloadFieldType::U32, PayloadFieldType::U64,
        PayloadFieldType::F32,  PayloadFieldType::F64, PayloadFieldType::Vec2, PayloadFieldType::Vec3,
        PayloadFieldType::Vec4, PayloadFieldType::Quat, PayloadFieldType::String,
    };
    for (std::size_t i = 0; i < expected_types.size(); ++i) {
        REQUIRE(lookup.schema->fields[i].type == expected_types[i]);
    }
    REQUIRE(static_cast<bool>(lookup.schema->fields[0].name == "i8"));
    REQUIRE(static_cast<bool>(lookup.schema->fields[0].unit == "count"));
    REQUIRE(std::get<std::pair<std::int64_t, std::int64_t>>(lookup.schema->fields[0].range) ==
            std::pair<std::int64_t, std::int64_t>{-8, 8});
    REQUIRE(std::get<std::pair<std::uint64_t, std::uint64_t>>(lookup.schema->fields[4].range) ==
            std::pair<std::uint64_t, std::uint64_t>{1, 8});
    REQUIRE(std::get<std::pair<double, double>>(lookup.schema->fields[8].range) == std::pair{-2.0, 2.0});
    REQUIRE(static_cast<bool>(lookup.schema->fields[8].unit == "m"));
    REQUIRE(std::holds_alternative<std::monostate>(lookup.schema->fields[13].range));
}

TEST_CASE("Event schema lookup classifies all four registered states and unknown", "[event-schema][matrix]") {
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<PayloadlessEvent>("Payloadless");
    registry.registerEvent<OpaqueEvent>("Opaque");
    registry.registerEvent<TypedEmptyEvent>("TypedEmpty");
    registry.registerEvent<TypedValueEvent>("TypedValue");

    REQUIRE(registry.findEventSchema("Unknown").state == EventSchemaState::UnknownEvent);
    const auto payloadless = registry.findEventSchema("Payloadless");
    REQUIRE(payloadless.state == EventSchemaState::Payloadless);
    REQUIRE(payloadless.schema != nullptr);
    REQUIRE(payloadless.schema->fields.empty());
    REQUIRE(registry.findEventSchema("Opaque").state == EventSchemaState::Opaque);
    REQUIRE(registry.findEventSchema("Opaque").schema == nullptr);
    REQUIRE(registry.findEventSchema("TypedEmpty").state == EventSchemaState::Typed);
    REQUIRE(registry.findEventSchema("TypedEmpty").schema->fields.empty());
    REQUIRE(registry.findEventSchema("TypedValue").schema->fields.size() == 1);
}

TEST_CASE("Registration and rejected payloads do not construct or load typed events", "[event-schema][prevalidate]") {
    constructor_count = 0;
    ref_count = 0;
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<CountingEvent>("Counting");
    REQUIRE(constructor_count == 0);
    REQUIRE(ref_count == 0);
    REQUIRE(registry.payloadLoadCallCount() == 0);

    const Json missing = Json::object();
    requireError([&] { registry.emitByName("Counting", &missing); }, EventPayloadErrorCode::MissingField);
    REQUIRE(constructor_count == 0);
    REQUIRE(ref_count == 0);
    REQUIRE(registry.payloadLoadCallCount() == 0);
    REQUIRE(registry.pendingEventCount() == 0);

    const Json valid{{"value", 7}};
    REQUIRE(registry.emitByName("Counting", &valid) == 1);
    REQUIRE(constructor_count == 1);
    REQUIRE(ref_count == 1);
    REQUIRE(registry.payloadLoadCallCount() == 1);
    REQUIRE(registry.pendingEventCount() == 1);
}

TEST_CASE("Prevalidator closes every scalar vector quaternion and string JSON shape", "[event-schema][prevalidate]") {
    all_fields_constructor_count = 0;
    all_fields_ref_count = 0;
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<AllFieldsEvent>("AllFields");
    REQUIRE(all_fields_constructor_count == 0);
    REQUIRE(all_fields_ref_count == 0);

    auto reject = [&](std::string_view field, Json invalid, EventPayloadErrorCode code = EventPayloadErrorCode::TypeMismatch) {
        auto payload = validAllFields();
        payload[std::string{field}] = std::move(invalid);
        const auto calls = registry.payloadLoadCallCount();
        const auto pending = registry.pendingEventCount();
        const auto constructors = all_fields_constructor_count;
        const auto refs = all_fields_ref_count;
        requireError([&] { registry.emitByName("AllFields", &payload); }, code);
        REQUIRE(registry.payloadLoadCallCount() == calls);
        REQUIRE(registry.pendingEventCount() == pending);
        REQUIRE(all_fields_constructor_count == constructors);
        REQUIRE(all_fields_ref_count == refs);
    };

    reject("i8", 128);
    reject("i16", 32768);
    reject("i32", INT64_C(2147483648));
    reject("i64", UINT64_C(9007199254740993));
    reject("i8", 1.0);
    reject("u8", -1);
    reject("u16", 65536);
    reject("u32", -1);
    reject("u64", UINT64_C(9007199254740993));
    reject("f32", 1.0e100);
    reject("f32", std::numeric_limits<double>::infinity());
    reject("f64", std::numeric_limits<double>::quiet_NaN());
    reject("string", 7);
    reject("vec2", Json::array({1.0}));
    reject("vec2", Json::array({1.0, "x"}));
    reject("vec2", Json::array({1.0, std::numeric_limits<double>::infinity()}));
    reject("vec2", Json::array({1.0, 6.0}), EventPayloadErrorCode::OutOfRange);
    reject("vec3", Json::array({1.0, 2.0}));
    reject("vec3", Json::array({1.0, 2.0, std::numeric_limits<double>::infinity()}));
    reject("vec4", Json::array({1.0, 2.0, 3.0, "x"}));
    reject("vec4", Json::array({1.0, 2.0, 3.0, std::numeric_limits<double>::infinity()}));
    reject("quat", Json::array({0.0, 0.0, 1.0}));
    reject("quat", Json::array({0.0, 0.0, 0.0, std::numeric_limits<double>::infinity()}));
    reject("i8", 9, EventPayloadErrorCode::OutOfRange);

    auto missing = validAllFields();
    missing.erase("string");
    requireError([&] { registry.emitByName("AllFields", &missing); }, EventPayloadErrorCode::MissingField);
    auto unknown = validAllFields();
    unknown["extra"] = 1;
    requireError([&] { registry.emitByName("AllFields", &unknown); }, EventPayloadErrorCode::UnknownField);
    const Json scalar = 1;
    requireError([&] { registry.emitByName("AllFields", &scalar); }, EventPayloadErrorCode::PayloadMustBeObject);
    REQUIRE(registry.payloadLoadCallCount() == 0);
    REQUIRE(registry.pendingEventCount() == 0);
    REQUIRE(all_fields_constructor_count == 0);
    REQUIRE(all_fields_ref_count == 0);

    auto boundary = validAllFields();
    boundary["i64"] = INT64_C(9007199254740992);
    boundary["u64"] = UINT64_C(9007199254740992);
    REQUIRE(registry.emitByName("AllFields", &boundary) == 1);
    REQUIRE(registry.payloadLoadCallCount() == 1);
    REQUIRE(registry.pendingEventCount() == 1);
    REQUIRE(all_fields_constructor_count == 1);
    REQUIRE(all_fields_ref_count == 1);
}

TEST_CASE("By-name empty payload matrix is strict for every schema state", "[event-schema][matrix]") {
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<PayloadlessEvent>("Payloadless");
    registry.registerEvent<OpaqueEvent>("Opaque");
    registry.registerEvent<TypedEmptyEvent>("TypedEmpty");
    registry.registerEvent<TypedValueEvent>("TypedValue");

    const Json empty = Json::object();
    const Json extra{{"extra", 1}};
    const Json typed{{"value", 1}};
    const Json typed_extra{{"value", 1}, {"extra", 1}};

    const std::array<const void *, 3> shapes{nullptr, &empty, &extra};
    for (const void *shape : shapes) {
        requireError([&] { registry.validateEventPayload("Unknown", shape); }, EventPayloadErrorCode::UnknownEvent);
        requireError([&] { registry.validateEventPayload("Opaque", shape); }, EventPayloadErrorCode::NoPayloadEvent);
    }

    registry.validateEventPayload("Payloadless", nullptr);
    registry.validateEventPayload("Payloadless", &empty);
    requireError([&] { registry.validateEventPayload("Payloadless", &extra); }, EventPayloadErrorCode::UnknownField);

    requireError([&] { registry.validateEventPayload("TypedEmpty", nullptr); },
                 EventPayloadErrorCode::PayloadRequired);
    registry.validateEventPayload("TypedEmpty", &empty);
    requireError([&] { registry.validateEventPayload("TypedEmpty", &extra); }, EventPayloadErrorCode::UnknownField);

    requireError([&] { registry.validateEventPayload("TypedValue", nullptr); },
                 EventPayloadErrorCode::PayloadRequired);
    requireError([&] { registry.validateEventPayload("TypedValue", &empty); }, EventPayloadErrorCode::MissingField);
    requireError([&] { registry.validateEventPayload("TypedValue", &typed_extra); },
                 EventPayloadErrorCode::UnknownField);
    registry.validateEventPayload("TypedValue", &typed);

    REQUIRE(registry.emitByName("Payloadless", nullptr) == 1);
    REQUIRE(registry.emitByName("Payloadless", &empty) == 1);
    REQUIRE(registry.emitByName("TypedEmpty", &empty) == 1);
    REQUIRE(registry.emitByName("TypedValue", &typed) == 1);
    REQUIRE(registry.pendingEventCount() == 4);
}

TEST_CASE("UI binding decisions are complete for every schema lookup state", "[event-schema][matrix][ui]") {
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<PayloadlessEvent>("Payloadless");
    registry.registerEvent<OpaqueEvent>("Opaque");
    registry.registerEvent<TypedEmptyEvent>("TypedEmpty");
    registry.registerEvent<TypedValueEvent>("TypedValue");

    auto uiDecision = [&](std::string_view name, bool fields_specified) -> std::string_view {
        switch (registry.findEventSchema(name).state) {
        case EventSchemaState::UnknownEvent:
            return "unknown_event";
        case EventSchemaState::Opaque:
            return "no_payload_event";
        case EventSchemaState::Payloadless:
            return fields_specified ? "no_payload_event" : "ok";
        case EventSchemaState::Typed:
            return "ok";
        }
        return "unreachable";
    };

    for (bool fields : {false, true}) {
        REQUIRE(static_cast<bool>(uiDecision("Unknown", fields) == "unknown_event"));
        REQUIRE(static_cast<bool>(uiDecision("Opaque", fields) == "no_payload_event"));
        REQUIRE(static_cast<bool>(uiDecision("TypedEmpty", fields) == "ok"));
        REQUIRE(static_cast<bool>(uiDecision("TypedValue", fields) == "ok"));
    }
    REQUIRE(static_cast<bool>(uiDecision("Payloadless", false) == "ok"));
    REQUIRE(static_cast<bool>(uiDecision("Payloadless", true) == "no_payload_event"));
}

TEST_CASE("Catalog gate rejects every descriptor ref drift classification", "[event-schema][catalog]") {
    auto mismatch = []<class Event>(std::string name) {
        UserEventRegistererTemplatePublic registry;
        registry.registerEvent<Event>(std::move(name));
        REQUIRE_THROWS_WITH(registry.validateCatalogAndFreeze(), Catch::Matchers::ContainsSubstring("catalog mismatch"));
    };
    mismatch.template operator()<MissingDescriptorField>("MissingDescriptorField");
    mismatch.template operator()<DescriptorOnlyField>("DescriptorOnlyField");
    mismatch.template operator()<RenamedField>("RenamedField");
    mismatch.template operator()<ReorderedFields>("ReorderedFields");
    mismatch.template operator()<DuplicateRefField>("DuplicateRefField");
}

TEST_CASE("Global typed event catalog is validated by the mandatory CI gate", "[event-schema][catalog]") {
    REQUIRE(global_catalog_constructor_count == 0);
    REQUIRE_NOTHROW(Pelican::internal::validateEventCatalog());
    REQUIRE(global_catalog_constructor_count == 1);
}

TEST_CASE("Catalog gate documents conditional ref default-branch limitation", "[event-schema][catalog][known-limit]") {
    UserEventRegistererTemplatePublic registry;
    registry.registerEvent<ConditionalRefKnownLimit>("ConditionalRefKnownLimit");
    REQUIRE_NOTHROW(registry.validateCatalogAndFreeze());
    REQUIRE_THROWS_WITH(registry.registerEvent<PayloadlessEvent>("LateEvent"),
                        Catch::Matchers::ContainsSubstring("after catalog validation"));
}
