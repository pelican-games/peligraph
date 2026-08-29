#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/communication/schemavocabularyadapter.hpp"
#include "../src/core/loader/authoringscenedocument.hpp"
#include "../src/core/userpublic/behavior.hpp"

#include <catch2/catch_test_macros.hpp>
#include <picosha2.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace Pelican;

constexpr std::string_view IntegerName = "wp358_integer_schema_types";
constexpr std::string_view FloatingName = "wp358_floating_schema_types";
constexpr std::string_view Vec2Name = "wp358_vec2_schema_type";
constexpr std::string_view Vec3Name = "wp358_vec3_schema_type";
constexpr std::string_view Vec4Name = "wp358_vec4_schema_type";
constexpr std::string_view QuatName = "wp358_quat_schema_type";
constexpr std::string_view OtherName = "wp358_other_schema_types";

enum class FixtureMode : std::uint8_t { Idle, Run };

struct IntegerParams {
    std::int8_t i8{};
    std::int16_t i16{};
    std::int32_t i32{};
    std::int64_t i64{};
    std::uint8_t u8{};
    std::uint16_t u16{};
    std::uint32_t u32{};
    std::uint64_t u64{};
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&IntegerParams::i8>("i8"), std::int8_t{-8}),
        defaulted(field<&IntegerParams::i16>("i16"), std::int16_t{-16}),
        defaulted(field<&IntegerParams::i32>("i32"), std::int32_t{-32}),
        defaulted(field<&IntegerParams::i64>("i64"), std::int64_t{-64}),
        defaulted(field<&IntegerParams::u8>("u8"), std::uint8_t{8}),
        defaulted(field<&IntegerParams::u16>("u16"), std::uint16_t{16}),
        defaulted(field<&IntegerParams::u32>("u32"), std::uint32_t{32}),
        defaulted(field<&IntegerParams::u64>("u64"), std::uint64_t{64}));
};

struct FloatingParams {
    float f32{};
    double f64{};
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&FloatingParams::f32>("f32"), 1.25F),
        defaulted(field<&FloatingParams::f64>("f64"), 2.5));
};

struct Vec2Params {
    vec2 value{};
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Vec2Params::value>("vec2"), vec2{1.0F, 2.0F}));
};

struct Vec3Params {
    vec3 value{};
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Vec3Params::value>("vec3"),
                  vec3{1.0F, 2.0F, 3.0F}));
};

struct Vec4Params {
    vec4 value{};
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Vec4Params::value>("vec4"),
                  vec4{1.0F, 2.0F, 3.0F, 4.0F}));
};

struct QuatParams {
    quat value{};
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&QuatParams::value>("quat"),
                  quat{0.0F, 0.0F, 0.0F, 1.0F}));
};

struct OtherParams {
    std::string string_value;
    bool bool_value{};
    FixtureMode enum_value{};
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&OtherParams::string_value>("string"), "fixture"),
        defaulted(field<&OtherParams::bool_value>("bool"), true),
        defaulted(field<&OtherParams::enum_value>("enum"), FixtureMode::Idle,
                  enumValues(enumValue("idle", FixtureMode::Idle),
                             enumValue("run", FixtureMode::Run))));
};

template <class ParamsType> class FixtureBehavior final : public Behavior {
  public:
    using Params = ParamsType;
};

using IntegerBehavior = FixtureBehavior<IntegerParams>;
using FloatingBehavior = FixtureBehavior<FloatingParams>;
using Vec2Behavior = FixtureBehavior<Vec2Params>;
using Vec3Behavior = FixtureBehavior<Vec3Params>;
using Vec4Behavior = FixtureBehavior<Vec4Params>;
using QuatBehavior = FixtureBehavior<QuatParams>;
using OtherBehavior = FixtureBehavior<OtherParams>;

class RegisteredWireFixture {
    std::vector<internal::RegistrationToken> tokens_;

    template <class BehaviorType> void add(std::string name) {
        tokens_.push_back(internal::getBehaviorRegisterer()
                              .registerBehavior<BehaviorType>(std::move(name),
                                                              1, {}));
    }

  public:
    RegisteredWireFixture() {
        add<IntegerBehavior>(std::string{IntegerName});
        add<FloatingBehavior>(std::string{FloatingName});
        add<Vec2Behavior>(std::string{Vec2Name});
        add<Vec3Behavior>(std::string{Vec3Name});
        add<Vec4Behavior>(std::string{Vec4Name});
        add<QuatBehavior>(std::string{QuatName});
        add<OtherBehavior>(std::string{OtherName});
    }

    RegisteredWireFixture(const RegisteredWireFixture &) = delete;
    RegisteredWireFixture &operator=(const RegisteredWireFixture &) = delete;

    ~RegisteredWireFixture() {
        for (auto token = tokens_.rbegin(); token != tokens_.rend(); ++token) {
            internal::unregisterBehavior(*token);
        }
    }

    internal::BehaviorRegistration &registration(std::string_view name) {
        const auto *found = internal::getBehaviorRegisterer().findByName(name);
        if (found == nullptr) throw std::logic_error("fixture behavior missing");
        return const_cast<internal::BehaviorRegistration &>(*found);
    }
};

AuthoringSceneDocument fixtureDocument() {
    return AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"name":"WP358Fixture","components":[{"name":"behavior","type":"wp358_integer_schema_types","params":{}},{"name":"behavior","type":"wp358_floating_schema_types","params":{}},{"name":"behavior","type":"wp358_vec2_schema_type","params":{}},{"name":"behavior","type":"wp358_vec3_schema_type","params":{}},{"name":"behavior","type":"wp358_vec4_schema_type","params":{}},{"name":"behavior","type":"wp358_quat_schema_type","params":{}},{"name":"behavior","type":"wp358_other_schema_types","params":{}}]}]}}})json",
        SceneRevision{1});
}

std::string runFixtureRpc(
    EditorCommandRpcAdapter::SchemaTypeNameResolver resolver = {}) {
    const auto document = fixtureDocument();
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{document.revision().value});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .resolved_scene = [&resolved]() -> const ResolvedScene & {
            return resolved;
        },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    EditorCommandRpcAdapter adapter{service, std::move(resolver)};
    std::istringstream input{
        R"json({"jsonrpc":"2.0","id":358,"method":"get_components","params":{"name":"WP358Fixture"}})json"
        "\n"};
    std::ostringstream output;
    RpcServer server{input, output};
    configureEditorRpcHandlers(
        server, adapter,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    server.run();
    return output.str();
}

std::string readBinary(std::string_view path) {
    std::ifstream input{std::string{path}, std::ios::binary};
    if (!input) throw std::runtime_error("failed to open WP358 fixture");
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

Json oneRpcEnvelope(const std::string &bytes) {
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(bytes.back() == '\n');
    REQUIRE(bytes.find('\n') == bytes.size() - 1);
    return Json::parse(bytes.begin(), bytes.end() - 1);
}

} // namespace

TEST_CASE("WP358 parent RpcServer fixture remains byte exact with all 17 types",
          "[wp358][schema][rpc][wire]") {
    RegisteredWireFixture registrations;
    const auto &integer_fingerprint =
        registrations.registration(IntegerName).params_schema_fingerprint;
    const auto integer_fingerprint_digest = picosha2::hash256_hex_string(
        integer_fingerprint.begin(), integer_fingerprint.end());
    REQUIRE(integer_fingerprint.size() == 585);
    REQUIRE(integer_fingerprint_digest ==
            "a935ab6465145eeea704463bd12e75ea1f167cd5858b44c588dbd7fa8d1b8ca5");
    const auto actual = runFixtureRpc();
    const auto expected = readBinary(
        PELICAN_WP358_RPC_FIXTURE);
    REQUIRE(actual == expected);
    REQUIRE(actual.size() == 3605);

    const auto envelope = oneRpcEnvelope(actual);
    std::vector<std::string> types;
    for (const auto &component : envelope.at("result").at("components")) {
        for (const auto &field : component.at("schema").at("fields")) {
            types.push_back(field.at("type").get<std::string>());
        }
    }
    REQUIRE(types == std::vector<std::string>{
                         "i8", "i16", "i32", "i64", "u8", "u16", "u32",
                         "u64", "f32", "f64", "vec2", "vec3", "vec4",
                         "quat", "string", "bool", "enum"});
    std::cout << "WP359_WP358_INTEGER_FINGERPRINT_BYTES="
              << integer_fingerprint.size()
              << " SHA256=" << integer_fingerprint_digest << '\n'
              << "WP358_WIRE_BYTES=" << actual.size()
              << " SHA256=8F5AC06031D34F9070E243E6143DFB094AB452D0B7CCE61BD6525F4104F56B63"
              << '\n';
}

TEST_CASE("WP358 rejecting resolver seam changes actual RPC output with named error",
          "[wp358][schema][rpc][sentinel]") {
    RegisteredWireFixture registrations;
    const auto baseline = runFixtureRpc();
    const auto sentinel = runFixtureRpc([](StructFieldType type) -> std::string {
        throw internal::SchemaTypeMappingError{
            internal::SchemaTypeMappingErrorCode::ResolverRejected,
            "WP358 sentinel rejected core ordinal " +
                std::to_string(static_cast<unsigned>(type))};
    });
    REQUIRE(sentinel != baseline);
    const auto envelope = oneRpcEnvelope(sentinel);
    REQUIRE(envelope.at("error").at("data").at("code") ==
            "schema_type_resolver_rejected");
    REQUIRE(envelope.at("error").at("message").get<std::string>().find(
                "schema_mapping_error[schema_type_resolver_rejected]") !=
            std::string::npos);
    std::cout << "WP358_SENTINEL_RPC=" << sentinel;
}

TEST_CASE("WP358 fake registration invalid ordinal becomes a named engine RPC error",
          "[wp358][schema][rpc][ordinal]") {
    RegisteredWireFixture registrations;
    auto &schema = registrations.registration(IntegerName).params_schema;
    REQUIRE_FALSE(schema.empty());
    schema.front().type = static_cast<StructFieldType>(255);
    const auto bytes = runFixtureRpc();
    const auto envelope = oneRpcEnvelope(bytes);
    REQUIRE(envelope.at("error").at("data").at("code") ==
            "invalid_core_schema_type_ordinal");
    REQUIRE(envelope.at("error").at("message").get<std::string>().find(
                "core StructFieldType ordinal 255 is invalid") !=
            std::string::npos);
    std::cout << "WP358_INVALID_ORDINAL_RPC=" << bytes;
}
