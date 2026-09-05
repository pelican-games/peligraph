#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/userpublic/details/schema/structfieldjson.hpp"

#include <optional>
#include <string>
#include <type_traits>

namespace {

using namespace Pelican;
using namespace Pelican::internal;

enum class InstanceFlag : std::uint32_t {
    CullDisable = 1u,
    FlipFacing = 2u,
    ForceOpaque = 4u,
};

struct OverrideRow {
    std::string occurrence_id;
    std::optional<std::uint8_t> mask;
    std::optional<Flags<InstanceFlag>> instance_flags;
    std::optional<std::uint32_t> custom_index;

    static constexpr auto schema = structFields(
        componentPolicy("wp382a_override_row"),
        required(field<&OverrideRow::occurrence_id>("occurrence_id")),
        optionalOverride(field<&OverrideRow::mask>("mask", urange(0, 255))),
        optionalOverride(
            field<&OverrideRow::instance_flags>("instance_flags"),
            enumValues(enumValue("cull_disable", InstanceFlag::CullDisable),
                       enumValue("flip_facing", InstanceFlag::FlipFacing),
                       enumValue("force_opaque", InstanceFlag::ForceOpaque))),
        optionalOverride(field<&OverrideRow::custom_index>(
            "custom_index", urange(0, 0x00ffffffu))));
};

struct CarrierParams {
    List<OverrideRow> overrides;
    bool enabled = false;

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&CarrierParams::overrides>("overrides")),
        defaulted(field<&CarrierParams::enabled>("enabled"), false));
};

struct NegativeZeroRange {
    float value = 0;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&NegativeZeroRange::value>("value", frange(-0.0, 1.0)),
                  0.0f));
};

struct PositiveZeroRange {
    float value = 0;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&PositiveZeroRange::value>("value", frange(+0.0, 1.0)),
                  0.0f));
};

template <class Fn>
void requireSchemaFailure(Fn &&fn, StructFieldErrorCode code,
                          std::string_view text) {
    try {
        fn();
        FAIL("expected StructFieldValidationError");
    } catch (const StructFieldValidationError &error) {
        REQUIRE(error.code() == code);
        REQUIRE(std::string{error.what()}.find(text) != std::string::npos);
    }
}

nlohmann::json validParams() {
    return nlohmann::json::parse(R"json({
      "overrides": {
        "$list": {"v": 1, "next_id": 3},
        "rows": [
          {"$row": {"id": 1, "v": 1, "hint": {"node": "chair"}},
           "occurrence_id": "occ-a", "mask": 0,
           "instance_flags": []},
          {"$row": {"id": 2, "v": 1},
           "occurrence_id": "occ-b", "custom_index": 7}
        ]
      },
      "enabled": true
    })json");
}

} // namespace

TEST_CASE("WP382a recursive List and Flags round-trip the object container",
          "[WP382a][schema][wire]") {
    static_assert(schemaRequiresV3(CarrierParams::schema));
    CarrierParams decoded;
    decodeBehaviorParams(validParams(), decoded, CarrierParams::schema);

    REQUIRE(decoded.overrides.next_id == 3);
    REQUIRE(decoded.overrides.rows.size() == 2);
    REQUIRE(decoded.overrides.rows[0].envelope.id == 1);
    REQUIRE(decoded.overrides.rows[0].value.mask == 0);
    REQUIRE(decoded.overrides.rows[0].value.instance_flags.has_value());
    REQUIRE(decoded.overrides.rows[0].value.instance_flags->bits() == 0);
    REQUIRE_FALSE(decoded.overrides.rows[0].value.custom_index.has_value());
    REQUIRE_FALSE(decoded.overrides.rows[1].value.mask.has_value());
    REQUIRE(decoded.overrides.rows[1].value.custom_index == 7);

    const auto encoded = encodeBehaviorParams(decoded, CarrierParams::schema);
    REQUIRE(nlohmann::json::parse(encoded.dump()) == validParams());
    const auto node = materializeSchemaNode(CarrierParams::schema);
    static_assert(std::is_trivially_destructible_v<SchemaNode>);
    REQUIRE(node.root().tag == SchemaNodeTag::Struct);
    const auto list = node.childIndex(0, 0);
    const auto row = node.childIndex(list, 0);
    REQUIRE(node.at(list).tag == SchemaNodeTag::List);
    REQUIRE(node.at(node.childIndex(row, 2)).tag == SchemaNodeTag::Flags);
    REQUIRE(node.at(node.childIndex(row, 1)).presence ==
            StructFieldPresence::OptionalOverride);

    auto one_flag = validParams();
    one_flag["overrides"]["rows"][0]["instance_flags"] =
        nlohmann::json::array({"cull_disable"});
    CarrierParams one_flag_value;
    decodeBehaviorParams(one_flag, one_flag_value, CarrierParams::schema);
    REQUIRE(one_flag_value.overrides.rows[0].value.instance_flags->contains(
        InstanceFlag::CullDisable));
    REQUIRE(nlohmann::json::parse(
                encodeBehaviorParams(one_flag_value, CarrierParams::schema)
                    .dump()) == one_flag);
}

TEST_CASE("WP382a fingerprint canonicalizes signed zero range endpoints",
          "[WP382a][schema][fingerprint]") {
    constexpr auto negative = materializeSchemaNode(NegativeZeroRange::schema);
    constexpr auto positive = materializeSchemaNode(PositiveZeroRange::schema);
    REQUIRE(schemaNodeFingerprint(negative) == schemaNodeFingerprint(positive));
}

TEST_CASE("WP382a list grammar has named failures",
          "[WP382a][schema][wire][negative]") {
    auto bare = validParams();
    bare["overrides"] = nlohmann::json::array();
    requireSchemaFailure(
        [&] { CarrierParams out; decodeBehaviorParams(bare, out, CarrierParams::schema); },
        StructFieldErrorCode::ListContainerMissing, "ListContainerMissing");

    auto no_list = validParams();
    no_list["overrides"].erase("$list");
    requireSchemaFailure(
        [&] { CarrierParams out; decodeBehaviorParams(no_list, out, CarrierParams::schema); },
        StructFieldErrorCode::RowEnvelopeMissing, "RowEnvelopeMissing");

    auto unknown_version = validParams();
    unknown_version["overrides"]["$list"]["v"] = 2;
    requireSchemaFailure(
        [&] { CarrierParams out; decodeBehaviorParams(unknown_version, out, CarrierParams::schema); },
        StructFieldErrorCode::RowEnvelopeUnsupported,
        "RowEnvelopeUnsupported");

    auto duplicate = validParams();
    duplicate["overrides"]["rows"][1]["$row"]["id"] = 1;
    requireSchemaFailure(
        [&] { CarrierParams out; decodeBehaviorParams(duplicate, out, CarrierParams::schema); },
        StructFieldErrorCode::DuplicateRowId, "DuplicateRowId");

    auto no_row = validParams();
    no_row["overrides"]["rows"][0].erase("$row");
    requireSchemaFailure(
        [&] { CarrierParams out; decodeBehaviorParams(no_row, out, CarrierParams::schema); },
        StructFieldErrorCode::RowEnvelopeMissing, "RowEnvelopeMissing");

    auto no_next_id = validParams();
    no_next_id["overrides"]["$list"].erase("next_id");
    requireSchemaFailure(
        [&] { CarrierParams out; decodeBehaviorParams(no_next_id, out, CarrierParams::schema); },
        StructFieldErrorCode::RowEnvelopeMissing, "RowEnvelopeMissing");

    auto reused_next_id = validParams();
    reused_next_id["overrides"]["$list"]["next_id"] = 1;
    requireSchemaFailure(
        [&] { CarrierParams out; decodeBehaviorParams(reused_next_id, out, CarrierParams::schema); },
        StructFieldErrorCode::OutOfRange, "$list.next_id must exceed every row id");

    CarrierParams typed;
    decodeBehaviorParams(validParams(), typed, CarrierParams::schema);
    typed.overrides.rows[1].envelope.id = 1;
    requireSchemaFailure(
        [&] { (void)encodeBehaviorParams(typed, CarrierParams::schema); },
        StructFieldErrorCode::DuplicateRowId, "DuplicateRowId");

    decodeBehaviorParams(validParams(), typed, CarrierParams::schema);
    typed.overrides.rows[0].envelope.version = 2;
    requireSchemaFailure(
        [&] { (void)encodeBehaviorParams(typed, CarrierParams::schema); },
        StructFieldErrorCode::RowEnvelopeUnsupported,
        "RowEnvelopeUnsupported");

    decodeBehaviorParams(validParams(), typed, CarrierParams::schema);
    typed.overrides.next_id = 2;
    requireSchemaFailure(
        [&] { (void)encodeBehaviorParams(typed, CarrierParams::schema); },
        StructFieldErrorCode::OutOfRange,
        "$list.next_id must exceed every row id");

    decodeBehaviorParams(validParams(), typed, CarrierParams::schema);
    typed.overrides.rows[0].envelope.hint_json = "[]";
    requireSchemaFailure(
        [&] { (void)encodeBehaviorParams(typed, CarrierParams::schema); },
        StructFieldErrorCode::TypeMismatch, "must be an object");

    decodeBehaviorParams(validParams(), typed, CarrierParams::schema);
    typed.overrides.rows[0].envelope.id = UINT32_MAX;
    typed.overrides.next_id = UINT32_MAX;
    requireSchemaFailure(
        [&] { (void)encodeBehaviorParams(typed, CarrierParams::schema); },
        StructFieldErrorCode::RowIdExhausted, "RowIdExhausted");
}

TEST_CASE("WP382d ray overrides require the List envelope",
          "[WP382a][ray][enablement]") {
    const auto *codec = testing::findComponentCodecIncludingDisabled(
        "pelican_ray_instance");
    REQUIRE(codec != nullptr);
    const auto rejected = codec->decodeAuthored(nlohmann::json{
            {"name", "pelican_ray_instance"},
            {"mask", 1},
            {"overrides", nlohmann::json::object()},
        });
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().leaf_message.find("RowEnvelopeMissing") != std::string::npos);
    const nlohmann::json supported{{"name", "pelican_ray_instance"}, {"mask", 1}};
    auto overridden = supported;
    overridden["overrides"] = {{"$list", {{"v", 1}, {"next_id", 1}}},
                               {"rows", nlohmann::json::array()}};
    REQUIRE(codec->decodeAuthored(overridden));
    for (const auto surface : {ComponentDecodeSurface::preview, ComponentDecodeSurface::web}) {
        REQUIRE(codec->decodeAuthored(supported, surface));
        const auto unsupported = codec->decodeAuthored(overridden, surface);
        REQUIRE_FALSE(unsupported);
        CHECK(unsupported.error().code == "ray_instance_overrides_unsupported@1");
        CHECK(unsupported.error().tokens == std::vector<std::string>{"overrides"});
        CHECK(unsupported.error().fields.at("surface") ==
              (surface == ComponentDecodeSurface::preview ? "preview" : "web"));
    }
}
