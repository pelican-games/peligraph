#include "../../../src/core/communication/editorcommandservice.hpp"
#include "../../../src/core/communication/editorrpchandlers.hpp"
#include "../../../src/core/communication/rpcserver.hpp"
#include "../../../src/core/loader/authoringscenedocument.hpp"
#include "../../../src/core/userpublic/behavior.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

enum class FixtureMode : std::uint8_t { Idle, Run };

struct IntegerSchemaTypesParams {
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
        defaulted(field<&IntegerSchemaTypesParams::i8>("i8"), std::int8_t{-8}),
        defaulted(field<&IntegerSchemaTypesParams::i16>("i16"), std::int16_t{-16}),
        defaulted(field<&IntegerSchemaTypesParams::i32>("i32"), std::int32_t{-32}),
        defaulted(field<&IntegerSchemaTypesParams::i64>("i64"), std::int64_t{-64}),
        defaulted(field<&IntegerSchemaTypesParams::u8>("u8"), std::uint8_t{8}),
        defaulted(field<&IntegerSchemaTypesParams::u16>("u16"), std::uint16_t{16}),
        defaulted(field<&IntegerSchemaTypesParams::u32>("u32"), std::uint32_t{32}),
        defaulted(field<&IntegerSchemaTypesParams::u64>("u64"), std::uint64_t{64}));
};

struct FloatingSchemaTypesParams {
    float f32{};
    double f64{};

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&FloatingSchemaTypesParams::f32>("f32"), 1.25F),
        defaulted(field<&FloatingSchemaTypesParams::f64>("f64"), 2.5));
};

struct Vec2SchemaTypeParams {
    float placeholder{};

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Vec2SchemaTypeParams::placeholder>("placeholder"),
                  0.0F));
};

struct Vec3SchemaTypeParams {
    float placeholder{};

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Vec3SchemaTypeParams::placeholder>("placeholder"),
                  0.0F));
};

struct Vec4SchemaTypeParams {
    float placeholder{};

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Vec4SchemaTypeParams::placeholder>("placeholder"),
                  0.0F));
};

struct QuatSchemaTypeParams {
    float placeholder{};

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&QuatSchemaTypeParams::placeholder>("placeholder"),
                  0.0F));
};

struct OtherSchemaTypesParams {
    std::string string_value;
    bool bool_value{};
    FixtureMode enum_value{};

    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&OtherSchemaTypesParams::string_value>("string"), "fixture"),
        defaulted(field<&OtherSchemaTypesParams::bool_value>("bool"), true),
        defaulted(field<&OtherSchemaTypesParams::enum_value>("enum"), FixtureMode::Idle,
                  enumValues(enumValue("idle", FixtureMode::Idle),
                             enumValue("run", FixtureMode::Run))));
};

class IntegerSchemaTypesBehavior final : public Behavior {
  public:
    using Params = IntegerSchemaTypesParams;
};

class FloatingSchemaTypesBehavior final : public Behavior {
  public:
    using Params = FloatingSchemaTypesParams;
};

class Vec2SchemaTypeBehavior final : public Behavior {
  public:
    using Params = Vec2SchemaTypeParams;
};

class Vec3SchemaTypeBehavior final : public Behavior {
  public:
    using Params = Vec3SchemaTypeParams;
};

class Vec4SchemaTypeBehavior final : public Behavior {
  public:
    using Params = Vec4SchemaTypeParams;
};

class QuatSchemaTypeBehavior final : public Behavior {
  public:
    using Params = QuatSchemaTypeParams;
};

class OtherSchemaTypesBehavior final : public Behavior {
  public:
    using Params = OtherSchemaTypesParams;
};

void installVectorWireFixture(std::string_view stable_name,
                              StructFieldType type,
                              std::string_view field_name,
                              internal::BehaviorCanonicalizeParamsFn canonicalize) {
    const auto *found =
        internal::getBehaviorRegisterer().findByName(stable_name);
    if (found == nullptr) {
        throw std::runtime_error("vector fixture registration is missing");
    }
    // The parent revision's typed vector encoder rejects its own defaults
    // (structfieldjson.hpp:321/326/331). Keep the parent tree byte-for-byte
    // clean and alter only this process-local wire fixture registration.
    auto &registration = const_cast<internal::BehaviorRegistration &>(*found);
    registration.params_schema = {StructFieldSchema{field_name, type, {}}};
    registration.canonicalize_params = canonicalize;
}

} // namespace
} // namespace Pelican

int main(int argc, char **argv) {
    using namespace Pelican;
    std::cerr << "capture:main\n";
    if (argc != 2) {
        throw std::invalid_argument("capture_parent_rpc requires one output path");
    }
    std::ofstream progress{std::string{argv[1]} + ".progress",
                           std::ios::binary};
    progress << "main\n" << std::flush;
    auto &behavior_registerer = internal::getBehaviorRegisterer();
    try {
    progress << "registering-integers\n" << std::flush;
    (void)behavior_registerer.registerBehavior<IntegerSchemaTypesBehavior>(
        "wp358_integer_schema_types", 1, {});
    progress << "registered-integers\n" << std::flush;
    (void)behavior_registerer.registerBehavior<FloatingSchemaTypesBehavior>(
        "wp358_floating_schema_types", 1, {});
    progress << "registered-floating\n" << std::flush;
    (void)behavior_registerer.registerBehavior<Vec2SchemaTypeBehavior>(
        "wp358_vec2_schema_type", 1, {});
    progress << "registered-vec2\n" << std::flush;
    (void)behavior_registerer.registerBehavior<Vec3SchemaTypeBehavior>(
        "wp358_vec3_schema_type", 1, {});
    progress << "registered-vec3\n" << std::flush;
    (void)behavior_registerer.registerBehavior<Vec4SchemaTypeBehavior>(
        "wp358_vec4_schema_type", 1, {});
    progress << "registered-vec4\n" << std::flush;
    (void)behavior_registerer.registerBehavior<QuatSchemaTypeBehavior>(
        "wp358_quat_schema_type", 1, {});
    progress << "registered-quat\n" << std::flush;
    (void)behavior_registerer.registerBehavior<OtherSchemaTypesBehavior>(
        "wp358_other_schema_types", 1, {});
    installVectorWireFixture(
        "wp358_vec2_schema_type", StructFieldType::Vec2, "vec2",
        [](const nlohmann::json &) { return std::string{R"({"vec2":[1.0,2.0]})"}; });
    installVectorWireFixture(
        "wp358_vec3_schema_type", StructFieldType::Vec3, "vec3",
        [](const nlohmann::json &) { return std::string{R"({"vec3":[1.0,2.0,3.0]})"}; });
    installVectorWireFixture(
        "wp358_vec4_schema_type", StructFieldType::Vec4, "vec4",
        [](const nlohmann::json &) { return std::string{R"({"vec4":[1.0,2.0,3.0,4.0]})"}; });
    installVectorWireFixture(
        "wp358_quat_schema_type", StructFieldType::Quat, "quat",
        [](const nlohmann::json &) { return std::string{R"({"quat":[0.0,0.0,0.0,1.0]})"}; });
    progress << "registered\n" << std::flush;
    } catch (const std::exception &error) {
        progress << "registration-error: " << error.what() << "\n"
                 << std::flush;
        throw;
    }

    const auto document = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"name":"WP358Fixture","components":[{"name":"behavior","type":"wp358_integer_schema_types","params":{}},{"name":"behavior","type":"wp358_floating_schema_types","params":{}},{"name":"behavior","type":"wp358_vec2_schema_type","params":{}},{"name":"behavior","type":"wp358_vec3_schema_type","params":{}},{"name":"behavior","type":"wp358_vec4_schema_type","params":{}},{"name":"behavior","type":"wp358_quat_schema_type","params":{}},{"name":"behavior","type":"wp358_other_schema_types","params":{}}]}]}}})json",
        SceneRevision{1});
    std::cerr << "capture:document\n";
    progress << "document\n" << std::flush;
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    EditorCommandRpcAdapter adapter{service};
    std::cerr << "capture:adapter\n";
    progress << "adapter\n" << std::flush;
    std::istringstream input{
        R"json({"jsonrpc":"2.0","id":358,"method":"get_components","params":{"name":"WP358Fixture"}})json"
        "\n"};
    std::ostringstream output;
    RpcServer server{input, output};
    std::cerr << "capture:server\n";
    progress << "server\n" << std::flush;
    configureEditorRpcHandlers(
        server, adapter,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    std::cerr << "capture:handlers\n";
    progress << "handlers\n" << std::flush;
    server.run();
    std::cerr << "capture:run\n";
    progress << "run\n" << std::flush;

    const std::string bytes = output.str();
    std::ofstream fixture{argv[1], std::ios::binary};
    if (!fixture) {
        throw std::runtime_error("failed to open capture output");
    }
    fixture.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::cerr << "capture:wrote\n";
    progress << "wrote\n" << std::flush;
    if (!fixture) {
        throw std::runtime_error("failed to write capture output");
    }
}
