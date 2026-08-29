#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/details/event/registerer.hpp"

#include <schema.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;
using LeafType = Pelican::Schema::FieldType;

enum class CorpusMode : std::uint8_t { Idle, Run };

struct IntegerCorpusParams {
    std::int8_t i8{};
    std::int16_t i16{};
    std::int32_t i32{};
    std::int64_t i64{};
    std::uint8_t u8{};
    std::uint16_t u16{};
    std::uint32_t u32{};
    std::uint64_t u64{};
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::i8>("i8"),
                           std::int8_t{-8}),
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::i16>("i16"),
                           std::int16_t{-16}),
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::i32>("i32"),
                           std::int32_t{-32}),
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::i64>("i64"),
                           std::int64_t{-64}),
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::u8>("u8"),
                           std::uint8_t{8}),
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::u16>("u16"),
                           std::uint16_t{16}),
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::u32>("u32"),
                           std::uint32_t{32}),
        Pelican::defaulted(Pelican::field<&IntegerCorpusParams::u64>("u64"),
                           std::uint64_t{64}));
};

struct FloatingCorpusParams {
    float f32{};
    double f64{};
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&FloatingCorpusParams::f32>("f32"),
                           1.25F),
        Pelican::defaulted(
            Pelican::field<&FloatingCorpusParams::f64>(
                "f64", Pelican::frange(-4.0, 4.0)),
            2.5));
};

struct VectorCorpusParams {
    Pelican::vec2 vec2{};
    Pelican::vec3 vec3{};
    Pelican::vec4 vec4{};
    Pelican::quat quat{};
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&VectorCorpusParams::vec2>("vec2"),
                           Pelican::vec2{1.0F, 2.0F}),
        Pelican::defaulted(Pelican::field<&VectorCorpusParams::vec3>("vec3"),
                           Pelican::vec3{1.0F, 2.0F, 3.0F}),
        Pelican::defaulted(Pelican::field<&VectorCorpusParams::vec4>("vec4"),
                           Pelican::vec4{1.0F, 2.0F, 3.0F, 4.0F}),
        Pelican::defaulted(Pelican::field<&VectorCorpusParams::quat>("quat"),
                           Pelican::quat{0.0F, 0.0F, 0.0F, 1.0F}));
};

struct OtherCorpusParams {
    std::string string;
    bool boolean{};
    CorpusMode enumeration{};
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(
            Pelican::field<&OtherCorpusParams::string>("string"), "fixture"),
        Pelican::defaulted(
            Pelican::field<&OtherCorpusParams::boolean>("bool"), true),
        Pelican::defaulted(
            Pelican::field<&OtherCorpusParams::enumeration>("enum"),
            CorpusMode::Idle,
            Pelican::enumValues(
                Pelican::enumValue("idle", CorpusMode::Idle),
                Pelican::enumValue("run", CorpusMode::Run))));
};

template <class ParamsType>
class CorpusBehavior final : public Pelican::Behavior {
  public:
    using Params = ParamsType;
};

using IntegerCorpusBehavior = CorpusBehavior<IntegerCorpusParams>;
using FloatingCorpusBehavior = CorpusBehavior<FloatingCorpusParams>;
using VectorCorpusBehavior = CorpusBehavior<VectorCorpusParams>;
using OtherCorpusBehavior = CorpusBehavior<OtherCorpusParams>;

struct EventCorpus {
    std::int8_t i8{};
    std::int16_t i16{};
    std::int32_t i32{};
    std::int64_t i64{};
    std::uint8_t u8{};
    std::uint16_t u16{};
    std::uint32_t u32{};
    std::uint64_t u64{};
    float f32{};
    double f64{};
    Pelican::vec2 vec2{};
    Pelican::vec3 vec3{};
    Pelican::vec4 vec4{};
    Pelican::quat quat{};
    std::string string;

    EventCorpus() noexcept = default;

    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&EventCorpus::i8>("i8"),
        Pelican::field<&EventCorpus::i16>("i16"),
        Pelican::field<&EventCorpus::i32>("i32"),
        Pelican::field<&EventCorpus::i64>("i64"),
        Pelican::field<&EventCorpus::u8>("u8"),
        Pelican::field<&EventCorpus::u16>("u16"),
        Pelican::field<&EventCorpus::u32>("u32"),
        Pelican::field<&EventCorpus::u64>("u64"),
        Pelican::field<&EventCorpus::f32>("f32"),
        Pelican::field<&EventCorpus::f64>("f64", Pelican::frange(-4.0, 4.0)),
        Pelican::field<&EventCorpus::vec2>("vec2"),
        Pelican::field<&EventCorpus::vec3>("vec3"),
        Pelican::field<&EventCorpus::vec4>("vec4"),
        Pelican::field<&EventCorpus::quat>("quat"),
        Pelican::field<&EventCorpus::string>("string"));

    template <class Archive> void ref(Archive &archive) {
        archive.prop("i8", i8);
        archive.prop("i16", i16);
        archive.prop("i32", i32);
        archive.prop("i64", i64);
        archive.prop("u8", u8);
        archive.prop("u16", u16);
        archive.prop("u32", u32);
        archive.prop("u64", u64);
        archive.prop("f32", f32);
        archive.prop("f64", f64);
        archive.prop("vec2", vec2);
        archive.prop("vec3", vec3);
        archive.prop("vec4", vec4);
        archive.prop("quat", quat);
        archive.prop("string", string);
    }
};

class TypedBehaviorRegistration {
    std::array<Pelican::internal::RegistrationToken, 4> tokens_;

    static const Pelican::internal::BehaviorRegistration &get(
        std::string_view name) {
        const auto *result =
            Pelican::internal::getBehaviorRegisterer().findByName(name);
        if (result == nullptr) throw std::logic_error("corpus behavior missing");
        return *result;
    }

  public:
    TypedBehaviorRegistration()
        : tokens_{
              Pelican::internal::getBehaviorRegisterer()
                  .registerBehavior<IntegerCorpusBehavior>(
                      "wp358_integer_value_corpus", 1, {}),
              Pelican::internal::getBehaviorRegisterer()
                  .registerBehavior<FloatingCorpusBehavior>(
                      "wp358_floating_value_corpus", 1, {}),
              Pelican::internal::getBehaviorRegisterer()
                  .registerBehavior<VectorCorpusBehavior>(
                      "wp358_vector_value_corpus", 1, {}),
              Pelican::internal::getBehaviorRegisterer()
                  .registerBehavior<OtherCorpusBehavior>(
                      "wp358_other_value_corpus", 1, {})} {}

    ~TypedBehaviorRegistration() {
        for (auto token = tokens_.rbegin(); token != tokens_.rend(); ++token) {
            Pelican::internal::unregisterBehavior(*token);
        }
    }

    OrderedJson canonicalize(const Json &payload) const {
        Json integers = Json::object();
        Json floating = Json::object();
        Json vectors = Json::object();
        Json other = Json::object();
        for (auto item = payload.begin(); item != payload.end(); ++item) {
            if (item.key() == "i8" || item.key() == "i16" ||
                item.key() == "i32" || item.key() == "i64" ||
                item.key() == "u8" || item.key() == "u16" ||
                item.key() == "u32" || item.key() == "u64") {
                integers[item.key()] = *item;
            } else if (item.key() == "f32" || item.key() == "f64") {
                floating[item.key()] = *item;
            } else if (item.key() == "vec2" || item.key() == "vec3" ||
                       item.key() == "vec4" || item.key() == "quat") {
                vectors[item.key()] = *item;
            } else if (item.key() == "string" || item.key() == "bool" ||
                       item.key() == "enum") {
                other[item.key()] = *item;
            } else {
                throw std::logic_error("unknown typed corpus field");
            }
        }
        auto result = OrderedJson::object();
        for (const auto name : {"wp358_integer_value_corpus",
                                "wp358_floating_value_corpus",
                                "wp358_vector_value_corpus",
                                "wp358_other_value_corpus"}) {
            const Json &part = name == std::string_view{"wp358_integer_value_corpus"}
                                   ? integers
                               : name == std::string_view{"wp358_floating_value_corpus"}
                                   ? floating
                               : name == std::string_view{"wp358_vector_value_corpus"}
                                   ? vectors
                                   : other;
            const auto canonical =
                OrderedJson::parse(get(name).canonicalize_params(part));
            for (const auto &item : canonical.items()) {
                result[item.key()] = item.value();
            }
        }
        return result;
    }
};

Pelican::Schema::FieldDeclaration leafField(
    LeafType type, Json default_value, std::string name = {}) {
    Pelican::Schema::FieldDeclaration result{
        .name = name.empty()
                    ? std::string{Pelican::Schema::fieldTypeName(type)}
                    : std::move(name),
        .type = type,
        .default_value = std::move(default_value),
    };
    if (type == LeafType::F64) {
        result.range = Pelican::Schema::FloatingRange{-4.0, 4.0};
    }
    if (type == LeafType::Enum) result.enum_values = {"idle", "run"};
    return result;
}

std::vector<Pelican::Schema::FieldDeclaration> behaviorLeafFields() {
    return {
        leafField(LeafType::I8, -8),
        leafField(LeafType::I16, -16),
        leafField(LeafType::I32, -32),
        leafField(LeafType::I64, -64),
        leafField(LeafType::U8, 8),
        leafField(LeafType::U16, 16),
        leafField(LeafType::U32, 32),
        leafField(LeafType::U64, 64),
        leafField(LeafType::F32, 1.25),
        leafField(LeafType::F64, 2.5),
        leafField(LeafType::Vec2, Json::array({1.0, 2.0})),
        leafField(LeafType::Vec3, Json::array({1.0, 2.0, 3.0})),
        leafField(LeafType::Vec4, Json::array({1.0, 2.0, 3.0, 4.0})),
        leafField(LeafType::Quat, Json::array({0.0, 0.0, 0.0, 1.0})),
        leafField(LeafType::String, "fixture"),
        leafField(LeafType::Bool, true, "bool"),
        leafField(LeafType::Enum, "idle", "enum"),
    };
}

std::vector<Pelican::Schema::FieldDeclaration> eventLeafFields() {
    return {
        leafField(LeafType::I8, -8),
        leafField(LeafType::I16, -16),
        leafField(LeafType::I32, -32),
        leafField(LeafType::I64, -64),
        leafField(LeafType::U8, 8),
        leafField(LeafType::U16, 16),
        leafField(LeafType::U32, 32),
        leafField(LeafType::U64, 64),
        leafField(LeafType::F32, 1.25),
        leafField(LeafType::F64, 2.5),
        leafField(LeafType::Vec2, Json::array({1.0, 2.0})),
        leafField(LeafType::Vec3, Json::array({1.0, 2.0, 3.0})),
        leafField(LeafType::Vec4, Json::array({1.0, 2.0, 3.0, 4.0})),
        leafField(LeafType::Quat, Json::array({0.0, 0.0, 0.0, 1.0})),
        leafField(LeafType::String, "fixture"),
    };
}

OrderedJson eventJson(const EventCorpus &event) {
    return OrderedJson{
        {"i8", event.i8},
        {"i16", event.i16},
        {"i32", event.i32},
        {"i64", event.i64},
        {"u8", event.u8},
        {"u16", event.u16},
        {"u32", event.u32},
        {"u64", event.u64},
        {"f32", event.f32},
        {"f64", event.f64},
        {"vec2", OrderedJson::array({event.vec2.x, event.vec2.y})},
        {"vec3", OrderedJson::array(
                     {event.vec3.x, event.vec3.y, event.vec3.z})},
        {"vec4", OrderedJson::array(
                     {event.vec4.x, event.vec4.y, event.vec4.z, event.vec4.w})},
        {"quat", OrderedJson::array(
                     {event.quat.x, event.quat.y, event.quat.z, event.quat.w})},
        {"string", event.string},
    };
}

OrderedJson loadEvent(const Json &payload) {
    Pelican::internal::UserEventRegistererTemplatePublic registry;
    (void)registry.registerEvent<EventCorpus>("WP358EventCorpus");
    REQUIRE(registry.emitByName("WP358EventCorpus", &payload) == 1);
    registry.freezePendingEventsForFrame();
    auto queued = registry.drainFrozenEvents();
    REQUIRE(queued.size() == 1);
    const auto *event = static_cast<const EventCorpus *>(queued.front().payload.get());
    REQUIRE(event != nullptr);
    return eventJson(*event);
}

struct InvalidValueCase {
    std::string name;
    Json value;
    Pelican::Schema::ErrorCode leaf_code;
    Pelican::StructFieldErrorCode core_code;
    Pelican::EventPayloadErrorCode event_code;
    bool in_behavior = true;
    bool in_event = true;
};

std::vector<InvalidValueCase> invalidCases() {
    using LeafError = Pelican::Schema::ErrorCode;
    using CoreError = Pelican::StructFieldErrorCode;
    using EventError = Pelican::EventPayloadErrorCode;
    return {
        {"i8", 128, LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch},
        {"i16", 32768, LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch},
        {"i32", INT64_C(2147483648), LeafError::TypeMismatch,
         CoreError::TypeMismatch, EventError::TypeMismatch},
        {"i64", UINT64_C(9007199254740993), LeafError::TypeMismatch,
         CoreError::TypeMismatch, EventError::TypeMismatch},
        {"u8", 256, LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch},
        {"u16", 65536, LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch},
        {"u32", UINT64_C(4294967296), LeafError::TypeMismatch,
         CoreError::TypeMismatch, EventError::TypeMismatch},
        {"u64", UINT64_C(9007199254740993), LeafError::TypeMismatch,
         CoreError::TypeMismatch, EventError::TypeMismatch},
        {"f32", 1.0e100, LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch},
        {"f64", 5.0, LeafError::OutOfRange, CoreError::OutOfRange,
         EventError::OutOfRange},
        {"vec2", Json::array({1.0}), LeafError::TypeMismatch,
         CoreError::TypeMismatch, EventError::TypeMismatch},
        {"vec3", Json::array({1.0, 2.0}), LeafError::TypeMismatch,
         CoreError::TypeMismatch, EventError::TypeMismatch},
        {"vec4", Json::array({1.0, 2.0, 3.0, "bad"}),
         LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch},
        {"quat", Json::array({0.0, 0.0, 1.0}), LeafError::TypeMismatch,
         CoreError::TypeMismatch, EventError::TypeMismatch},
        {"string", 7, LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch},
        {"bool", 1, LeafError::TypeMismatch, CoreError::TypeMismatch,
         EventError::TypeMismatch, true, false},
        {"enum", "missing", LeafError::InvalidEnum, CoreError::InvalidEnum,
         EventError::TypeMismatch, true, false},
    };
}

std::string eventErrorPath(const Pelican::EventPayloadValidationError &error) {
    constexpr std::string_view marker = " field '";
    const std::string_view message = error.what();
    const auto field_begin = message.find(marker);
    if (field_begin == std::string_view::npos) {
        throw std::logic_error("event validation error does not name a field");
    }
    const auto name_begin = field_begin + marker.size();
    const auto name_end = message.find('\'', name_begin);
    if (name_end == std::string_view::npos || name_end == name_begin) {
        throw std::logic_error("event validation error has an invalid field path");
    }
    return "params." + std::string{message.substr(name_begin, name_end - name_begin)};
}

const Pelican::Schema::FieldDeclaration &findLeafField(
    const std::vector<Pelican::Schema::FieldDeclaration> &fields,
    std::string_view name) {
    for (const auto &field : fields) {
        if (field.name == name) return field;
    }
    throw std::logic_error("leaf corpus field missing");
}

} // namespace

TEST_CASE("WP359 typed Vec2 default registers through the production path",
          "[wp359][schema][behavior][reproduction]") {
    TypedBehaviorRegistration registered;
    REQUIRE(registered.canonicalize(Json::object()).at("vec2") ==
            OrderedJson::array({1.0, 2.0}));
}

TEST_CASE("WP359 typed behavior and leaf agree on all 17 encode-decode-canonicalize values",
          "[wp358][wp359][schema][corpus][behavior]") {
    TypedBehaviorRegistration registered;
    const auto fields = behaviorLeafFields();
    const auto leaf_defaults =
        Pelican::Schema::resolveObject(fields, Json::object());
    const auto core_defaults = registered.canonicalize(Json::object());
    REQUIRE(core_defaults == leaf_defaults);
    REQUIRE(registered.canonicalize(Json::parse(core_defaults.dump())) ==
            core_defaults);

    Json supplied = Json::object();
    supplied["i8"] = 7;
    supplied["i16"] = -160;
    supplied["i32"] = 32000;
    supplied["i64"] = INT64_C(9007199254740992);
    supplied["u8"] = 250;
    supplied["u16"] = 65000;
    supplied["u32"] = UINT64_C(4000000000);
    supplied["u64"] = UINT64_C(9007199254740992);
    supplied["f32"] = 16777217.0;
    supplied["f64"] = 3.5;
    supplied["vec2"] = Json::array({16777217.0, -2.0});
    supplied["vec3"] = Json::array({3.0, 4.0, 5.0});
    supplied["vec4"] = Json::array({6.0, 7.0, 8.0, 9.0});
    supplied["quat"] = Json::array({0.0, 0.0, 1.0, 0.0});
    supplied["string"] = "resolved";
    supplied["bool"] = false;
    supplied["enum"] = "run";
    const auto leaf_resolved = Pelican::Schema::resolveObject(fields, supplied);
    const auto core_resolved = registered.canonicalize(supplied);
    REQUIRE(core_resolved == leaf_resolved);
    REQUIRE(core_resolved.at("f32") == 16777216.0);
    REQUIRE(core_resolved.at("vec2").at(0) == 16777216.0);
    REQUIRE(registered.canonicalize(Json::parse(core_resolved.dump())) ==
            core_resolved);
    REQUIRE(core_resolved.dump().find("\"i8\"") <
            core_resolved.dump().find("\"u8\""));
    std::cout << "WP359_CORPUS_TYPED_BEHAVIOR_TYPES=17 ACROSS=4 ROUNDTRIPS=2 DEFAULT_F32="
              << core_defaults.at("f32") << " RESOLVED_F32="
              << core_resolved.at("f32") << " RESOLVED_VEC2_X="
              << core_resolved.at("vec2").at(0) << '\n';
}

TEST_CASE("WP358 event and leaf agree on all 15 event types including resolved F32",
          "[wp358][schema][corpus][event]") {
    const auto fields = eventLeafFields();
    const auto defaults = Pelican::Schema::resolveObject(fields, Json::object());
    REQUIRE(loadEvent(defaults) == defaults);

    Json supplied = defaults;
    supplied["f32"] = 16777217.0;
    supplied["vec2"] = Json::array({16777217.0, 2.0});
    supplied["string"] = "resolved";
    const auto leaf_resolved = Pelican::Schema::resolveObject(fields, supplied);
    const auto event_resolved = loadEvent(supplied);
    REQUIRE(event_resolved == leaf_resolved);
    REQUIRE(event_resolved.at("f32") == 16777216.0);
    REQUIRE(event_resolved.at("vec2").at(0) == 16777216.0);
    std::cout << "WP358_CORPUS_EVENT_TYPES=15 RESOLVED_F32="
              << event_resolved.at("f32") << " RESOLVED_VEC2_X="
              << event_resolved.at("vec2").at(0) << '\n';
}

TEST_CASE("WP358 common negative corpus fixes category and field path",
          "[wp358][schema][corpus][errors]") {
    TypedBehaviorRegistration registered;
    const auto behavior_fields = behaviorLeafFields();
    const auto event_fields = eventLeafFields();
    const Json behavior_defaults =
        Pelican::Schema::resolveObject(behavior_fields, Json::object());
    const Json event_defaults =
        Pelican::Schema::resolveObject(event_fields, Json::object());

    std::size_t behavior_compared = 0;
    std::size_t event_compared = 0;
    for (const auto &entry : invalidCases()) {
        CAPTURE(entry.name);
        const auto &leaf_field = findLeafField(
            entry.in_behavior ? behavior_fields : event_fields, entry.name);
        const auto expected_path = "params." + entry.name;
        try {
            (void)Pelican::Schema::resolveValue(
                leaf_field, entry.value, expected_path);
            FAIL("leaf accepted invalid corpus value");
        } catch (const Pelican::Schema::Error &error) {
            REQUIRE(error.code() == entry.leaf_code);
            REQUIRE(std::string{error.path()} == expected_path);
        }

        if (entry.in_behavior) {
            Json payload = behavior_defaults;
            payload[entry.name] = entry.value;
            try {
                (void)registered.canonicalize(payload);
                FAIL("core behavior accepted invalid corpus value");
            } catch (const Pelican::StructFieldValidationError &error) {
                REQUIRE(error.code() == entry.core_code);
                REQUIRE(std::string{error.path()} == expected_path);
            }
            ++behavior_compared;
        }

        if (entry.in_event) {
            Json payload = event_defaults;
            payload[entry.name] = entry.value;
            Pelican::internal::UserEventRegistererTemplatePublic registry;
            (void)registry.registerEvent<EventCorpus>("WP358EventCorpusError");
            try {
                registry.validateEventPayload("WP358EventCorpusError", &payload);
                FAIL("event accepted invalid corpus value");
            } catch (const Pelican::EventPayloadValidationError &error) {
                REQUIRE(error.code() == entry.event_code);
                REQUIRE(eventErrorPath(error) == expected_path);
            }
            ++event_compared;
        }
    }
    REQUIRE(behavior_compared == 17);
    REQUIRE(event_compared == 15);
    std::cout << "WP359_CORPUS_NEGATIVE_LEAF=17 CORE_TYPED=17 EVENT=15"
              << " PATH_PREFIX=params.\n";
}
