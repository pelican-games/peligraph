#include "../src/core/communication/schemavocabularyadapter.hpp"
#include "../src/project/schemawire.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using CoreType = Pelican::StructFieldType;
using LeafType = Pelican::Schema::FieldType;
using Json = nlohmann::json;

struct MappingPair {
    CoreType core;
    LeafType leaf;
    std::string_view name;
};

constexpr std::array<MappingPair, 17> MappingPairs{{
    {CoreType::I8, LeafType::I8, "i8"},
    {CoreType::I16, LeafType::I16, "i16"},
    {CoreType::I32, LeafType::I32, "i32"},
    {CoreType::I64, LeafType::I64, "i64"},
    {CoreType::U8, LeafType::U8, "u8"},
    {CoreType::U16, LeafType::U16, "u16"},
    {CoreType::U32, LeafType::U32, "u32"},
    {CoreType::U64, LeafType::U64, "u64"},
    {CoreType::F32, LeafType::F32, "f32"},
    {CoreType::F64, LeafType::F64, "f64"},
    {CoreType::Vec2, LeafType::Vec2, "vec2"},
    {CoreType::Vec3, LeafType::Vec3, "vec3"},
    {CoreType::Vec4, LeafType::Vec4, "vec4"},
    {CoreType::Quat, LeafType::Quat, "quat"},
    {CoreType::String, LeafType::String, "string"},
    {CoreType::Bool, LeafType::Bool, "bool"},
    {CoreType::Enum, LeafType::Enum, "enum"},
}};

static_assert(MappingPairs.size() == Pelican::Schema::allFieldTypes.size());

template <class Invoke>
void requireSchemaError(Invoke &&invoke, Pelican::Schema::ErrorCode code,
                        std::string_view path) {
    try {
        invoke();
        FAIL("expected Pelican::Schema::Error");
    } catch (const Pelican::Schema::Error &error) {
        REQUIRE(error.code() == code);
        REQUIRE(static_cast<bool>(error.path() == path));
        REQUIRE(std::string{error.what()}.find(
                    Pelican::Schema::errorCodeName(code)) != std::string::npos);
    }
}

Pelican::Schema::FieldDeclaration declaration(LeafType type) {
    Pelican::Schema::FieldDeclaration result{
        .name = std::string{Pelican::Schema::fieldTypeName(type)},
        .type = type,
    };
    switch (type) {
    case LeafType::I8: result.default_value = std::int8_t{-8}; break;
    case LeafType::I16: result.default_value = std::int16_t{-16}; break;
    case LeafType::I32: result.default_value = std::int32_t{-32}; break;
    case LeafType::I64: result.default_value = std::int64_t{-64}; break;
    case LeafType::U8: result.default_value = std::uint8_t{8}; break;
    case LeafType::U16: result.default_value = std::uint16_t{16}; break;
    case LeafType::U32: result.default_value = std::uint32_t{32}; break;
    case LeafType::U64: result.default_value = std::uint64_t{64}; break;
    case LeafType::F32: result.default_value = 1.25F; break;
    case LeafType::F64: result.default_value = 2.5; break;
    case LeafType::Vec2: result.default_value = Json::array({1.0, 2.0}); break;
    case LeafType::Vec3:
        result.default_value = Json::array({1.0, 2.0, 3.0});
        break;
    case LeafType::Vec4:
        result.default_value = Json::array({1.0, 2.0, 3.0, 4.0});
        break;
    case LeafType::Quat:
        result.default_value = Json::array({0.0, 0.0, 0.0, 1.0});
        break;
    case LeafType::String: result.default_value = "value"; break;
    case LeafType::Bool: result.default_value = true; break;
    case LeafType::Enum:
        result.enum_values = {"idle", "run"};
        result.default_value = "idle";
        break;
    }
    return result;
}

struct ValueCase {
    LeafType type;
    Json accepted;
    Json resolved;
    Json rejected;
    Pelican::Schema::ErrorCode rejected_code;
};

std::vector<ValueCase> valueCases() {
    using Error = Pelican::Schema::ErrorCode;
    return {
        {LeafType::I8, 127, 127, 128, Error::TypeMismatch},
        {LeafType::I16, -32768, -32768, -32769, Error::TypeMismatch},
        {LeafType::I32, INT64_C(2147483647), INT64_C(2147483647),
         INT64_C(2147483648), Error::TypeMismatch},
        {LeafType::I64, INT64_C(9007199254740992),
         INT64_C(9007199254740992), INT64_C(9007199254740993),
         Error::TypeMismatch},
        {LeafType::U8, UINT64_C(255), UINT64_C(255), UINT64_C(256),
         Error::TypeMismatch},
        {LeafType::U16, UINT64_C(65535), UINT64_C(65535), UINT64_C(65536),
         Error::TypeMismatch},
        {LeafType::U32, UINT64_C(4294967295), UINT64_C(4294967295),
         UINT64_C(4294967296), Error::TypeMismatch},
        {LeafType::U64, UINT64_C(9007199254740992),
         UINT64_C(9007199254740992), UINT64_C(9007199254740993),
         Error::TypeMismatch},
        {LeafType::F32, 16777217.0, 16777216.0, 1.0e100,
         Error::TypeMismatch},
        {LeafType::F64, 1.25, 1.25, "number", Error::TypeMismatch},
        {LeafType::Vec2, Json::array({16777217.0, 2.0}),
         Json::array({16777216.0, 2.0}), Json::array({1.0}),
         Error::TypeMismatch},
        {LeafType::Vec3, Json::array({1.0, 2.0, 3.0}),
         Json::array({1.0, 2.0, 3.0}), Json::array({1.0, 2.0}),
         Error::TypeMismatch},
        {LeafType::Vec4, Json::array({1.0, 2.0, 3.0, 4.0}),
         Json::array({1.0, 2.0, 3.0, 4.0}),
         Json::array({1.0, 2.0, 3.0, "bad"}), Error::TypeMismatch},
        {LeafType::Quat, Json::array({0.0, 0.0, 0.0, 1.0}),
         Json::array({0.0, 0.0, 0.0, 1.0}),
         Json::array({0.0, 0.0, 1.0}), Error::TypeMismatch},
        {LeafType::String, "value", "value", 7, Error::TypeMismatch},
        {LeafType::Bool, true, true, 1, Error::TypeMismatch},
        {LeafType::Enum, "run", "run", "missing", Error::InvalidEnum},
    };
}

LeafType i8U8SwapMutation(CoreType type) {
    if (type == CoreType::I8) return LeafType::U8;
    if (type == CoreType::U8) return LeafType::I8;
    return Pelican::internal::toLeafSchemaType(type);
}

} // namespace

TEST_CASE("WP358 normative table fixes every one of the 17 wire meanings",
          "[wp358][schema][table]") {
    const auto &rules = Pelican::Schema::typeRules();
    REQUIRE(rules.size() == MappingPairs.size());
    std::set<std::string_view> names;
    std::set<unsigned> ordinals;
    for (std::size_t index = 0; index < MappingPairs.size(); ++index) {
        const auto &expected = MappingPairs[index];
        const auto &rule = rules[index];
        REQUIRE(rule.type == expected.leaf);
        REQUIRE(static_cast<bool>(rule.wire_name == expected.name));
        REQUIRE(Pelican::Schema::allFieldTypes[index] == expected.leaf);
        REQUIRE(names.insert(rule.wire_name).second);
        REQUIRE(ordinals.insert(static_cast<unsigned>(rule.type)).second);
    }

    REQUIRE(rules[0].signed_min == -128);
    REQUIRE(rules[0].signed_max == 127);
    REQUIRE(rules[3].signed_min == std::numeric_limits<std::int64_t>::min());
    REQUIRE(rules[3].signed_max == std::numeric_limits<std::int64_t>::max());
    REQUIRE(rules[4].unsigned_max == 255);
    REQUIRE(rules[7].unsigned_max ==
            std::numeric_limits<std::uint64_t>::max());
    REQUIRE(rules[8].components_require_f32);
    REQUIRE_FALSE(rules[9].components_require_f32);
    REQUIRE(rules[10].vector_arity == 2);
    REQUIRE(rules[11].vector_arity == 3);
    REQUIRE(rules[12].vector_arity == 4);
    REQUIRE(rules[13].vector_arity == 4);
    REQUIRE(rules[13].range_kind == Pelican::Schema::RangeKind::None);
    REQUIRE(Pelican::Schema::maxEnumValues == 8);
}

TEST_CASE("WP358 leaf name conversion is a named bidirectional partial function",
          "[wp358][schema][names]") {
    for (const auto &pair : MappingPairs) {
        REQUIRE(Pelican::Schema::fieldTypeFromName(pair.name) == pair.leaf);
        REQUIRE(static_cast<bool>(
            Pelican::Schema::fieldTypeName(pair.leaf) == pair.name));
    }
    requireSchemaError(
        [] { (void)Pelican::Schema::fieldTypeFromName("i128", "wire.type"); },
        Pelican::Schema::ErrorCode::UnknownTypeName, "wire.type");
    requireSchemaError(
        [] {
            (void)Pelican::Schema::fieldTypeName(
                static_cast<LeafType>(255), "wire.type");
        },
        Pelican::Schema::ErrorCode::InvalidTypeOrdinal, "wire.type");
}

TEST_CASE("WP358 core mapping fixes all 17 semantic pairs, coverage and roundtrip",
          "[wp358][schema][mapping]") {
    std::set<unsigned> core_ordinals;
    std::set<unsigned> leaf_ordinals;
    for (const auto &pair : MappingPairs) {
        CAPTURE(std::string{pair.name});
        REQUIRE(Pelican::internal::toLeafSchemaType(pair.core) == pair.leaf);
        REQUIRE(Pelican::internal::toCoreSchemaType(pair.leaf) == pair.core);
        REQUIRE(Pelican::internal::toCoreSchemaType(
                    Pelican::internal::toLeafSchemaType(pair.core)) == pair.core);
        REQUIRE(Pelican::internal::toLeafSchemaType(
                    Pelican::internal::toCoreSchemaType(pair.leaf)) == pair.leaf);
        REQUIRE(static_cast<bool>(
            Pelican::internal::schemaTypeName(pair.core) == pair.name));
        REQUIRE(core_ordinals.insert(static_cast<unsigned>(pair.core)).second);
        REQUIRE(leaf_ordinals.insert(static_cast<unsigned>(pair.leaf)).second);
    }
    REQUIRE(core_ordinals.size() == 17);
    REQUIRE(leaf_ordinals.size() == 17);

    requireSchemaError(
        [] {
            (void)Pelican::Schema::typeRule(static_cast<LeafType>(255));
        },
        Pelican::Schema::ErrorCode::InvalidTypeOrdinal, "type");
    REQUIRE_THROWS_AS(
        Pelican::internal::toLeafSchemaType(static_cast<CoreType>(255)),
        Pelican::internal::SchemaTypeMappingError);
    REQUIRE_THROWS_AS(
        Pelican::internal::toCoreSchemaType(static_cast<LeafType>(255)),
        Pelican::internal::SchemaTypeMappingError);
}

TEST_CASE("WP358 I8-U8 transposition mutation is killed by pair semantics",
          "[wp358][schema][mutation]") {
    std::size_t killed_pairs = 0;
    for (const auto &pair : MappingPairs) {
        killed_pairs += i8U8SwapMutation(pair.core) != pair.leaf ? 1U : 0U;
    }
    REQUIRE(killed_pairs == 2);
    std::cout << "WP358_MAPPING_MUTATION=I8<->U8 KILLED_PAIRS="
              << killed_pairs << '\n';
}

TEST_CASE("WP358 declaration corpus covers every normative rule row",
          "[wp358][schema][declaration]") {
    for (const auto type : Pelican::Schema::allFieldTypes) {
        auto field = declaration(type);
        const auto &rule = Pelican::Schema::typeRule(type);
        switch (rule.range_kind) {
        case Pelican::Schema::RangeKind::None: break;
        case Pelican::Schema::RangeKind::Signed:
            field.range = Pelican::Schema::SignedRange{rule.signed_min,
                                                       rule.signed_max};
            break;
        case Pelican::Schema::RangeKind::Unsigned:
            field.range = Pelican::Schema::UnsignedRange{0,
                                                         rule.unsigned_max};
            break;
        case Pelican::Schema::RangeKind::Floating:
            field.range = Pelican::Schema::FloatingRange{-100.0, 100.0};
            break;
        }
        field.unit = "opaque:unit/string";
        INFO(std::string{Pelican::Schema::fieldTypeName(type)});
        REQUIRE_NOTHROW(Pelican::Schema::validateDeclaration(field));
        REQUIRE(field.unit == "opaque:unit/string");
    }

    auto field = declaration(LeafType::I8);
    field.range = Pelican::Schema::SignedRange{1, 0};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.i8"); },
        Pelican::Schema::ErrorCode::RangeMinimumExceedsMaximum, "schema.i8");
    field.range = Pelican::Schema::SignedRange{-129, 127};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.i8"); },
        Pelican::Schema::ErrorCode::RangeBoundNotRepresentable, "schema.i8");
    field.range = Pelican::Schema::UnsignedRange{0, 1};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.i8"); },
        Pelican::Schema::ErrorCode::RangeKindMismatch, "schema.i8");

    field = declaration(LeafType::U8);
    field.range = Pelican::Schema::UnsignedRange{2, 1};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.u8"); },
        Pelican::Schema::ErrorCode::RangeMinimumExceedsMaximum, "schema.u8");
    field.range = Pelican::Schema::UnsignedRange{0, 256};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.u8"); },
        Pelican::Schema::ErrorCode::RangeBoundNotRepresentable, "schema.u8");

    field = declaration(LeafType::F32);
    field.range = Pelican::Schema::FloatingRange{
        -std::numeric_limits<double>::infinity(), 1.0};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.f32"); },
        Pelican::Schema::ErrorCode::RangeBoundNotFinite, "schema.f32");
    field.range = Pelican::Schema::FloatingRange{2.0, 1.0};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.f32"); },
        Pelican::Schema::ErrorCode::RangeMinimumExceedsMaximum, "schema.f32");
    field.range = Pelican::Schema::FloatingRange{-1.0, 1.0e100};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.f32"); },
        Pelican::Schema::ErrorCode::RangeBoundNotRepresentable, "schema.f32");

    for (const auto type : {LeafType::Quat, LeafType::String, LeafType::Bool,
                            LeafType::Enum}) {
        field = declaration(type);
        field.range = Pelican::Schema::FloatingRange{0.0, 1.0};
        requireSchemaError(
            [&] { Pelican::Schema::validateDeclaration(field, "schema.none"); },
            Pelican::Schema::ErrorCode::RangeNotApplicable, "schema.none");
    }

    field = declaration(LeafType::Enum);
    field.enum_values.clear();
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.enum"); },
        Pelican::Schema::ErrorCode::EnumValuesRequired, "schema.enum");
    field.enum_values.assign(9, "x");
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.enum"); },
        Pelican::Schema::ErrorCode::EnumTooManyValues, "schema.enum");
    field.enum_values = {"idle", ""};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.enum"); },
        Pelican::Schema::ErrorCode::EnumValueEmpty, "schema.enum");
    field.enum_values = {"idle", "idle"};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.enum"); },
        Pelican::Schema::ErrorCode::EnumValueDuplicate, "schema.enum");
    field.enum_values = {"idle", "run"};
    field.default_value = "missing";
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.enum"); },
        Pelican::Schema::ErrorCode::InvalidEnum, "schema.enum.default");

    field = declaration(LeafType::String);
    field.enum_values = {"not", "an", "enum"};
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.string"); },
        Pelican::Schema::ErrorCode::EnumValuesNotApplicable, "schema.string");
    field = declaration(LeafType::Bool);
    field.default_value = "true";
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclaration(field, "schema.bool"); },
        Pelican::Schema::ErrorCode::TypeMismatch, "schema.bool.default");

    auto duplicate = std::array{declaration(LeafType::I8),
                                declaration(LeafType::U8)};
    duplicate[1].name = duplicate[0].name;
    requireSchemaError(
        [&] { Pelican::Schema::validateDeclarations(duplicate); },
        Pelican::Schema::ErrorCode::DuplicateFieldName, "schema[1].name");
}

TEST_CASE("WP358 value corpus covers all 17 shapes bounds arities and F32 resolution",
          "[wp358][schema][values]") {
    for (const auto &entry : valueCases()) {
        auto field = declaration(entry.type);
        const std::string path = "params." + field.name;
        INFO(field.name);
        REQUIRE(Pelican::Schema::resolveValue(field, entry.accepted, path) ==
                entry.resolved);
        requireSchemaError(
            [&] {
                (void)Pelican::Schema::resolveValue(field, entry.rejected,
                                                    path);
            },
            entry.rejected_code, path);
    }

    auto ranged = declaration(LeafType::Vec2);
    ranged.range = Pelican::Schema::FloatingRange{-2.0, 2.0};
    requireSchemaError(
        [&] {
            (void)Pelican::Schema::resolveValue(
                ranged, Json::array({1.0, 3.0}), "params.vec2");
        },
        Pelican::Schema::ErrorCode::OutOfRange, "params.vec2");
}

TEST_CASE("WP358 object resolver supplies defaults and canonical declaration order",
          "[wp358][schema][canonical]") {
    std::vector<Pelican::Schema::FieldDeclaration> fields;
    for (const auto type : Pelican::Schema::allFieldTypes) {
        fields.push_back(declaration(type));
    }
    const auto defaults = Pelican::Schema::resolveObject(fields, Json::object());
    REQUIRE(defaults.size() == 17);
    for (std::size_t index = 0; index < fields.size(); ++index) {
        REQUIRE(defaults.items().begin().key() == "i8");
        REQUIRE(defaults.contains(fields[index].name));
    }
    REQUIRE(defaults.dump().find("\"i8\"") < defaults.dump().find("\"u8\""));
    REQUIRE(defaults.at("f32") == 1.25);
    REQUIRE(defaults.at("enum") == "idle");

    auto supplied = Json::object();
    for (const auto &entry : valueCases()) {
        supplied[std::string{Pelican::Schema::fieldTypeName(entry.type)}] =
            entry.accepted;
    }
    const auto resolved = Pelican::Schema::resolveObject(fields, supplied);
    REQUIRE(resolved.at("f32") == 16777216.0);
    REQUIRE(resolved.at("vec2")[0] == 16777216.0);

    requireSchemaError(
        [&] { (void)Pelican::Schema::resolveObject(fields, Json::array()); },
        Pelican::Schema::ErrorCode::PayloadMustBeObject, "params");
    supplied["unknown"] = 1;
    requireSchemaError(
        [&] { (void)Pelican::Schema::resolveObject(fields, supplied); },
        Pelican::Schema::ErrorCode::UnknownField, "params.unknown");
    fields[0].default_value.reset();
    requireSchemaError(
        [&] { (void)Pelican::Schema::resolveObject(fields, Json::object()); },
        Pelican::Schema::ErrorCode::MissingField, "params.i8");
}

TEST_CASE("WP358 project wire parser delegates every type and rule to the leaf",
          "[wp358][schema][project]") {
    for (const auto &pair : MappingPairs) {
        Json wire{{"name", std::string{pair.name}},
                  {"type", std::string{pair.name}}};
        const auto source = declaration(pair.leaf);
        const auto &rule = Pelican::Schema::typeRule(pair.leaf);
        switch (rule.range_kind) {
        case Pelican::Schema::RangeKind::None: break;
        case Pelican::Schema::RangeKind::Signed:
            wire["range"] = Json::array({rule.signed_min, rule.signed_max});
            break;
        case Pelican::Schema::RangeKind::Unsigned:
            wire["range"] = Json::array({0, rule.unsigned_max});
            break;
        case Pelican::Schema::RangeKind::Floating:
            wire["range"] = Json::array({-100.0, 100.0});
            break;
        }
        if (pair.leaf == LeafType::Enum) wire["enum"] = source.enum_values;
        wire["default"] = *source.default_value;
        const auto parsed =
            Pelican::parseSchemaFieldDeclaration(wire, "project.fields[0]");
        REQUIRE(parsed.type == pair.leaf);
        REQUIRE(static_cast<bool>(parsed.name == pair.name));
    }

    const Json unknown{{"name", "future"}, {"type", "future_type"}};
    requireSchemaError(
        [&] {
            (void)Pelican::parseSchemaFieldDeclaration(
                unknown, "project.fields[4]");
        },
        Pelican::Schema::ErrorCode::UnknownTypeName,
        "project.fields[4].type");

    const Json invalid_range{{"name", "i8"},
                             {"type", "i8"},
                             {"range", Json::array({-129, 127})},
                             {"default", 0}};
    requireSchemaError(
        [&] {
            (void)Pelican::parseSchemaFieldDeclaration(
                invalid_range, "project.fields[5]");
        },
        Pelican::Schema::ErrorCode::RangeBoundNotRepresentable,
        "project.fields[5]");
    std::cout << "WP358_UNKNOWN_STRING=unknown_type_name"
              << " PATH=project.fields[4].type\n";
}
