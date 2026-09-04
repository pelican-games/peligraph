#include "../src/core/communication/editorcommandservice.hpp"
#if PELICAN_WITH_RPC
#include "../src/core/communication/editorrpchandlers.hpp"
#endif
#include "../src/core/communication/editorprojectionplan.hpp"
#if PELICAN_WITH_RPC
#include "../src/core/communication/rpcserver.hpp"
#endif
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/editorprojectiontransaction.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/userpublic/details/schema/structfieldjson.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <any>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OJson = nlohmann::ordered_json;

namespace frozen_struct_field_v1 {

enum class ErrorCode : std::uint8_t {
    PayloadMustBeObject,
    UnknownField,
    MissingField,
    TypeMismatch,
    OutOfRange,
    InvalidEnum,
};

class ValidationError : public std::runtime_error {
    ErrorCode error_code;
    std::string field_path;

  public:
    ValidationError(ErrorCode code, std::string path, std::string message)
        : std::runtime_error(std::move(message)), error_code(code),
          field_path(std::move(path)) {}
    ErrorCode code() const noexcept { return error_code; }
    std::string_view path() const noexcept { return field_path; }
};

} // namespace frozen_struct_field_v1

static_assert(std::is_base_of_v<std::runtime_error,
                                StructFieldValidationError>);
static_assert(sizeof(StructFieldValidationError) ==
              sizeof(frozen_struct_field_v1::ValidationError));
static_assert(alignof(StructFieldValidationError) ==
              alignof(frozen_struct_field_v1::ValidationError));
static_assert(std::is_constructible_v<
              StructFieldValidationError, StructFieldErrorCode,
              std::string, std::string>);

#if defined(_WIN32)
class FixtureDll final {
    HMODULE module_ = nullptr;

  public:
    explicit FixtureDll(const char *path) : module_{LoadLibraryA(path)} {
        REQUIRE(module_ != nullptr);
    }
    ~FixtureDll() {
        if (module_ != nullptr) FreeLibrary(module_);
    }
    template <class Function>
    Function symbol(const char *name) const {
        const auto address = GetProcAddress(module_, name);
        REQUIRE(address != nullptr);
        return reinterpret_cast<Function>(address);
    }
};
#endif

std::string fixtureBytes(std::string_view name) {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                      "test" / "fixtures" / "wp378" / name;
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

Json resolutionFixture(float znear = 5000.0F,
                       std::optional<float> zfar = std::nullopt,
                       float collider_radius = 0.0F) {
    Json camera{{"name", "camera"},
                {"type", "perspective"},
                {"yfov", 0.5F},
                {"znear", znear}};
    if (zfar) camera["zfar"] = *zfar;
    return Json{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes",
         {{"main",
           {{"objects",
             Json::array({
                 {{"name", "Padding0"}, {"components", Json::array()}},
                 {{"name", "Padding1"}, {"components", Json::array()}},
                 {{"name", "Padding2"}, {"components", Json::array()}},
                 {{"name", "Broken"},
                  {"components",
                   Json::array({
                       {{"name", "transform"},
                        {"pos", {0, 0, 0}},
                        {"rotation", {0, 0, 0, 1}},
                        {"scale", {1, 1, 1}}},
                       std::move(camera),
                       {{"name", "collider"},
                        {"shape", "sphere"},
                        {"radius", collider_radius}},
                   })}},
             })}}}}},
    };
}

const ResolvedObject &brokenObject(const ResolvedScene &resolved) {
    const auto *scene = resolved.findScene("main");
    REQUIRE(scene != nullptr);
    REQUIRE(scene->objects.size() == 4);
    return scene->objects.at(3);
}

ResolutionResult<ComponentCodecValue> decodeUnvalidatedCamera(
    const Json &json) {
    if (json.value("wp378_typed_failure", false)) {
        return ResolutionResult<ComponentCodecValue>::failure(
            *cameraCrossFieldViolation(5000.0F, 1000.0F));
    }
    CameraCodecData value;
    value.projection_specified = true;
    value.projection_kind = CameraProjectionKind::Perspective;
    value.yfov = json.at("yfov").get<float>();
    value.znear = json.at("znear").get<float>();
    value.zfar = json.at("zfar").get<float>();
    return ResolutionResult<ComponentCodecValue>::success(std::move(value));
}

OJson encodeUnvalidatedCamera(const ComponentCodecValue &value) {
    const auto &camera = std::any_cast<const CameraCodecData &>(value);
    return OJson{{"name", "camera"},
                 {"type", "perspective"},
                 {"yfov", *camera.yfov},
                 {"znear", *camera.znear},
                 {"zfar", *camera.zfar}};
}

const SchemaNode &noCameraFields() {
    static const SchemaNode schema = SchemaNode::fromFlat({});
    return schema;
}
void applyUnvalidatedCamera(const ComponentCodecValue &, void *) {}
ComponentCodecValue projectUnvalidatedCamera(const void *) {
    return CameraCodecData{};
}

ComponentCodec unvalidatedCameraCodec() {
    return ComponentCodec{
        "camera", ComponentCodecRuntimeKind::Camera,
        EditorProjectionPolicy::Camera, decodeUnvalidatedCamera,
        encodeUnvalidatedCamera, noCameraFields, applyUnvalidatedCamera,
        projectUnvalidatedCamera, nullptr, nullptr};
}

} // namespace

TEST_CASE("WP378 decode seam returns camera violations and preserves internal throws",
          "[WP378][resolution-result][decode][mutation]") {
    const auto &camera = requireComponentCodec("camera");
    auto invalid = camera.decodeAuthored(
        {{"name", "camera"}, {"type", "perspective"},
         {"yfov", 0.5F}, {"znear", 5000.0F}, {"zfar", 1000.0F}});
    REQUIRE_FALSE(invalid.hasValue());
    REQUIRE(invalid.error().code ==
            "camera.zfar_not_greater_than_znear@1");
    const auto direct_json = resolutionFailureJson(
        StoredResolutionFailure{invalid.error()});
    REQUIRE(direct_json.at("path") == "/znear");
    REQUIRE(direct_json.at("fields") ==
            OJson{{"znear", 5000.0F}, {"zfar", 1000.0F}});

    auto alias = camera.decodeAuthored(
        {{"name", "camera"}, {"near", 0.1F}});
    REQUIRE_FALSE(alias.hasValue());
    REQUIRE(alias.error().code == "camera.field_alias_not_supported@1");
    REQUIRE(alias.error().leaf_message ==
            "camera field 'near' is not supported in v1; use 'znear'");

    ComponentCodec missing_callback = camera;
    missing_callback.decode_authored = nullptr;
    REQUIRE_THROWS_AS(missing_callback.decodeAuthored({{"name", "camera"}}),
                      std::logic_error);
}

TEST_CASE("WP378 camera completion retains all four named leaf contracts",
          "[WP378][camera][completion][leaf-bytes][mutation]") {
    struct Case {
        Json camera;
        CameraProjectionSpec defaults;
        std::string code;
        std::string token;
        std::string message;
    };
    const auto perspective_defaults = CameraProjectionSpec{
        .kind = CameraProjectionKind::Perspective,
        .yfov = 0.5F,
        .znear = 0.1F,
        .zfar = 1000.0F,
        .xmag = 1.0F,
        .ymag = 1.0F,
    };
    auto orthographic_defaults = perspective_defaults;
    orthographic_defaults.kind = CameraProjectionKind::Orthographic;
    const std::array cases{
        Case{
            .camera = {{"name", "camera"}, {"type", "perspective"},
                       {"znear", 0.1F}, {"zfar", 1000.0F}},
            .defaults = orthographic_defaults,
            .code = "camera.yfov_required_for_perspective@1",
            .token = "yfov",
            .message =
                "camera.yfov is required when changing projection to perspective",
        },
        Case{
            .camera = {{"name", "camera"}, {"type", "orthographic"},
                       {"znear", 0.1F}, {"zfar", 1000.0F}},
            .defaults = perspective_defaults,
            .code = "camera.xmag_required_for_orthographic@1",
            .token = "xmag",
            .message =
                "camera.xmag is required when changing projection to orthographic",
        },
        Case{
            .camera = {{"name", "camera"}, {"type", "orthographic"},
                       {"xmag", 2.0F}, {"znear", 0.1F},
                       {"zfar", 1000.0F}},
            .defaults = perspective_defaults,
            .code = "camera.ymag_required_for_orthographic@1",
            .token = "ymag",
            .message =
                "camera.ymag is required when changing projection to orthographic",
        },
        Case{
            .camera = {{"name", "camera"}, {"type", "perspective"},
                       {"yfov", 0.5F}, {"znear", 5000.0F}},
            .defaults = perspective_defaults,
            .code = "camera.zfar_not_greater_than_znear@1",
            .token = "znear",
            .message =
                "camera zfar (1000) must be greater than znear (5000)",
        },
    };

    for (const auto &test : cases) {
        DYNAMIC_SECTION(test.code) {
            const auto document = AuthoringSceneDocument::load(
                Json{{"schema", "pelican.scene"},
                     {"version", 1},
                     {"scenes",
                      {{"main",
                        {{"objects",
                          Json::array({
                              {{"name", "Camera"},
                               {"components",
                                Json::array({test.camera})}},
                          })}}}}}}
                    .dump(),
                SceneRevision{378});
            const auto resolved = ResolvedSceneResolver::resolve(
                document, SceneResolverGeneration{378},
                ResolvedSceneDefaults{.camera_projection = test.defaults});
            const auto &component = resolved.scenes()
                                        .front().objects.front()
                                        .components.front();
            REQUIRE(component.runtime_resolution_error.has_value());
            const auto *typed = std::get_if<UserDataViolation>(
                &*component.runtime_resolution_error);
            REQUIRE(typed != nullptr);
            REQUIRE(typed->code == test.code);
            REQUIRE(typed->leaf_message == test.message);
            REQUIRE(resolutionJsonPointer(typed->tokens) ==
                    "/scenes/main/objects/0/components/0/" + test.token);
        }
    }
}

TEST_CASE("WP378 resolver retains exact typed and legacy failures and consumes repaired values",
          "[WP378][resolver][exact-json][mutation]") {
    const auto document = AuthoringSceneDocument::load(
        resolutionFixture(5000.0F, 1000.0F, 0.0F).dump(),
        SceneRevision{7});
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{9});
    const auto &object = brokenObject(resolved);
    REQUIRE(object.components.size() == 3);

    const auto &camera = object.components.at(1);
    REQUIRE(camera.runtime_resolution_error.has_value());
    const auto camera_json =
        resolutionFailureJson(*camera.runtime_resolution_error);
    REQUIRE(camera_json.dump() + "\n" ==
            fixtureBytes("camera_resolution_error.jsonl"));
    REQUIRE_FALSE(camera_json.contains("primary_source"));
    REQUIRE_FALSE(camera_json.contains("related_sources"));

    const auto &collider = object.components.at(2);
    REQUIRE(collider.runtime_resolution_error.has_value());
    const auto collider_json =
        resolutionFailureJson(*collider.runtime_resolution_error);
    REQUIRE(collider_json.dump() + "\n" ==
            fixtureBytes("collider_resolution_error.jsonl"));
    REQUIRE_FALSE(collider_json.contains("code"));
    REQUIRE_FALSE(collider_json.contains("path"));
    REQUIRE_FALSE(collider_json.contains("fields"));

    REQUIRE_THROWS_AS(camera.requireRuntimeValue(),
                      ResolutionFailureException);
    REQUIRE_THROWS_AS(collider.requireRuntimeValue(),
                      ResolutionFailureException);

    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .resolved_scene = [&resolved]() -> const ResolvedScene & {
            return resolved;
        },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    const auto query = editorQueryJson(service.getComponents(
        {.scene_id = "main", .name = "Broken"}));
    REQUIRE(query.at("components").at(1).at("resolution_error") ==
            camera_json);
    REQUIRE(query.at("components").at(2).at("resolution_error") ==
            collider_json);

#if PELICAN_WITH_RPC
    EditorCommandRpcAdapter rpc_adapter{service};
    std::istringstream rpc_input;
    std::ostringstream rpc_output;
    RpcServer rpc{rpc_input, rpc_output};
    configureEditorRpcHandlers(
        rpc, rpc_adapter,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    const auto rpc_response = OJson::parse(rpc.processLine(
        R"json({"jsonrpc":"2.0","id":378,"method":"get_components","params":{"scene_id":"main","name":"Broken"}})json"));
    const auto &rpc_components = rpc_response.at("result").at("components");
    REQUIRE(rpc_components.at(1).at("resolution_error").dump() + "\n" ==
            fixtureBytes("camera_resolution_error.jsonl"));
    REQUIRE(rpc_components.at(2).at("resolution_error").dump() + "\n" ==
            fixtureBytes("collider_resolution_error.jsonl"));
#endif

    EditorComponentQueryResult generated;
    generated.generated = GeneratedComponentOrigin{
        .stable_generated_id = "g", .prefab = "p", .instance_id = "i",
        .component_key = "camera", .digest_sha256 = "d"};
    generated.generated_resolved_json = OJson{{"name", "camera"}};
    generated.resolution_failure = camera.runtime_resolution_error;
    const auto generated_json = editorQueryJson(generated);
    REQUIRE(generated_json.at("resolution_error") == camera_json);
    REQUIRE(generated_json.contains("generated"));

    const auto repaired_document = AuthoringSceneDocument::load(
        resolutionFixture(500.0F, 6000.0F, 1.0F).dump(),
        SceneRevision{8});
    const auto repaired = ResolvedSceneResolver::resolve(
        repaired_document, SceneResolverGeneration{10});
    const auto &repaired_camera = brokenObject(repaired).components.at(1);
    REQUIRE_FALSE(repaired_camera.runtime_resolution_error);
    const auto &runtime = std::any_cast<const CameraCodecData &>(
        repaired_camera.requireRuntimeValue());
    REQUIRE(*runtime.znear == 500.0F);
    REQUIRE(*runtime.zfar == 6000.0F);
}

TEST_CASE("WP378 projection planning uses a distinct move-only failure value",
          "[WP378][projection-plan][resolution-result]") {
    test_support::SceneProjectionTarget target{
        AuthoringSceneDocument::load(
            resolutionFixture(0.1F, 1000.0F, 1.0F).dump(),
            SceneRevision{20})};
    std::vector<ComponentCodec> codecs{componentCodecs().begin(),
                                       componentCodecs().end()};
    const auto defaults = componentCodecCanonicalDefaults();
    constexpr auto layout_mode = EditorLayoutSemanticMode::Unconfigured;
    const auto layout_generation = editorProjectionCodecGeneration(
        codecs, layout_mode, defaults);
    const std::vector<EditorBehaviorPolicy> behaviors;
    const auto behavior_generation = editorProjectionBehaviorGeneration(
        behaviors, EditorBehaviorAvailability::DllUnavailable);
    const EditorProjectionContext context{
        "main", EditorProjectionMode::Committed, std::move(codecs),
        layout_mode, layout_generation,
        EditorBehaviorAvailability::DllUnavailable, behaviors,
        behavior_generation, defaults};
    auto result = planEditorProjection(
        EditorProjectionPlanningRequest{
            .base_revision = target.projectionState().revision(),
            .restore_committed = true,
        },
        target.projectionState(), context);
    REQUIRE_FALSE(result.hasValue());
    REQUIRE(std::holds_alternative<EditorProjectionRecoverable>(
        result.error()));
}

TEST_CASE("WP378 transaction completion seam maps typed failure and consumes success",
          "[WP378][transaction][camera][mutation]") {
    test_support::SceneProjectionTarget target{
        AuthoringSceneDocument::load(
            resolutionFixture(0.1F, 1000.0F, 1.0F).dump(),
            SceneRevision{30})};
    std::vector<ComponentCodec> replacement{unvalidatedCameraCodec()};
    testing::ScopedComponentCodecCatalog catalog{std::move(replacement)};

    const auto result_failure_command = makeSetComponentValueCommand(
        "main", "Broken", "camera",
        {{"name", "camera"}, {"type", "perspective"},
         {"yfov", 0.5F}, {"znear", 0.1F}, {"zfar", 1000.0F},
         {"wp378_typed_failure", true}});
    EditorProjectionTransaction result_failure_transaction{
        target, target.projectionState().revision()};
    const std::array result_failure_commands{result_failure_command};
    std::vector<EditorProjectionAdapter *> no_adapters;
    const auto result_failure = result_failure_transaction.commit(
        result_failure_commands, no_adapters);
    REQUIRE(result_failure.status == EditorProjectionStatus::Rejected);
    REQUIRE(result_failure.error.has_value());
    REQUIRE(result_failure.error->code ==
            EditorProjectionErrorCode::CommandInvalid);
    REQUIRE(result_failure.error->data.at("resolution_error").at("code") ==
            "camera.zfar_not_greater_than_znear@1");
    REQUIRE(target.projectionState().revision() == SceneRevision{30});

    const auto invalid_command = makeSetComponentValueCommand(
        "main", "Broken", "camera",
        {{"name", "camera"}, {"type", "perspective"},
         {"yfov", 0.5F}, {"znear", 5000.0F}, {"zfar", 1000.0F}});
    EditorProjectionTransaction invalid_transaction{
        target, target.projectionState().revision()};
    const std::array invalid_commands{invalid_command};
    const auto rejected = invalid_transaction.commit(invalid_commands,
                                                      no_adapters);
    REQUIRE(rejected.status == EditorProjectionStatus::Rejected);
    REQUIRE(rejected.error.has_value());
    REQUIRE(rejected.error->code == EditorProjectionErrorCode::CommandInvalid);
    REQUIRE(rejected.error->data.at("resolution_error").at("code") ==
            "camera.zfar_not_greater_than_znear@1");
    REQUIRE(target.projectionState().revision() == SceneRevision{30});

    const auto valid_command = makeSetComponentValueCommand(
        "main", "Broken", "camera",
        {{"name", "camera"}, {"type", "perspective"},
         {"yfov", 0.75F}, {"znear", 2.0F}, {"zfar", 20.0F}});
    EditorProjectionTransaction valid_transaction{
        target, target.projectionState().revision()};
    const std::array valid_commands{valid_command};
    const auto committed = valid_transaction.commit(valid_commands,
                                                     no_adapters);
    REQUIRE(committed.committed());
    const auto &camera = target.document.rawJson()
                             .at("scenes").at("main").at("objects").at(3)
                             .at("components").at(1);
    REQUIRE(camera.at("yfov") == 0.75F);
    REQUIRE(camera.at("znear") == 2.0F);
    REQUIRE(camera.at("zfar") == 20.0F);
}

#if PELICAN_WITH_RPC
TEST_CASE("WP378 load_scene RPC emits exact typed and legacy error data",
          "[WP378][rpc][load-scene][exact-json][mutation]") {
    const auto call = [](StoredResolutionFailure failure) {
        std::istringstream input;
        std::ostringstream output;
        RpcServer server{input, output};
        server.setHandler(
            "load_scene",
            [failure = std::move(failure)](const Json &params) -> Json {
                internal::loadSceneResolutionBoundary(
                    params.at("name").get<std::string>(),
                    [&failure](std::string_view) {
                        throw ResolutionFailureException{failure};
                    });
                return Json::object();
            });
        return OJson::parse(server.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", 378},
                 {"method", "load_scene"},
                 {"params", {{"name", "main"}}}}
                .dump()));
    };

    const auto typed = call(UserDataViolation{
        .code = "camera.zfar_not_greater_than_znear@1",
        .tokens = {"scenes", "main", "objects", "3", "components", "1",
                   "znear"},
        .fields = OJson{{"znear", 5000.0F}, {"zfar", 1000.0F}},
        .leaf_message =
            "camera zfar (1000) must be greater than znear (5000)",
    });
    REQUIRE(typed.at("error").at("code") == -32000);
    REQUIRE(typed.at("error").at("data").dump() + "\n" ==
            fixtureBytes("camera_resolution_error.jsonl"));

    const auto legacy = call(LegacyResolutionText{
        .legacy_branch_id = "collider.radius_non_positive",
        .message = "Sphere collider radius must be positive",
    });
    REQUIRE(legacy.at("error").at("code") == -32000);
    REQUIRE(legacy.at("error").at("data").dump() + "\n" ==
            fixtureBytes("collider_resolution_error.jsonl"));
}
#endif

#if defined(_WIN32)
TEST_CASE("WP378 frozen struct-field header throws across the DLL ABI",
          "[WP378][abi][dll][mutation]") {
    FixtureDll fixture{PELICAN_WP378_STRUCTFIELD_ABI_DLL};
    using Thrower = void (*)();
    const auto get_thrower = fixture.symbol<Thrower (*)()>(
        "pelican_wp378_struct_field_error_thrower");
    const auto throw_old = get_thrower();
    REQUIRE(throw_old != nullptr);
    try {
        throw_old();
        FAIL("the frozen-header client did not throw");
    } catch (const StructFieldValidationError &error) {
        REQUIRE(error.code() == StructFieldErrorCode::OutOfRange);
        REQUIRE(std::string{error.path()} == "camera.znear");
        REQUIRE(std::string{error.what()} ==
                "field 'camera.znear' must be greater than zero");
    }
}
#endif

} // namespace Pelican
