#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/userpublic/details/schema/structfieldjson.hpp"

#include <optional>
#include <string>

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
    REQUIRE(node.tag == SchemaNodeTag::Struct);
    REQUIRE(node.children.at(0).tag == SchemaNodeTag::List);
    REQUIRE(node.children.at(0).children.at(0).children.at(2).tag ==
            SchemaNodeTag::Flags);
    REQUIRE(node.children.at(0).children.at(0).children.at(1).presence ==
            StructFieldPresence::OptionalOverride);
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
}

TEST_CASE("WP382a ray overrides remain gated",
          "[WP382a][ray][enablement]") {
    const auto *codec = testing::findComponentCodecIncludingDisabled(
        "pelican_ray_instance");
    REQUIRE(codec != nullptr);
    REQUIRE_THROWS_WITH(
        codec->decodeAuthored(nlohmann::json{
            {"name", "pelican_ray_instance"},
            {"mask", 1},
            {"overrides", nlohmann::json::object()},
        }),
        Catch::Matchers::ContainsSubstring(
            "ray_instance_overrides_not_enabled@1"));
}
