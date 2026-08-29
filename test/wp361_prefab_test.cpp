#include "../src/core/build_features.hpp"
#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorjournal.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/editorruntimefactory.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/imgui/inspector.hpp"
#include "../src/core/loader/authoringsceneauthority.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/loader/editorprojectionadapters.hpp"
#include "../src/core/loader/editorprojectiontransaction.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/prefabprovider.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#include "../src/project/prefab.hpp"
#include "../src/project/projectformat.hpp"
#include "../src/project/sceneformat.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <picosha2.h>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OJson = nlohmann::ordered_json;

enum class Wp361Mode { none, y_axis, full };

struct Wp361Params {
    std::int32_t hp = 0;
    std::string target = "none";
    Wp361Mode mode = Wp361Mode::none;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Wp361Params::hp>("hp"), std::int32_t{0}),
        defaulted(field<&Wp361Params::target>("target"), "none"),
        defaulted(field<&Wp361Params::mode>("mode"), Wp361Mode::none,
                  enumValues(enumValue("none", Wp361Mode::none),
                             enumValue("y_axis", Wp361Mode::y_axis),
                             enumValue("full", Wp361Mode::full))));
};

std::vector<std::string> wp361_callback_trace;
std::vector<int> wp361_update_trace;
std::size_t wp361_destroy_count = 0;

struct Wp361bLifetimeParams {
    std::int32_t hp = 0;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Wp361bLifetimeParams::hp>("hp"),
                  std::int32_t{0}));
};

ECSCoreTemplatePublic *wp361b_lifetime_ecs = nullptr;
std::size_t wp361b_lifetime_init_count = 0;
std::vector<bool> wp361b_lifetime_destroy_alive;

class Wp361bLifetimeBehavior final : public Behavior {
  public:
    using Params = Wp361bLifetimeParams;
    void onInit(BehaviorContext &) override {
        ++wp361b_lifetime_init_count;
    }
    void onDestroy(BehaviorContext &context) noexcept override {
        wp361b_lifetime_destroy_alive.push_back(
            wp361b_lifetime_ecs != nullptr &&
            wp361b_lifetime_ecs->isAlive(context.self()));
    }
};

class Wp361Behavior final : public Behavior {
  public:
    using Params = Wp361Params;
    void onInit(BehaviorContext &context) override {
        const auto &params = context.params<Params>();
        wp361_callback_trace.push_back(std::to_string(params.hp) + ":" +
                                       params.target);
    }
    void onDestroy(BehaviorContext &) noexcept override {
        ++wp361_destroy_count;
    }
    void onUpdate(BehaviorContext &context) override {
        wp361_update_trace.push_back(context.params<Params>().hp);
    }
};

struct Wp361EscapedParams {
    std::string dotted = "dot";
    std::string slash = "slash";
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Wp361EscapedParams::dotted>("aim.target"), "dot"),
        defaulted(field<&Wp361EscapedParams::slash>("a/b"), "slash"));
};

class Wp361EscapedBehavior final : public Behavior {
  public:
    using Params = Wp361EscapedParams;
};

void ensureBehavior() {
    static const bool registered = [] {
        auto &registry = internal::getBehaviorRegisterer();
        if (registry.findByName("wp361_behavior") == nullptr) {
            (void)registry.registerBehavior<Wp361Behavior>(
                "wp361_behavior", 1, {});
        }
        return true;
    }();
    (void)registered;
}

void ensureEscapedBehavior() {
    ensureBehavior();
    static const bool registered = [] {
        auto &registry = internal::getBehaviorRegisterer();
        if (registry.findByName("wp361_escaped") == nullptr) {
            (void)registry.registerBehavior<Wp361EscapedBehavior>(
                "wp361_escaped", 1, {});
        }
        return true;
    }();
    (void)registered;
}

void ensureWp361bLifetimeBehavior() {
    ensureBehavior();
    static const bool registered = [] {
        auto &registry = internal::getBehaviorRegisterer();
        if (registry.findByName("wp361b_lifetime") == nullptr) {
            (void)registry.registerBehavior<Wp361bLifetimeBehavior>(
                "wp361b_lifetime", 1, {});
        }
        return true;
    }();
    (void)registered;
}

Json tagged(std::string name) { return Json{{"$param", std::move(name)}}; }

void ensureWp361Logger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

class Wp361TemporaryProject final {
    std::filesystem::path root_;

  public:
    Wp361TemporaryProject() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        for (std::uint64_t nonce = 0;; ++nonce) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("pelican-wp361a-" + std::to_string(stamp) + "-" +
                 std::to_string(nonce));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root_ = std::move(candidate);
                break;
            }
            if (nonce == 1024) {
                throw std::runtime_error(
                    "could not allocate WP361a temporary project directory");
            }
        }
    }
    ~Wp361TemporaryProject() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    const std::filesystem::path &root() const noexcept { return root_; }
    void write(std::string_view relative, std::string_view bytes) const {
        std::ofstream output{root_ / relative,
                             std::ios_base::binary | std::ios_base::trunc};
        if (!output.is_open()) {
            throw std::runtime_error("could not write WP361a fixture");
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("could not flush WP361a fixture");
        }
    }
    std::string read(std::string_view relative) const {
        std::ifstream input{root_ / relative, std::ios_base::binary};
        if (!input.is_open()) {
            throw std::runtime_error("could not read WP362 fixture");
        }
        std::ostringstream bytes;
        bytes << input.rdbuf();
        if (!input.eof() && input.fail()) {
            throw std::runtime_error("could not read WP362 fixture bytes");
        }
        return bytes.str();
    }
};

Json prefabFixture(std::string_view shape = "capsule") {
    return Json{
        {"schema", "pelican.prefab"},
        {"version", 1},
        {"name", "unit"},
        {"parameters",
         Json::array({
             {{"name", "hp"}, {"type", "i32"}, {"range", {1, 100}}, {"default", 30}},
             {{"name", "target"}, {"kind", "object"},
              {"required_components", Json::array({"transform"})}},
             {{"name", "tint"}, {"type", "vec4"}, {"default", {1.0, 1.0, 1.0, 1.0}}},
             {{"name", "texture"}, {"kind", "asset"}, {"asset_kind", "texture"},
              {"default", "white"}},
             {{"name", "radius"}, {"type", "f32"}, {"range", {0.1, 10.0}},
              {"default", 0.4}},
         })},
        {"components",
         Json::array({
             {{"name", "sprite_view"}, {"id", "sprite"},
              {"texture", tagged("texture")}, {"color", tagged("tint")},
              {"size", {1.0, 1.0}}, {"pivot", {0.5, 0.5}}},
             {{"name", "collider"}, {"id", "hit"}, {"shape", shape},
              {"radius", tagged("radius")}},
             {{"name", "behavior"}, {"id", "logic_hp"},
              {"type", "wp361_behavior"}, {"params", {{"hp", tagged("hp")}}}},
             {{"name", "behavior"}, {"id", "logic_target"},
              {"type", "wp361_behavior"},
              {"params", {{"target", tagged("target")}}}},
         })},
    };
}

PrefabRegistrySnapshot registryFor(const Json &prefab) {
    const auto bytes = prefab.dump();
    const std::vector<PrefabRegistryEntry> entries{{"unit", "prefabs/unit.json"}};
    return buildPrefabRegistrySnapshot(
        entries, [bytes](std::string_view path) {
            REQUIRE(std::string{path} == "prefabs/unit.json");
            return bytes;
        }, 3);
}

Json transform() {
    return {{"name", "transform"}, {"pos", {0.0, 0.0, 0.0}},
            {"rotation", {0.0, 0.0, 0.0, 1.0}}, {"scale", {1.0, 1.0, 1.0}}};
}

Json prefabObject(std::string name, std::string id, std::string target,
                  std::optional<int> hp = std::nullopt) {
    Json parameters{{"target", std::move(target)}};
    if (hp) parameters["hp"] = *hp;
    return {{"name", std::move(name)},
            {"components", Json::array({
                 transform(),
                 Json{{"name", "behavior"}, {"type", "wp361_behavior"},
                      {"params", Json::object()}},
             })},
            {"prefab", {{"ref", "unit"}, {"instance_id", std::move(id)},
                        {"parameters", std::move(parameters)}}}};
}

Json sceneFixture(bool reverse = false, bool override_same_default = false) {
    Json target{{"name", "Target"}, {"components", Json::array({transform()})}};
    auto instance = prefabObject("Instance", "gi_main", "Target",
                                 override_same_default ? std::optional{30}
                                                       : std::nullopt);
    Json objects = reverse ? Json::array({instance, target})
                           : Json::array({target, instance});
    return {{"schema", "pelican.scene"}, {"version", 2},
            {"scenes", {{"main", {{"objects", std::move(objects)}}}}}};
}

Json enumPrefabFixture() {
    auto prefab = prefabFixture();
    prefab["parameters"].push_back(
        {{"name", "mode"},
         {"type", "enum"},
         {"enum_values", Json::array({"none", "y_axis", "full"})},
         {"default", "none"}});
    prefab["components"][0]["billboard"] = tagged("mode");
    prefab["components"][2]["params"]["mode"] = tagged("mode");
    return prefab;
}

Json enumSceneFixture() {
    auto scene = sceneFixture();
    scene["scenes"]["main"]["objects"][1]["prefab"]["parameters"]
         ["mode"] = "full";
    return scene;
}

struct PreparedFixture {
    PrefabRegistrySnapshot registry;
    BindableProviderSnapshot provider;
    AuthoringSceneDocument document;
    ResolvedScene resolved;
};

class PrefabProjectionTarget final : public EditorProjectionDocumentTarget {
    SceneProjectionState state_;

  public:
    PrefabProjectionTarget(AuthoringSceneDocument document,
                           PrefabRegistrySnapshot registry,
                           BindableProviderSnapshot provider)
        : state_{ResolvedSceneResolver::prepare(
              std::move(document), SceneResolverGeneration{20}, {},
              std::move(registry), std::move(provider))} {}

    const SceneProjectionState &projectionState() const override {
        return state_;
    }
    SceneRevision nextProjectionRevision() const override {
        return SceneRevision{state_.revision().value + 1U};
    }
    SceneResolverGeneration nextProjectionResolverGeneration() const override {
        return SceneResolverGeneration{
            state_.resolverGeneration().value + 1U};
    }
    void publishProjectionState(SceneProjectionState &candidate) noexcept override {
        state_.swap(candidate);
    }

    void refreshDependencies(PrefabRegistrySnapshot registry,
                             BindableProviderSnapshot provider) {
        auto candidate = ResolvedSceneResolver::prepare(
            state_.authoring(), nextProjectionResolverGeneration(),
            state_.resolved().defaults(), std::move(registry),
            std::move(provider));
        publishProjectionState(candidate);
    }
};

PreparedFixture prepareFixture(Json scene = sceneFixture()) {
    ensureBehavior();
    auto registry = registryFor(prefabFixture());
    auto provider = buildProductionBindableProviderSnapshot(7);
    auto document = AuthoringSceneDocument::load(scene.dump(), SceneRevision{11});
    auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{12}, {}, registry, provider);
    return {std::move(registry), std::move(provider), std::move(document),
            std::move(resolved)};
}

const ResolvedObject &instanceObject(const ResolvedScene &resolved) {
    const auto &objects = resolved.findScene("main")->objects;
    return *std::find_if(objects.begin(), objects.end(), [](const auto &object) {
        return object.name == "Instance";
    });
}

const ResolvedComponent &generatedByKey(const ResolvedObject &object,
                                        std::string_view key) {
    return *std::find_if(object.components.begin(), object.components.end(),
                         [&](const auto &component) {
                             const auto *origin = component.origin.generated();
                             return origin != nullptr && origin->component_key == key;
                         });
}

std::vector<BoundSceneBehaviorAttachment> bindPreparedToEcs(
    std::vector<PreparedSceneBehaviorAttachment> prepared) {
    std::vector<GameObjectId> entities;
    std::vector<BoundSceneBehaviorAttachment> result;
    for (auto &attachment : prepared) {
        if (entities.size() <= attachment.object_index) {
            entities.resize(attachment.object_index + 1, invalidGameObjectId);
        }
        auto &entity = entities[attachment.object_index];
        if (entity == invalidGameObjectId) {
            entity = GameObjects::createWithComponents(
                std::span<const ComponentId>{}, {});
        }
        result.push_back({.prepared = std::move(attachment), .entity = entity});
    }
    return result;
}

using ArenaIdentity =
    std::tuple<std::uint64_t, std::uint64_t, std::size_t, bool, std::string>;

std::vector<ArenaIdentity> arenaIdentity(
    std::span<const BehaviorAttachmentInfo> snapshot) {
    std::vector<ArenaIdentity> result;
    for (const auto &attachment : snapshot) {
        const auto *generated = attachment.component_origin.generated();
        result.emplace_back(attachment.handle.value, attachment.attachment_seq,
                            attachment.component_index, generated != nullptr,
                            generated == nullptr
                                ? std::string{}
                                : generated->stable_generated_id);
    }
    return result;
}

using BehaviorOriginIdentity = std::pair<bool, std::string>;

std::vector<BehaviorOriginIdentity> behaviorOriginIdentity(
    const ResolvedScene &resolved, std::string_view object_name = "Instance") {
    std::vector<BehaviorOriginIdentity> result;
    for (const auto &attachment : prepareResolvedSceneBehaviorAttachments(
             resolved.findScene("main")->objects,
             BehaviorRegistryAvailability::active)) {
        if (attachment.object_name != object_name) continue;
        const auto *generated = attachment.component_origin.generated();
        result.emplace_back(generated != nullptr,
                            generated == nullptr
                                ? std::string{}
                                : generated->stable_generated_id);
    }
    return result;
}

void requirePrefabError(PrefabErrorCode code, const auto &call) {
    try {
        call();
        FAIL("prefab operation unexpectedly succeeded");
    } catch (const PrefabError &error) {
        REQUIRE(error.code() == code);
        REQUIRE(std::string{prefabErrorCodeName(error.code())}
                    .starts_with("prefab_"));
    }
}

std::string independentGid(std::string_view instance, std::string_view key) {
    std::vector<unsigned char> bytes;
    const auto lp = [&](std::string_view text) {
        const auto size = static_cast<std::uint32_t>(text.size());
        bytes.push_back(static_cast<unsigned char>(size));
        bytes.push_back(static_cast<unsigned char>(size >> 8));
        bytes.push_back(static_cast<unsigned char>(size >> 16));
        bytes.push_back(static_cast<unsigned char>(size >> 24));
        bytes.insert(bytes.end(), text.begin(), text.end());
    };
    lp("pelican.prefab.gid.v1");
    lp(instance);
    lp(key);
    std::vector<unsigned char> digest(picosha2::k_digest_size);
    picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "gid_";
    for (std::size_t index = 0; index < 16; ++index) {
        result.push_back(hex[digest[index] >> 4]);
        result.push_back(hex[digest[index] & 15]);
    }
    return result;
}

void appendWp362Le32(std::vector<unsigned char> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void appendWp362Lp(std::vector<unsigned char> &bytes, std::string_view value) {
    appendWp362Le32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendWp362Float(std::vector<unsigned char> &bytes, std::string_view name,
                      float value) {
    appendWp362Lp(bytes, name);
    if (value == 0.0F) value = 0.0F;
    appendWp362Le32(bytes, std::bit_cast<std::uint32_t>(value));
}

template <class Enum>
void appendWp362Enum(std::vector<unsigned char> &bytes, std::string_view name,
                     Enum value) {
    appendWp362Lp(bytes, name);
    appendWp362Le32(bytes, static_cast<std::uint32_t>(value));
}

std::string independentResolverInputsFingerprint(
    const ResolvedSceneDefaults &defaults) {
    std::vector<unsigned char> bytes;
    appendWp362Lp(bytes, "pelican.resolver.inputs.v1");
    appendWp362Enum(bytes, "camera_projection.kind",
                    defaults.camera_projection.kind);
    appendWp362Float(bytes, "camera_projection.yfov",
                     defaults.camera_projection.yfov);
    appendWp362Float(bytes, "camera_projection.znear",
                     defaults.camera_projection.znear);
    appendWp362Float(bytes, "camera_projection.zfar",
                     defaults.camera_projection.zfar);
    appendWp362Lp(bytes, "camera_projection.aspect");
    bytes.push_back(defaults.camera_projection.aspect ? 0x01U : 0x00U);
    if (defaults.camera_projection.aspect) {
        auto value = *defaults.camera_projection.aspect;
        if (value == 0.0F) value = 0.0F;
        appendWp362Le32(bytes, std::bit_cast<std::uint32_t>(value));
    }
    appendWp362Float(bytes, "camera_projection.xmag",
                     defaults.camera_projection.xmag);
    appendWp362Float(bytes, "camera_projection.ymag",
                     defaults.camera_projection.ymag);
    appendWp362Enum(bytes, "camera_sprite.pixel_perfect",
                    defaults.camera_sprite.pixel_perfect);
    appendWp362Enum(bytes, "camera_sprite.sort",
                    defaults.camera_sprite.sort);
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

PrefabRegistrySnapshot registryWithUnused(const Json &used) {
    auto unused = used;
    unused["name"] = "unused";
    unused["parameters"] = Json::array();
    unused["components"] = Json::array(
        {{{"name", "sprite_view"}, {"texture", "white"}}});
    const auto used_bytes = used.dump();
    const auto unused_bytes = unused.dump();
    const std::vector<PrefabRegistryEntry> entries{
        {"unused", "prefabs/unused.json"},
        {"unit", "prefabs/unit.json"},
    };
    return buildPrefabRegistrySnapshot(
        entries, [used_bytes, unused_bytes](std::string_view path) {
            return path == "prefabs/unit.json" ? used_bytes : unused_bytes;
        }, 99);
}

} // namespace

TEST_CASE("WP361 v4 leaf grammar registry gate and GID are byte fixed",
          "[wp361][prefab][leaf]") {
    const std::array error_catalog{
        std::pair{PrefabErrorCode::PrefabNotFound, "prefab_not_found"},
        std::pair{PrefabErrorCode::PrefabVersionUnsupported, "prefab_version_unsupported"},
        std::pair{PrefabErrorCode::PrefabRequiresSceneV2, "prefab_requires_scene_v2"},
        std::pair{PrefabErrorCode::PrefabDuplicateName, "prefab_duplicate_name"},
        std::pair{PrefabErrorCode::PrefabParameterUnknown, "prefab_parameter_unknown"},
        std::pair{PrefabErrorCode::PrefabParameterDuplicate, "prefab_parameter_duplicate"},
        std::pair{PrefabErrorCode::PrefabParameterType, "prefab_parameter_type"},
        std::pair{PrefabErrorCode::PrefabParameterUnused, "prefab_parameter_unused"},
        std::pair{PrefabErrorCode::PrefabBindingNotBindable, "prefab_binding_not_bindable"},
        std::pair{PrefabErrorCode::PrefabBindingInactive, "prefab_binding_inactive"},
        std::pair{PrefabErrorCode::PrefabObjectRefUnresolved, "prefab_object_ref_unresolved"},
        std::pair{PrefabErrorCode::PrefabObjectRefMissingComponent,
                  "prefab_object_ref_missing_component"},
        std::pair{PrefabErrorCode::PrefabTransformForbidden, "prefab_transform_forbidden"},
        std::pair{PrefabErrorCode::PrefabDependencyMismatch, "prefab_dependency_mismatch"},
        std::pair{PrefabErrorCode::PrefabNestedUnsupported, "prefab_nested_unsupported"},
        std::pair{PrefabErrorCode::PrefabProviderStale, "prefab_provider_stale"},
        std::pair{PrefabErrorCode::PrefabParameterRequired, "prefab_parameter_required"},
        std::pair{PrefabErrorCode::PrefabInstanceIdCollision, "prefab_instance_id_collision"},
        std::pair{PrefabErrorCode::PrefabGeneratedReadOnly, "prefab_generated_read_only"},
        std::pair{PrefabErrorCode::PrefabGeneratedIdCollision, "prefab_generated_id_collision"},
        std::pair{PrefabErrorCode::PrefabInstanceTransformMissing,
                  "prefab_instance_transform_missing"},
        std::pair{PrefabErrorCode::PrefabInstanceTransformDuplicate,
                  "prefab_instance_transform_duplicate"},
        std::pair{PrefabErrorCode::PrefabComponentDuplicate, "prefab_component_duplicate"},
        std::pair{PrefabErrorCode::PrefabNameMismatch, "prefab_name_mismatch"},
        std::pair{PrefabErrorCode::PrefabPersistenceUnsupported,
                  "prefab_persistence_unsupported"},
        std::pair{PrefabErrorCode::PrefabPathInvalid, "prefab_path_invalid"},
        std::pair{PrefabErrorCode::PrefabRegistryInvalid, "prefab_registry_invalid"},
        std::pair{PrefabErrorCode::PrefabDocumentInvalid, "prefab_document_invalid"},
        std::pair{PrefabErrorCode::PrefabInstanceInvalid, "prefab_instance_invalid"},
        std::pair{PrefabErrorCode::PrefabInstanceIdInvalid, "prefab_instance_id_invalid"},
        std::pair{PrefabErrorCode::PrefabComponentKeyDuplicate,
                  "prefab_component_key_duplicate"},
    };
    for (const auto &[code, expected_name] : error_catalog) {
        REQUIRE(std::string{prefabErrorCodeName(code)} == expected_name);
    }

    const auto project = parseProjectEnvelopeJson(
        {{"schema", "pelican.project"}, {"version", 1},
         {"prefabs", Json::array({{{"name", "unit"},
                                    {"path", "prefabs/unit.json"}}})}});
    REQUIRE(project.envelope.prefabs.size() == 1);
    REQUIRE(registryFor(prefabFixture()).records().front().name == "unit");

    const auto expected = independentGid("gi_main", "sprite");
    REQUIRE(prefabGeneratedId("gi_main", "sprite") == expected);
    REQUIRE(expected.size() == 36);
    REQUIRE(prefabGeneratedId("gi_other", "sprite") != expected);
    REQUIRE(prefabGeneratedId("gi_main", "hit") != expected);

    auto v1 = sceneFixture();
    v1["version"] = 1;
    v1["scenes"]["main"]["objects"][1]["prefab"] = nullptr;
    requirePrefabError(PrefabErrorCode::PrefabRequiresSceneV2,
                       [&] { (void)normalizeSceneDataJson(v1); });
    v1["scenes"]["main"]["objects"][1]["prefab"] = 42;
    requirePrefabError(PrefabErrorCode::PrefabRequiresSceneV2,
                       [&] { (void)normalizeSceneDataJson(v1); });

    requirePrefabError(PrefabErrorCode::PrefabPathInvalid, [&] {
        (void)parseProjectEnvelopeJson(
            {{"schema", "pelican.project"}, {"version", 1},
             {"prefabs", Json::array({{{"name", "unit"},
                                        {"path", "../unit.json"}}})}});
    });
    requirePrefabError(PrefabErrorCode::PrefabPathInvalid, [&] {
        (void)parseProjectEnvelopeJson(
            {{"schema", "pelican.project"}, {"version", 1},
             {"prefabs", Json::array({{{"name", "unit"},
                                        {"path", "project://prefabs/unit.json"}}})}});
    });
    for (const auto path : {"project:foo", "https:unit.json"}) {
        requirePrefabError(PrefabErrorCode::PrefabPathInvalid, [&] {
            (void)parseProjectEnvelopeJson(
                {{"schema", "pelican.project"}, {"version", 1},
                 {"prefabs", Json::array({{{"name", "unit"},
                                            {"path", path}}})}});
        });
    }
    requirePrefabError(PrefabErrorCode::PrefabRegistryInvalid, [&] {
        (void)parseProjectEnvelopeJson(
            {{"schema", "pelican.project"}, {"version", 1},
             {"prefabs", Json::array({{{"name", "unit"}, {"path", "u"},
                                        {"extra", true}}})}});
    });
}

TEST_CASE("WP361 enum prefab parameter reaches codec and behavior canonicalization",
          "[wp361][prefab][enum][resolver]") {
    ensureBehavior();
    auto registry = registryFor(enumPrefabFixture());
    const auto *record = registry.find("unit");
    REQUIRE(record != nullptr);
    const auto &declaration = record->document->parameters.back();
    REQUIRE(declaration.name == "mode");
    REQUIRE(declaration.value_schema.enum_values ==
            std::vector<std::string>{"none", "y_axis", "full"});

    auto provider = buildProductionBindableProviderSnapshot(7);
    auto document = AuthoringSceneDocument::load(enumSceneFixture().dump(),
                                                  SceneRevision{11});
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{12}, {}, registry, provider);
    const auto &object = instanceObject(resolved);
    const auto &parameter = object.prefab_instance->parameters.back();
    REQUIRE(parameter.name == "mode");
    REQUIRE(parameter.is_override);
    REQUIRE(parameter.value_authored == Json("full"));
    REQUIRE(parameter.value_resolved == OJson("full"));

    const auto &sprite = generatedByKey(object, "sprite");
    REQUIRE_FALSE(sprite.runtime_resolution_error);
    REQUIRE_NOTHROW(sprite.requireRuntimeValue());
    REQUIRE(sprite.effective_json.at("billboard") == "full");

    const auto &behavior = generatedByKey(object, "logic_hp");
    REQUIRE_FALSE(behavior.runtime_resolution_error);
    REQUIRE(behavior.behavior_canonical_params.has_value());
    REQUIRE(Json::parse(*behavior.behavior_canonical_params).at("mode") ==
            "full");
    const auto prepared = prepareResolvedSceneBehaviorAttachments(
        resolved.findScene("main")->objects,
        BehaviorRegistryAvailability::active);
    const auto prepared_behavior =
        std::find_if(prepared.begin(), prepared.end(), [](const auto &entry) {
            const auto *origin = entry.component_origin.generated();
            return origin != nullptr && origin->component_key == "logic_hp";
        });
    REQUIRE(prepared_behavior != prepared.end());
    REQUIRE(Json::parse(prepared_behavior->canonical_params).at("mode") ==
            "full");
}

TEST_CASE("WP361 enum parameter requires enum_values",
          "[wp361][prefab][enum][negative]") {
    auto prefab = enumPrefabFixture();
    prefab["parameters"].back().erase("enum_values");
    requirePrefabError(PrefabErrorCode::PrefabDocumentInvalid,
                       [&] { (void)registryFor(prefab); });
}

TEST_CASE("WP361 non-enum parameter forbids enum_values",
          "[wp361][prefab][enum][negative]") {
    auto prefab = prefabFixture();
    prefab["parameters"][0]["enum_values"] = Json::array({"thirty"});
    requirePrefabError(PrefabErrorCode::PrefabDocumentInvalid,
                       [&] { (void)registryFor(prefab); });
}

TEST_CASE("WP361 enum parameter default must be declared",
          "[wp361][prefab][enum][negative]") {
    auto prefab = enumPrefabFixture();
    prefab["parameters"].back()["default"] = "sideways";
    requirePrefabError(PrefabErrorCode::PrefabParameterType,
                       [&] { (void)registryFor(prefab); });

    prefab["parameters"].back()["default"] = 7;
    requirePrefabError(PrefabErrorCode::PrefabParameterType,
                       [&] { (void)registryFor(prefab); });
}

TEST_CASE("WP361 resolver expands authored then generated and preserves camera-era order",
          "[wp361][prefab][resolver][order]") {
    auto fixture = prepareFixture();
    const auto &object = instanceObject(fixture.resolved);
    REQUIRE(object.components.size() == 6);
    REQUIRE_FALSE(object.components[0].origin.isGenerated());
    REQUIRE_FALSE(object.components[1].origin.isGenerated());
    REQUIRE(object.components[2].origin.generated()->component_key == "sprite");
    REQUIRE(object.components[3].origin.generated()->component_key == "hit");
    REQUIRE(object.components[4].origin.generated()->component_key == "logic_hp");
    REQUIRE(object.components[5].origin.generated()->component_key == "logic_target");
    REQUIRE(generatedByKey(object, "sprite").effective_json.at("texture") == "white");
    REQUIRE(generatedByKey(object, "hit").effective_json.at("radius") == 0.4F);
    REQUIRE(object.prefab_instance->closure_generation == 12);
    REQUIRE(object.prefab_instance->provider_generation == 7);
    REQUIRE(object.prefab_instance->parameters[0].name == "hp");
    REQUIRE_FALSE(object.prefab_instance->parameters[0].is_override);

    const auto prepared = prepareResolvedSceneBehaviorAttachments(
        fixture.resolved.findScene("main")->objects,
        BehaviorRegistryAvailability::active);
    std::vector<std::size_t> trace;
    for (const auto &attachment : prepared) {
        if (attachment.object_name == "Instance") {
            trace.push_back(attachment.component_index);
        }
    }
    REQUIRE(trace == std::vector<std::size_t>{1, 4, 5});
    REQUIRE(trace != std::vector<std::size_t>{4, 1, 5});
    REQUIRE(sceneBehaviorAttachmentSeq(1, 4) ==
            (std::uint64_t{1} << 32U | 4U));
    REQUIRE(prepared.back().component_origin.generated() != nullptr);

    const auto *raw_sprite = fixture.registry.find("unit");
    REQUIRE_THROWS(findComponentCodec("sprite_view")
                       ->decodeAuthored(raw_sprite->document->components[0].body));
    REQUIRE_FALSE(generatedByKey(object, "sprite").runtime_resolution_error);

    auto changed_instance_scene = sceneFixture();
    changed_instance_scene["scenes"]["main"]["objects"][1]["prefab"]
                          ["instance_id"] = "gi_other";
    const auto changed_instance = prepareFixture(changed_instance_scene);
    REQUIRE(generatedByKey(instanceObject(changed_instance.resolved), "sprite")
                .effective_json == generatedByKey(object, "sprite").effective_json);
    REQUIRE(generatedByKey(instanceObject(changed_instance.resolved), "sprite")
                .origin.generated()->stable_generated_id !=
            generatedByKey(object, "sprite")
                .origin.generated()->stable_generated_id);

    auto changed_value_scene = sceneFixture();
    changed_value_scene["scenes"]["main"]["objects"][1]["prefab"]
                       ["parameters"]["texture"] = "blue";
    const auto changed_value = prepareFixture(changed_value_scene);
    REQUIRE(generatedByKey(instanceObject(changed_value.resolved), "sprite")
                .origin.generated()->stable_generated_id ==
            generatedByKey(object, "sprite")
                .origin.generated()->stable_generated_id);
    REQUIRE(generatedByKey(instanceObject(changed_value.resolved), "sprite")
                .effective_json.at("texture") == "blue");

    auto prepended_prefab = prefabFixture();
    prepended_prefab["components"].insert(
        prepended_prefab["components"].begin(),
        Json{{"name", "animation"}, {"id", "preinsert"}, {"clip", "idle"}});
    const auto prepended_registry = registryFor(prepended_prefab);
    const auto prepended_resolved = ResolvedSceneResolver::resolve(
        fixture.document, SceneResolverGeneration{18}, {}, prepended_registry,
        fixture.provider);
    const auto &prepended_object = instanceObject(prepended_resolved);
    REQUIRE(generatedByKey(prepended_object, "sprite")
                .origin.generated()->stable_generated_id ==
            generatedByKey(object, "sprite").origin.generated()->stable_generated_id);
    REQUIRE(generatedByKey(prepended_object, "logic_hp")
                .origin.generated()->stable_generated_id ==
            generatedByKey(object, "logic_hp").origin.generated()->stable_generated_id);
    REQUIRE(generatedByKey(prepended_object, "preinsert").merged_component_index == 2);
}

TEST_CASE("WP361a Generated camera is the actual first camera and seeds later defaults",
          "[wp361a][prefab][camera][order]") {
    const Json prefab{
        {"schema", "pelican.prefab"}, {"version", 1}, {"name", "unit"},
        {"components", Json::array({
             {{"name", "camera"}, {"id", "generated_camera"},
              {"type", "perspective"}, {"yfov", 0.61F},
              {"znear", 0.27F}, {"zfar", 321.0F}, {"aspect", 1.7F}}
         })}};
    const Json scene{
        {"schema", "pelican.scene"}, {"version", 2},
        {"scenes", {{"main", {{"objects", Json::array({
             {{"name", "PrefabCamera"},
              {"components", Json::array({transform()})},
              {"prefab", {{"ref", "unit"}, {"instance_id", "gi_camera"}}}},
             {{"name", "InheritedCamera"},
              {"components", Json::array({transform(),
                                           Json{{"name", "camera"}}})}}
         })}}}}}};
    const auto document = AuthoringSceneDocument::load(scene.dump(),
                                                        SceneRevision{1});
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{1}, {}, registryFor(prefab),
        buildProductionBindableProviderSnapshot(1));
    const auto &first_object = resolved.findScene("main")->objects[0];
    const auto &generated = generatedByKey(first_object, "generated_camera");
    REQUIRE(generated.origin.isGenerated());
    REQUIRE(generated.name == "camera");
    const auto &later = resolved.findScene("main")->objects[1].components[1];
    const auto &later_camera =
        std::any_cast<const CameraCodecData &>(later.requireRuntimeValue());
    REQUIRE(*later_camera.yfov == Catch::Approx(0.61F));
    REQUIRE(*later_camera.znear == Catch::Approx(0.27F));
    REQUIRE(*later_camera.zfar == Catch::Approx(321.0F));
    REQUIRE(later_camera.aspect == std::optional<float>{1.7F});
}

TEST_CASE("WP361 provider descriptor includes defaults applicability and behaviors",
          "[wp361][prefab][provider]") {
    ensureBehavior();
    const auto first = buildProductionBindableProviderSnapshot(1);
    const auto second = buildProductionBindableProviderSnapshot(2);
    REQUIRE(std::string{first.fingerprint()} ==
            std::string{second.fingerprint()});
    std::unordered_set<std::string> codecs;
    for (const auto &descriptor : first.descriptors()) {
        codecs.insert(descriptor.provider_name);
    }
    for (const auto *name : {"transform", "simplemodelview", "camera", "light",
                             "collider", "animation", "sprite_view"}) {
        REQUIRE(codecs.contains(name));
    }
    REQUIRE(codecs.contains("behavior:wp361_behavior"));
    const auto *radius = first.find("collider", "/radius");
    REQUIRE(radius != nullptr);
    REQUIRE(radius->canonical_default == OJson(0.5));
    REQUIRE(first.active(*radius, Json{{"name", "collider"},
                                      {"shape", "capsule"}}));
    REQUIRE_FALSE(first.active(*radius, Json{{"name", "collider"},
                                            {"shape", "box"}}));
    REQUIRE(first.find("collider", "/shape") == nullptr);
    const auto shape_discriminant = std::find_if(
        first.descriptors().begin(), first.descriptors().end(),
        [](const auto &descriptor) {
            return descriptor.provider_name == "collider" &&
                   descriptor.json_pointer == "/shape";
        });
    REQUIRE(shape_discriminant != first.descriptors().end());
    REQUIRE_FALSE(shape_discriminant->bindable);
    REQUIRE(shape_discriminant->canonical_default == OJson("sphere"));

    auto reordered = std::vector<BindableDescriptor>{first.descriptors().begin(),
                                                     first.descriptors().end()};
    std::reverse(reordered.begin(), reordered.end());
    REQUIRE(std::string{BindableProviderSnapshot{3, reordered}.fingerprint()} ==
            std::string{first.fingerprint()});
    auto applicability_mutated = reordered;
    const auto active_found = std::find_if(
        applicability_mutated.begin(), applicability_mutated.end(),
        [](const auto &descriptor) {
            return descriptor.provider_name == "collider" &&
                   descriptor.json_pointer == "/radius";
        });
    REQUIRE(active_found != applicability_mutated.end());
    active_found->applicability.front().active_values = {"sphere"};
    REQUIRE(std::string{BindableProviderSnapshot{
                            5, std::move(applicability_mutated)}
                            .fingerprint()} !=
            std::string{first.fingerprint()});

    auto discriminant_mutated = reordered;
    const auto discriminant_found = std::find_if(
        discriminant_mutated.begin(), discriminant_mutated.end(),
        [](const auto &descriptor) {
            return descriptor.provider_name == "collider" &&
                   descriptor.json_pointer == "/shape";
        });
    REQUIRE(discriminant_found != discriminant_mutated.end());
    discriminant_found->canonical_default = "box";
    REQUIRE(std::string{BindableProviderSnapshot{
                            6, std::move(discriminant_mutated)}
                            .fingerprint()} !=
            std::string{first.fingerprint()});
}

TEST_CASE("WP361a codec default change reaches provider and resolved value",
          "[wp361a][prefab][provider][default][wiring]") {
    const Json prefab{
        {"schema", "pelican.prefab"}, {"version", 1}, {"name", "unit"},
        {"components", Json::array({
             {{"name", "collider"}, {"id", "hit"}, {"shape", "sphere"}}
         })}};
    const Json scene{
        {"schema", "pelican.scene"}, {"version", 2},
        {"scenes", {{"main", {{"objects", Json::array({
             {{"name", "Instance"}, {"components", Json::array({transform()})},
              {"prefab", {{"ref", "unit"}, {"instance_id", "gi_default"}}}}
         })}}}}}};
    const auto document = AuthoringSceneDocument::load(scene.dump(),
                                                        SceneRevision{1});
    const auto resolve_radius = [&](std::uint64_t generation) {
        auto provider = buildProductionBindableProviderSnapshot(generation);
        const auto *descriptor = provider.find("collider", "/radius");
        REQUIRE(descriptor != nullptr);
        const auto resolved = ResolvedSceneResolver::resolve(
            document, SceneResolverGeneration{generation}, {},
            registryFor(prefab), provider);
        const auto value = generatedByKey(instanceObject(resolved), "hit")
                               .effective_json.at("radius").get<float>();
        return std::tuple{value, descriptor->canonical_default.value(),
                          std::string{provider.fingerprint()}};
    };

    const auto baseline = resolve_radius(10);
    REQUIRE(std::get<0>(baseline) == Catch::Approx(0.5F));
    {
        testing::ScopedComponentCodecCanonicalDefaults changed{
            ComponentCodecCanonicalDefaults{.collider_radius = 0.875F}};
        const auto mutated = resolve_radius(11);
        REQUIRE(std::get<0>(mutated) == Catch::Approx(0.875F));
        REQUIRE(std::get<1>(mutated) == OJson(0.875F));
        REQUIRE(std::get<2>(mutated) != std::get<2>(baseline));
    }
    REQUIRE(std::get<0>(resolve_radius(12)) == Catch::Approx(0.5F));
}

TEST_CASE("WP361a provider fingerprint v4 has an independent hand-byte oracle",
          "[wp361a][prefab][provider][fingerprint][oracle]") {
    // Hand-assembled wire bytes: LP(domain), descriptor count, LP("x"),
    // LP("/v"), F32/Value/empty asset kind, flags, typed 1.5F LE, then
    // empty range/enum/applicability.  No production encoder helper is used.
    const std::vector<unsigned char> bytes{
        0x1c,0x00,0x00,0x00,0x70,0x65,0x6c,0x69,0x63,0x61,0x6e,0x2e,
        0x62,0x69,0x6e,0x64,0x61,0x62,0x6c,0x65,0x2e,0x70,0x72,0x6f,
        0x76,0x69,0x64,0x65,0x72,0x2e,0x76,0x34,0x01,0x00,0x00,0x00,
        0x01,0x00,0x00,0x00,0x78,0x02,0x00,0x00,0x00,0x2f,0x76,
        0x08,                         // descriptor type: F32
        0x00,                         // semantic kind: Value
        0x00,0x00,0x00,0x00,         // LP(asset_kind): empty
        0x01,0x00,0x01,               // bindable, required, has-default
        0x08,0x00,0x00,0xc0,0x3f,    // typed F32 1.5, IEEE754 LE
        0x00,                         // no range
        0x00,0x00,0x00,0x00,         // no enum values
        0x00,0x00,0x00,0x00};        // no applicability clauses
    const std::string expected =
        "b5c7a65309cc8f8720ad65ad5d61f2526556ce3c120dc1ce31b5ac5610b923c7";
    REQUIRE(picosha2::hash256_hex_string(bytes.begin(), bytes.end()) ==
            expected);
    BindableDescriptor descriptor{
        .provider_name = "x", .json_pointer = "/v",
        .type = Schema::FieldType::F32,
        .semantic_kind = BindableSemanticKind::Value,
        .bindable = true, .required = false,
        .canonical_default = OJson(1.5F)};
    REQUIRE(std::string{BindableProviderSnapshot{1, {std::move(descriptor)}}
                            .fingerprint()} == expected);

    // Independently hand-assembled Vec3 and String branches. In particular,
    // String carries raw UTF-8 bytes (including the quote), not JSON dump
    // quoting/escaping. These fixed vectors therefore reject the old encoder.
    const std::vector<unsigned char> vec3_bytes{
        0x1c,0x00,0x00,0x00,0x70,0x65,0x6c,0x69,0x63,0x61,0x6e,0x2e,
        0x62,0x69,0x6e,0x64,0x61,0x62,0x6c,0x65,0x2e,0x70,0x72,0x6f,
        0x76,0x69,0x64,0x65,0x72,0x2e,0x76,0x34,0x01,0x00,0x00,0x00,
        0x01,0x00,0x00,0x00,0x78,0x02,0x00,0x00,0x00,0x2f,0x76,
        0x0b,                         // descriptor type: Vec3
        0x00,                         // semantic kind: Value
        0x00,0x00,0x00,0x00,         // LP(asset_kind): empty
        0x01,0x00,0x01,               // bindable, required, has-default
        0x0b,                         // typed value: Vec3
        0x00,0x00,0x80,0x3f,         // 1.0F
        0x00,0x00,0x00,0xc0,         // -2.0F
        0x00,0x00,0x00,0x3f,         // 0.5F
        0x00,                         // no range
        0x00,0x00,0x00,0x00,         // no enum values
        0x00,0x00,0x00,0x00};        // no applicability clauses
    const std::string vec3_expected =
        "af5e97973566f6bcb517d4afb40d1d6dd95c5265513c107267a9ed27449d2f93";
    REQUIRE(picosha2::hash256_hex_string(vec3_bytes.begin(),
                                        vec3_bytes.end()) == vec3_expected);
    BindableDescriptor vec3_descriptor{
        .provider_name = "x", .json_pointer = "/v",
        .type = Schema::FieldType::Vec3,
        .semantic_kind = BindableSemanticKind::Value,
        .bindable = true, .required = false,
        .canonical_default = OJson::array({1.0F, -2.0F, 0.5F})};
    REQUIRE(std::string{BindableProviderSnapshot{
                            2, {std::move(vec3_descriptor)}}
                            .fingerprint()} == vec3_expected);

    const std::vector<unsigned char> string_bytes{
        0x1c,0x00,0x00,0x00,0x70,0x65,0x6c,0x69,0x63,0x61,0x6e,0x2e,
        0x62,0x69,0x6e,0x64,0x61,0x62,0x6c,0x65,0x2e,0x70,0x72,0x6f,
        0x76,0x69,0x64,0x65,0x72,0x2e,0x76,0x34,0x01,0x00,0x00,0x00,
        0x01,0x00,0x00,0x00,0x78,0x02,0x00,0x00,0x00,0x2f,0x76,
        0x0e,                         // descriptor type: String
        0x00,                         // semantic kind: Value
        0x00,0x00,0x00,0x00,         // LP(asset_kind): empty
        0x01,0x00,0x01,               // bindable, required, has-default
        0x0e,                         // typed value: String
        0x03,0x00,0x00,0x00,         // LP byte count
        0x61,0x22,0x62,               // raw UTF-8: a\"b
        0x00,                         // no range
        0x00,0x00,0x00,0x00,         // no enum values
        0x00,0x00,0x00,0x00};        // no applicability clauses
    const std::string string_expected =
        "5c35bea99e3882a451bcc77b2735f4644f2eef656d21ac93e878fb65bd8465b4";
    REQUIRE(picosha2::hash256_hex_string(string_bytes.begin(),
                                        string_bytes.end()) ==
            string_expected);
    BindableDescriptor string_descriptor{
        .provider_name = "x", .json_pointer = "/v",
        .type = Schema::FieldType::String,
        .semantic_kind = BindableSemanticKind::Value,
        .bindable = true, .required = false,
        .canonical_default = OJson("a\"b")};
    REQUIRE(std::string{BindableProviderSnapshot{
                            3, {std::move(string_descriptor)}}
                            .fingerprint()} == string_expected);
}

TEST_CASE("WP361a light provider required/default rules match the decoder",
          "[wp361a][prefab][light][provider][decoder]") {
    const auto provider = buildProductionBindableProviderSnapshot(1);
    const auto &codec = requireComponentCodec("light");
    const std::array valid{
        Json{{"name", "light"}, {"type", "directional"},
             {"direction", {0.0, -1.0, 0.0}}, {"color", {1.0, 1.0, 1.0}}},
        Json{{"name", "light"}, {"type", "point"},
             {"position", {0.0, 0.0, 0.0}}, {"color", {1.0, 1.0, 1.0}}},
        Json{{"name", "light"}, {"type", "spot"},
             {"position", {0.0, 0.0, 0.0}},
             {"direction", {0.0, -1.0, 0.0}}, {"color", {1.0, 1.0, 1.0}}}};
    for (const auto &component : valid) {
        const auto canonical = codec.encodeCanonical(codec.decodeAuthored(component));
        REQUIRE(canonical.at("intensity") == Catch::Approx(1.0F));
        const auto *intensity = provider.findActive("light", "/intensity", component);
        REQUIRE(intensity != nullptr);
        REQUIRE_FALSE(intensity->required);
        REQUIRE(intensity->canonical_default == OJson(1.0F));
        for (const auto path : {std::string_view{"/position"},
                                std::string_view{"/direction"},
                                std::string_view{"/color"}}) {
            const auto *field = provider.findActive("light", path, component);
            if (field != nullptr) {
                REQUIRE(field->required);
                REQUIRE_FALSE(field->canonical_default.has_value());
            }
        }
    }
    REQUIRE_THROWS(codec.decodeAuthored(
        Json{{"name", "light"}, {"direction", {0.0, -1.0, 0.0}},
             {"color", {1.0, 1.0, 1.0}}}));
    REQUIRE_THROWS(codec.decodeAuthored(
        Json{{"name", "light"}, {"type", "point"},
             {"position", {0.0, 0.0, 0.0}}}));
    REQUIRE_THROWS(codec.decodeAuthored(
        Json{{"name", "light"}, {"type", "spot"},
             {"position", {0.0, 0.0, 0.0}}, {"color", {1.0, 1.0, 1.0}}}));
}

TEST_CASE("WP361a behavior binding field names use RFC6901 tokens",
          "[wp361a][prefab][behavior][rfc6901]") {
    ensureEscapedBehavior();
    const auto provider = buildProductionBindableProviderSnapshot(1);
    REQUIRE(provider.find("behavior:wp361_escaped", "/params/aim.target") !=
            nullptr);
    REQUIRE(provider.find("behavior:wp361_escaped", "/params/a~1b") != nullptr);
    REQUIRE(provider.find("behavior:wp361_escaped", "/params/aim/target") ==
            nullptr);

    const Json prefab{
        {"schema", "pelican.prefab"}, {"version", 1}, {"name", "unit"},
        {"parameters", Json::array({
             {{"name", "dot"}, {"type", "string"}, {"default", "D"}},
             {{"name", "slash"}, {"type", "string"}, {"default", "S"}}
         })},
        {"components", Json::array({
             {{"name", "behavior"}, {"id", "escaped"},
              {"type", "wp361_escaped"},
              {"params", {{"aim.target", tagged("dot")},
                           {"a/b", tagged("slash")}}}}
         })}};
    const Json scene{
        {"schema", "pelican.scene"}, {"version", 2},
        {"scenes", {{"main", {{"objects", Json::array({
             {{"name", "Instance"}, {"components", Json::array({transform()})},
              {"prefab", {{"ref", "unit"}, {"instance_id", "gi_escape"}}}}
         })}}}}}};
    const auto document = AuthoringSceneDocument::load(scene.dump(),
                                                        SceneRevision{1});
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{1}, {}, registryFor(prefab), provider);
    const auto params = generatedByKey(instanceObject(resolved), "escaped")
                            .effective_json.at("params");
    REQUIRE(params.at("aim.target") == "D");
    REQUIRE(params.at("a/b") == "S");
    REQUIRE_FALSE(params.contains("aim"));
}

TEST_CASE("WP361 validation negatives are named before codec canonicalization",
          "[wp361][prefab][negative]") {
    ensureBehavior();
    const auto provider = buildProductionBindableProviderSnapshot(1);
    const auto resolve = [&](Json prefab, Json scene = sceneFixture()) {
        auto registry = registryFor(prefab);
        auto document = AuthoringSceneDocument::load(scene.dump(), SceneRevision{1});
        return ResolvedSceneResolver::resolve(document, SceneResolverGeneration{1},
                                              {}, registry, provider);
    };

    auto unused = prefabFixture();
    unused["parameters"].push_back({{"name", "unused"}, {"type", "bool"},
                                    {"default", false}});
    requirePrefabError(PrefabErrorCode::PrefabParameterUnused,
                       [&] { (void)registryFor(unused); });

    auto illegal_range = prefabFixture();
    illegal_range["parameters"][3]["range"] = Json::array({0, 1});
    requirePrefabError(PrefabErrorCode::PrefabDocumentInvalid,
                       [&] { (void)registryFor(illegal_range); });

    auto missing = sceneFixture();
    missing["scenes"]["main"]["objects"][1]["prefab"]["parameters"].erase("target");
    requirePrefabError(PrefabErrorCode::PrefabParameterRequired,
                       [&] { (void)resolve(prefabFixture(), missing); });

    auto wrong_type = sceneFixture();
    wrong_type["scenes"]["main"]["objects"][1]["prefab"]["parameters"]["hp"] = "thirty";
    requirePrefabError(PrefabErrorCode::PrefabParameterType,
                       [&] { (void)resolve(prefabFixture(), wrong_type); });
    auto wrong_asset_subtype = prefabFixture();
    wrong_asset_subtype["parameters"][3]["asset_kind"] = "model";
    requirePrefabError(PrefabErrorCode::PrefabParameterType,
                       [&] { (void)resolve(wrong_asset_subtype); });
    auto out_of_range = sceneFixture();
    out_of_range["scenes"]["main"]["objects"][1]["prefab"]["parameters"]["hp"] = 101;
    requirePrefabError(PrefabErrorCode::PrefabParameterType,
                       [&] { (void)resolve(prefabFixture(), out_of_range); });

    requirePrefabError(PrefabErrorCode::PrefabBindingInactive,
                       [&] { (void)resolve(prefabFixture("box")); });

    auto discriminant = prefabFixture();
    discriminant["parameters"].push_back(
        {{"name", "shape_value"}, {"type", "string"}, {"default", "sphere"}});
    discriminant["components"][1]["shape"] = tagged("shape_value");
    requirePrefabError(PrefabErrorCode::PrefabBindingNotBindable,
                       [&] { (void)resolve(discriminant); });

    auto unresolved = sceneFixture();
    unresolved["scenes"]["main"]["objects"][1]["prefab"]["parameters"]["target"] = "Absent";
    requirePrefabError(PrefabErrorCode::PrefabObjectRefUnresolved,
                       [&] { (void)resolve(prefabFixture(), unresolved); });
    auto required = prefabFixture();
    required["parameters"][1]["required_components"] = Json::array({"light"});
    requirePrefabError(PrefabErrorCode::PrefabObjectRefMissingComponent,
                       [&] { (void)resolve(required); });

    auto transformed = prefabFixture();
    transformed["components"].push_back(transform());
    requirePrefabError(PrefabErrorCode::PrefabTransformForbidden,
                       [&] { (void)registryFor(transformed); });

    auto missing_transform = sceneFixture();
    missing_transform["scenes"]["main"]["objects"][1]["components"].erase(0);
    requirePrefabError(PrefabErrorCode::PrefabInstanceTransformMissing,
                       [&] { (void)resolve(prefabFixture(), missing_transform); });
    auto duplicate_transform = sceneFixture();
    duplicate_transform["scenes"]["main"]["objects"][1]["components"]
        .push_back(transform());
    requirePrefabError(PrefabErrorCode::PrefabInstanceTransformDuplicate,
                       [&] { (void)resolve(prefabFixture(), duplicate_transform); });
    auto duplicate_component = sceneFixture();
    duplicate_component["scenes"]["main"]["objects"][1]["components"]
        .push_back({{"name", "sprite_view"}, {"texture", "white"}});
    requirePrefabError(PrefabErrorCode::PrefabComponentDuplicate,
                       [&] { (void)resolve(prefabFixture(), duplicate_component); });

    auto duplicate_behavior = prefabFixture();
    duplicate_behavior["components"][2].erase("id");
    duplicate_behavior["components"][3].erase("id");
    requirePrefabError(PrefabErrorCode::PrefabComponentKeyDuplicate,
                       [&] { (void)registryFor(duplicate_behavior); });

    auto nested = prefabFixture();
    nested["components"][0]["prefab"] = Json::object();
    requirePrefabError(PrefabErrorCode::PrefabNestedUnsupported,
                       [&] { (void)registryFor(nested); });

    requirePrefabError(PrefabErrorCode::PrefabProviderStale, [&] {
        auto registry = registryFor(prefabFixture());
        auto document = AuthoringSceneDocument::load(sceneFixture().dump(),
                                                      SceneRevision{1});
        (void)ResolvedSceneResolver::resolve(
            document, SceneResolverGeneration{1}, {}, registry,
            BindableProviderSnapshot{});
    });
}

TEST_CASE("WP361 object references use frozen identity for forward backward self and generated supply",
          "[wp361][prefab][object-ref][four-phase]") {
    ensureBehavior();
    auto registry = registryFor(prefabFixture());
    auto provider = buildProductionBindableProviderSnapshot(1);
    const auto run = [&](const Json &scene) {
        auto document = AuthoringSceneDocument::load(scene.dump(), SceneRevision{1});
        return ResolvedSceneResolver::resolve(document, SceneResolverGeneration{1},
                                              {}, registry, provider);
    };
    const auto forward = run(sceneFixture(false));
    const auto backward = run(sceneFixture(true));
    REQUIRE(generatedByKey(instanceObject(forward), "logic_target").effective_json ==
            generatedByKey(instanceObject(backward), "logic_target").effective_json);

    auto self = sceneFixture();
    self["scenes"]["main"]["objects"][1]["prefab"]["parameters"]["target"] = "Instance";
    REQUIRE_NOTHROW(run(self));

    auto generated_required = prefabFixture();
    generated_required["parameters"][1]["required_components"] =
        Json::array({"sprite_view"});
    auto generated_forward_scene = sceneFixture();
    generated_forward_scene["scenes"]["main"]["objects"][0] =
        prefabObject("Target", "gi_target", "Target");
    const auto run_generated = [&](const Json &scene) {
        auto local_registry = registryFor(generated_required);
        auto document = AuthoringSceneDocument::load(scene.dump(), SceneRevision{2});
        return ResolvedSceneResolver::resolve(document, SceneResolverGeneration{2},
                                              {}, local_registry, provider);
    };
    const auto generated_forward = run_generated(generated_forward_scene);
    auto generated_backward_scene = generated_forward_scene;
    std::reverse(generated_backward_scene["scenes"]["main"]["objects"].begin(),
                 generated_backward_scene["scenes"]["main"]["objects"].end());
    const auto generated_backward = run_generated(generated_backward_scene);
    REQUIRE(generatedByKey(instanceObject(generated_forward), "logic_target")
                .effective_json ==
            generatedByKey(instanceObject(generated_backward), "logic_target")
                .effective_json);
}

TEST_CASE("WP361b production factory lifecycle reconciles Generated behaviors and rebases sequence",
          "[wp361b][prefab][lifecycle][arena][production][factory][rebase]") {
    ensureBehavior();
    ensureWp361Logger();
    Wp361TemporaryProject temporary;
    const Json prefab{
        {"schema", "pelican.prefab"}, {"version", 1}, {"name", "unit"},
        {"parameters", Json::array({
             {{"name", "hp"}, {"type", "i32"}, {"default", 9}}
         })},
        {"components", Json::array({
             {{"name", "behavior"}, {"id", "generated_logic"},
              {"type", "wp361_behavior"},
              {"params", {{"hp", tagged("hp")}}}}
         })}};
    const Json scene{
        {"schema", "pelican.scene"}, {"version", 2},
        {"scenes", {{"main", {{"objects", Json::array()}}}}}};
    const Json project{
        {"schema", "pelican.project"}, {"version", 1},
        {"name", "wp361a-lifecycle"}, {"engine_min_version", "0.1.0"},
        {"prefabs", Json::array({
             {{"name", "unit"}, {"path", "unit.prefab.json"}}
         })},
        {"basic_config", {{"default_scene_id", "main"},
                          {"scene_data_json", "scene.json"},
                          {"asset_data_json", "assets.json"},
                          {"rendering_config_json", "render.json"},
                          {"default_rendering_pass", "main_render"}}}};
    temporary.write("unit.prefab.json", prefab.dump());
    temporary.write("scene.json", scene.dump());
    temporary.write(
        "assets.json",
        Json{{"schema", "pelican.asset_data"}, {"version", 1},
             {"models", Json::array()}}
            .dump());
    temporary.write(
        "render.json",
        Json{{"pipeline",
              {{"preset",
                "engine://render_pipelines/hybrid_v1.json"}}}}
            .dump());

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temporary.root(), false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &config = GET_MODULE(ProjectBasicConfig);
    (void)config.sceneProjectionState();
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    auto &scene_loader = GET_MODULE(SceneLoader);
    scene_loader.load("main");
    auto editor_service = makeEditorRuntimeService();
    const auto session = editor_service->openEditorSession(
        {{"display_name", "wp361b lifecycle factory"}});
    const auto actor = session.at("actor_id").get<std::uint64_t>();
    const auto commit = [&](Json operation) {
        const auto accepted = editor_service->edit(
            {{"actor_id", actor},
             {"base_revision", config.sceneDocument().revision().value},
             {"operations", Json::array({std::move(operation)})}});
        REQUIRE(accepted.at("status") == "accepted");
        editor_service->commitPendingEdits();
        const auto result =
            editor_service->getEditResult({{"ticket", accepted.at("ticket")}});
        INFO("WP361a lifecycle result: " << result.dump());
        REQUIRE(result.at("status") == "committed");
        return result;
    };
    const auto spawn_object = [](std::string name, int hp) {
        return Json{{"name", std::move(name)},
                    {"components", Json::array({transform()})},
                    {"prefab", {{"ref", "unit"},
                                {"instance_id", "replaced"},
                                {"parameters", {{"hp", hp}}}}}};
    };

    wp361_callback_trace.clear();
    wp361_update_trace.clear();
    wp361_destroy_count = 0;
    const auto first_spawn = commit(
        {{"op", "spawn"}, {"scene_id", "main"},
         {"declaration_index", 0}, {"object", spawn_object("First", 9)},
         {"new_instance_id", "gi_first"}});
    GameContext game;
    arena.update(game);
    arena.update(game);
    REQUIRE(wp361_callback_trace == std::vector<std::string>{"9:none"});
    REQUIRE(arena.snapshot().size() == 1);
    REQUIRE(arena.snapshot().front().component_origin.isGenerated());
    const auto first_id = config.resolvedScene()
                              .findScene("main")->objects.front()
                              .authoring_object_id;

    (void)commit({{"op", "spawn"}, {"scene_id", "main"},
                  {"declaration_index", 0},
                  {"object", spawn_object("Head", 2)},
                  {"new_instance_id", "gi_head"}});
    arena.update(game);
    arena.update(game);
    REQUIRE(wp361_callback_trace ==
            std::vector<std::string>{"9:none", "2:none"});
    auto attachments = arena.snapshot();
    REQUIRE(attachments.size() == 2);
    REQUIRE(attachments[0].scene_object_index == 0);
    REQUIRE(attachments[1].scene_object_index == 1);
    REQUIRE(attachments[1].attachment_seq == sceneBehaviorAttachmentSeq(1, 1));

    // This is the real server wired to the production service. Both `.edit`
    // and `.runtime_query` come from editorruntimefactory; deleting either
    // factory branch makes the positive/query assertions below fail.
    EditorCommandRpcAdapter rpc_adapter{*editor_service};
    std::istringstream rpc_input;
    std::ostringstream rpc_output;
    RpcServer rpc_server{rpc_input, rpc_output};
    configureEditorRpcHandlers(
        rpc_server, rpc_adapter,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    std::uint64_t rpc_id = 1;
    const auto rpc_response = [&](std::string method, Json params) {
        return Json::parse(rpc_server.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", rpc_id++},
                 {"method", std::move(method)}, {"params", std::move(params)}}
                .dump()));
    };
    const auto rpc = [&](std::string method, Json params) {
        const auto response =
            rpc_response(std::move(method), std::move(params));
        INFO("WP361a RPC response: " << response.dump());
        REQUIRE_FALSE(response.contains("error"));
        return response.at("result");
    };
    const auto rpc_actor = actor;
    const auto collision = rpc_response(
        "edit",
        {{"actor_id", rpc_actor},
         {"base_revision", config.sceneDocument().revision().value},
         {"operations",
          Json::array({{{"op", "spawn"},
                        {"scene_id", "main"},
                        {"declaration_index", 2},
                        {"object", spawn_object("Collision", 7)},
                        {"new_instance_id", "gi_first"}}})}});
    INFO("WP361a collision RPC response: " << collision.dump());
    REQUIRE(collision.at("result").at("status") == "rejected");
    REQUIRE(collision.at("result").at("error").at("code") ==
            "prefab_instance_id_collision");
    const auto add_accepted = rpc(
        "edit",
        {{"actor_id", rpc_actor},
         {"base_revision", config.sceneDocument().revision().value},
         {"operations",
          Json::array({{{"op", "add_component"},
                        {"object_id", first_id.value},
                        {"component_index", 1},
                        {"component", {{"name", "behavior"},
                                       {"type", "wp361_behavior"},
                                       {"params", {{"hp", 4}}}}}}})}});
    REQUIRE(add_accepted.at("status") == "accepted");
    editor_service->commitPendingEdits();
    const auto add_result = rpc(
        "get_edit_result", {{"ticket", add_accepted.at("ticket")}});
    REQUIRE(add_result.at("status") == "committed");
    const auto authored_after_rpc =
        AuthoringSceneAuthority::rawView(config.sceneDocument()).documentJson();
    const auto &authored_first =
        authored_after_rpc.at("scenes").at("main").at("objects").at(1);
    REQUIRE(authored_first.at("components").size() == 2);
    REQUIRE(authored_first.at("components").at(1).at("params").at("hp") == 4);
    arena.update(game);
    arena.update(game);
    REQUIRE(wp361_callback_trace ==
            std::vector<std::string>{"9:none", "2:none", "4:none"});
    attachments = arena.snapshot();
    REQUIRE(attachments.size() == 3);
    std::vector<std::uint64_t> runtime_sequence;
    for (const auto &attachment : attachments) {
        runtime_sequence.push_back(attachment.attachment_seq);
    }
    std::vector<std::uint64_t> fresh_sequence;
    std::vector<int> fresh_hp;
    for (const auto &attachment : prepareResolvedSceneBehaviorAttachments(
             config.resolvedScene().findScene("main")->objects,
             BehaviorRegistryAvailability::active)) {
        fresh_sequence.push_back(sceneBehaviorAttachmentSeq(
            attachment.object_index, attachment.component_index));
        fresh_hp.push_back(
            Json::parse(attachment.canonical_params).at("hp").get<int>());
    }
    REQUIRE(runtime_sequence == fresh_sequence);
    REQUIRE(fresh_hp == std::vector<int>{2, 4, 9});
    const auto journal_rpc = rpc("query_journal", Json::object());
    const auto journal_seq =
        journal_rpc.at("records").back().at("ordered_forward").front().at(
            "attachment_seq");
    const auto components_rpc =
        rpc("get_components", {{"authoring_object_id", first_id.value}});
    REQUIRE(components_rpc.at("entity_id").is_object());
    const auto behavior_rpc = std::find_if(
        components_rpc.at("components").begin(),
        components_rpc.at("components").end(), [](const auto &component) {
            return component.at("name") == "behavior" &&
                   component.at("component_index") == 1;
        });
    REQUIRE(behavior_rpc != components_rpc.at("components").end());
    const auto rpc_seq = behavior_rpc->at("behavior").at("attachment_seq");
    REQUIRE(journal_seq == sceneBehaviorAttachmentSeq(1, 1));
    REQUIRE(rpc_seq == journal_seq);
    REQUIRE(attachments[1].attachment_seq == journal_seq);
    wp361_update_trace.clear();
    arena.update(game);
    REQUIRE(wp361_update_trace == fresh_hp);

    const auto before_destroy = wp361_destroy_count;
    const auto destroyed =
        commit({{"op", "destroy"}, {"object_id", first_id.value}});
    REQUIRE(arena.snapshot().size() == 1);
    REQUIRE(wp361_destroy_count == before_destroy + 2);
    const auto undo = editor_service->undo(
        {{"actor_id", actor},
         {"base_revision", config.sceneDocument().revision().value},
         {"transaction_id", destroyed.at("transaction_id")}});
    editor_service->commitPendingEdits();
    const auto undone =
        editor_service->getEditResult({{"ticket", undo.at("ticket")}});
    REQUIRE(undone.at("status") == "committed");
    arena.update(game);
    arena.update(game);
    REQUIRE(arena.snapshot().size() == 3);
    REQUIRE(wp361_callback_trace[wp361_callback_trace.size() - 2] == "4:none");
    REQUIRE(wp361_callback_trace.back() == "9:none");

    const auto redo = editor_service->redo(
        {{"actor_id", actor},
         {"base_revision", config.sceneDocument().revision().value},
         {"transaction_id", undone.at("transaction_id")}});
    editor_service->commitPendingEdits();
    REQUIRE(editor_service->getEditResult({{"ticket", redo.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(arena.snapshot().size() == 1);
    editor_service.reset();
    arena.deactivateAll();
    GameObjects::removeAll();
    REQUIRE(arena.snapshot().empty());
    REQUIRE(ecs.liveCount() == 0);
    (void)first_spawn;
}

TEST_CASE("WP361b factory aggregate preserves scene isolation identity and lifetime",
          "[wp361b][prefab][lifecycle][factory][aggregate][rpc]") {
    ensureWp361bLifetimeBehavior();
    ensureWp361Logger();
    Wp361TemporaryProject temporary;
    const Json prefab{
        {"schema", "pelican.prefab"}, {"version", 1}, {"name", "unit"},
        {"parameters", Json::array({
             {{"name", "hp"}, {"type", "i32"}, {"default", 9}}
         })},
        {"components", Json::array({
             {{"name", "behavior"}, {"id", "generated_logic"},
              {"type", "wp361_behavior"},
              {"params", {{"hp", tagged("hp")}}}}
         })}};
    const Json probe{
        {"name", "Probe"},
        {"components", Json::array({
             transform(),
             Json{{"name", "camera"}, {"type", "perspective"},
                  {"yfov", 0.7F}, {"znear", 0.2F}, {"zfar", 200.0F}},
             Json{{"name", "light"}, {"type", "directional"},
                  {"direction", {0.0F, -1.0F, 0.0F}},
                  {"color", {1.0F, 0.8F, 0.6F}}, {"intensity", 2.0F}},
             Json{{"name", "collider"}, {"shape", "sphere"},
                  {"radius", 0.75F}},
             Json{{"name", "behavior"}, {"type", "wp361b_lifetime"},
                  {"params", {{"hp", 1}}}}
         })}};
    const Json scene{
        {"schema", "pelican.scene"}, {"version", 2},
        {"scenes", {{"main", {{"objects", Json::array({probe})}}},
                    {"secondary", {{"objects", Json::array()}}}}}};
    const Json project{
        {"schema", "pelican.project"}, {"version", 1},
        {"name", "wp361b-factory"}, {"engine_min_version", "0.1.0"},
        {"prefabs", Json::array({
             {{"name", "unit"}, {"path", "unit.prefab.json"}}
         })},
        {"basic_config", {{"default_scene_id", "main"},
                          {"scene_data_json", "scene.json"},
                          {"asset_data_json", "assets.json"},
                          {"rendering_config_json", "render.json"},
                          {"default_rendering_pass", "main_render"}}}};
    temporary.write("unit.prefab.json", prefab.dump());
    temporary.write("scene.json", scene.dump());
    temporary.write(
        "assets.json",
        Json{{"schema", "pelican.asset_data"}, {"version", 1},
             {"models", Json::array()}}
            .dump());
    temporary.write(
        "render.json",
        Json{{"pipeline",
              {{"preset",
                "engine://render_pipelines/hybrid_v1.json"}}}}
            .dump());

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temporary.root(), false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    GET_MODULE(EngineLaunchConfig).headless = true;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &config = GET_MODULE(ProjectBasicConfig);
    (void)config.sceneProjectionState();
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    auto &scene_loader = GET_MODULE(SceneLoader);
    wp361b_lifetime_ecs = &ecs;
    struct LifetimeObserverReset {
        ~LifetimeObserverReset() { wp361b_lifetime_ecs = nullptr; }
    } lifetime_observer_reset;
    wp361b_lifetime_init_count = 0;
    wp361b_lifetime_destroy_alive.clear();
    wp361_callback_trace.clear();
    wp361_destroy_count = 0;
    scene_loader.load("main");
    REQUIRE(scene_loader.currentScene() == "main");

    auto service = makeEditorRuntimeService();
    EditorCommandRpcAdapter rpc_adapter{*service};
    std::istringstream rpc_input;
    std::ostringstream rpc_output;
    RpcServer rpc_server{rpc_input, rpc_output};
    configureEditorRpcHandlers(
        rpc_server, rpc_adapter,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    std::uint64_t rpc_id = 1;
    const auto rpc_response = [&](std::string method, Json params) {
        return Json::parse(rpc_server.processLine(
            Json{{"jsonrpc", "2.0"}, {"id", rpc_id++},
                 {"method", std::move(method)}, {"params", std::move(params)}}
                .dump()));
    };
    const auto rpc = [&](std::string method, Json params) {
        const auto response =
            rpc_response(std::move(method), std::move(params));
        INFO("WP361b RPC response: " << response.dump());
        REQUIRE_FALSE(response.contains("error"));
        return response.at("result");
    };
    const auto session =
        rpc("open_editor_session", {{"display_name", "wp361b factory"}});
    const auto actor = session.at("actor_id").get<std::uint64_t>();
    const auto commit = [&](Json operations) {
        const auto accepted = rpc(
            "edit", {{"actor_id", actor},
                     {"base_revision", config.sceneDocument().revision().value},
                     {"operations", std::move(operations)}});
        REQUIRE(accepted.at("status") == "accepted");
        service->commitPendingEdits();
        const auto result =
            rpc("get_edit_result", {{"ticket", accepted.at("ticket")}});
        INFO("WP361b edit result: " << result.dump());
        REQUIRE(result.at("status") == "committed");
        return result;
    };
    const auto runtime_snapshot = [] {
        const auto full = internal::editorRuntimeStateSnapshotForTesting();
        return OJson{
            {"camera", full.at("camera")},
            {"light", full.at("light")},
            {"phys", full.at("phys")},
            {"behavior_attachments",
             full.at("behavior").at("attachments")},
        };
    };
    const auto find_object = [&](std::string_view scene_id,
                                 std::string_view name)
        -> const ResolvedObject & {
        const auto *resolved_scene = config.resolvedScene().findScene(scene_id);
        REQUIRE(resolved_scene != nullptr);
        const auto found = std::find_if(
            resolved_scene->objects.begin(), resolved_scene->objects.end(),
            [&](const auto &object) { return object.name == name; });
        REQUIRE(found != resolved_scene->objects.end());
        return *found;
    };
    const auto plain_object = [](std::string name) {
        return Json{{"name", std::move(name)},
                    {"components", Json::array({transform()})}};
    };
    const auto prefab_object = [](std::string name) {
        return Json{{"name", std::move(name)},
                    {"components", Json::array({transform()})},
                    {"prefab", {{"ref", "unit"},
                                {"instance_id", "replaced"}}}};
    };

    // A non-current lifecycle edit must remain authoring-only. The oracle is
    // the production isolation snapshot and enumerates all four affected
    // runtime domains rather than comparing only resolved component JSON.
    const auto before_secondary_spawn = runtime_snapshot();
    (void)commit(Json::array({
        {{"op", "spawn"}, {"scene_id", "secondary"},
         {"declaration_index", 0},
         {"object",
          Json{{"name", "SecondaryOnly"},
               {"components", Json::array({
                    transform(),
                    Json{{"name", "camera"}, {"type", "orthographic"},
                         {"xmag", 4.0F}, {"ymag", 3.0F}},
                    Json{{"name", "light"}, {"type", "directional"},
                         {"direction", {1.0F, 0.0F, 0.0F}},
                         {"color", {0.2F, 0.3F, 1.0F}}},
                    Json{{"name", "collider"}, {"shape", "sphere"},
                         {"radius", 3.0F}}
                })},
               {"prefab", {{"ref", "unit"},
                           {"instance_id", "replaced"}}}}},
         {"new_instance_id", "gi_secondary"}}
    }));
    REQUIRE(runtime_snapshot() == before_secondary_spawn);
    const auto secondary_query =
        rpc("get_components", {{"scene_id", "secondary"},
                               {"name", "SecondaryOnly"}});
    REQUIRE_FALSE(secondary_query.contains("entity_id"));
    const auto secondary_id =
        find_object("secondary", "SecondaryOnly").authoring_object_id;

    const auto before_secondary_destroy = runtime_snapshot();
    (void)commit(Json::array({
        {{"op", "destroy"}, {"object_id", secondary_id.value}}
    }));
    REQUIRE(runtime_snapshot() == before_secondary_destroy);
    REQUIRE(config.resolvedScene().findScene("secondary")->objects.empty());

    // One request may author lifecycle changes in several scenes; only main
    // receives an aggregate runtime participant.
    (void)commit(Json::array({
        {{"op", "spawn"}, {"scene_id", "main"},
         {"declaration_index", 1},
         {"object", plain_object("MainBatch")}},
        {{"op", "spawn"}, {"scene_id", "secondary"},
         {"declaration_index", 0},
         {"object", plain_object("SecondaryBatch")}}
    }));
    REQUIRE(find_object("main", "MainBatch").name == "MainBatch");
    REQUIRE(find_object("secondary", "SecondaryBatch").name ==
            "SecondaryBatch");
    REQUIRE(rpc("get_components", {{"scene_id", "main"},
                                   {"name", "MainBatch"}})
                .contains("entity_id"));
    REQUIRE_FALSE(rpc("get_components", {{"scene_id", "secondary"},
                                         {"name", "SecondaryBatch"}})
                      .contains("entity_id"));
    GameContext game;
    arena.update(game);
    arena.update(game);
    REQUIRE(wp361b_lifetime_init_count == 1);

    const auto probe_id = find_object("main", "Probe").authoring_object_id;
    // Leading spawn rebases Probe to object index 1. The behavior add in the
    // same operations array must consume one candidate-derived identity plan.
    (void)commit(Json::array({
        {{"op", "spawn"}, {"scene_id", "main"},
         {"declaration_index", 0},
         {"object", prefab_object("IdentityHead")},
         {"new_instance_id", "gi_identity"}},
        {{"op", "add_component"}, {"object_id", probe_id.value},
         {"component_index", 5},
         {"component", {{"name", "behavior"},
                        {"type", "wp361_behavior"},
                        {"params", {{"hp", 4}}}}}}
    }));
    arena.update(game);
    arena.update(game);
    const auto journal = rpc("query_journal", Json::object());
    const auto &forward = journal.at("records").back().at("ordered_forward");
    const auto journal_add = std::find_if(
        forward.begin(), forward.end(), [](const auto &operation) {
            return operation.at("op") == "add_component";
        });
    REQUIRE(journal_add != forward.end());
    const auto journal_handle =
        journal_add->at("attachment_handle").get<std::uint64_t>();
    const auto journal_seq =
        journal_add->at("attachment_seq").get<std::uint64_t>();
    REQUIRE(journal_seq == sceneBehaviorAttachmentSeq(1, 5));
    const auto probe_entity = scene_loader.objectId("Probe");
    REQUIRE(probe_entity.has_value());
    const auto arena_after_identity = arena.snapshot();
    const auto arena_identity = std::find_if(
        arena_after_identity.begin(), arena_after_identity.end(),
        [&](const auto &attachment) {
            return attachment.entity == *probe_entity &&
                   attachment.component_index == 5;
        });
    REQUIRE(arena_identity != arena_after_identity.end());
    REQUIRE(arena_identity->handle.value == journal_handle);
    REQUIRE(arena_identity->attachment_seq == journal_seq);
    const auto probe_rpc =
        rpc("get_components", {{"authoring_object_id", probe_id.value}});
    const auto rpc_behavior = std::find_if(
        probe_rpc.at("components").begin(), probe_rpc.at("components").end(),
        [](const auto &component) {
            return component.contains("name") &&
                   component.at("name") == "behavior" &&
                   component.at("component_index") == 5;
        });
    REQUIRE(rpc_behavior != probe_rpc.at("components").end());
    REQUIRE(rpc_behavior->at("behavior").at("attachment_handle") ==
            journal_handle);
    REQUIRE(rpc_behavior->at("behavior").at("attachment_seq") ==
            journal_seq);

    // The same real server has an authored positive above and the Generated
    // negative below, both with production .edit and .runtime_query wiring.
    const auto &identity_head = find_object("main", "IdentityHead");
    const auto generated_gid =
        generatedByKey(identity_head, "generated_logic")
            .origin.generated()->stable_generated_id;
    const auto generated_rejected = rpc_response(
        "edit", {{"actor_id", actor},
                 {"base_revision", config.sceneDocument().revision().value},
                 {"operations", Json::array({
                      {{"op", "set_component_value"},
                       {"object_id", identity_head.authoring_object_id.value},
                       {"generated_id", generated_gid},
                       {"field_path", "/params/hp"}, {"value", 8}}
                  })}});
    REQUIRE(generated_rejected.at("error").at("data").at("code") ==
            "prefab_generated_read_only");

    // All operation syntax is checked before selector resolution. Without the
    // syntax leaf this valid gid would win and incorrectly report read_only.
    const auto malformed_before_selector = rpc_response(
        "edit", {{"actor_id", actor},
                 {"base_revision", config.sceneDocument().revision().value},
                 {"operations", Json::array({
                      {{"op", "spawn"}, {"scene_id", "main"},
                       {"object", 42}},
                      {{"op", "set_component_value"},
                       {"object_id", identity_head.authoring_object_id.value},
                       {"generated_id", generated_gid},
                       {"field_path", "/params/hp"}, {"value", 8}}
                  })}});
    REQUIRE(malformed_before_selector.at("error").at("data").at("code") ==
            "invalid_params");

    const auto init_trace_before_params = wp361_callback_trace;
    const auto destroy_before_params = wp361_destroy_count;
    const auto lifetime_init_before_params = wp361b_lifetime_init_count;
    const auto lifetime_destroy_before_params =
        wp361b_lifetime_destroy_alive;
    (void)commit(Json::array({
        {{"op", "set_component_value"}, {"object_id", probe_id.value},
         {"component_slot", "behavior"}, {"attachment_index", 5},
         {"field_path", "/params/hp"}, {"value", 8}},
        {{"op", "spawn"}, {"scene_id", "main"},
         {"object", plain_object("Unrelated")}}
    }));
    arena.update(game);
    REQUIRE(wp361_callback_trace == init_trace_before_params);
    REQUIRE(wp361_destroy_count == destroy_before_params);
    REQUIRE(wp361b_lifetime_init_count == lifetime_init_before_params);
    REQUIRE(wp361b_lifetime_destroy_alive == lifetime_destroy_before_params);
    const auto arena_after_params = arena.snapshot();
    const auto params_attachment = std::find_if(
        arena_after_params.begin(), arena_after_params.end(),
        [&](const auto &attachment) {
            return attachment.entity == *probe_entity &&
                   attachment.component_index == 5;
        });
    REQUIRE(params_attachment != arena_after_params.end());
    REQUIRE(params_attachment->handle.value == journal_handle);
    REQUIRE(Json::parse(params_attachment->canonical_params).at("hp") == 8);

    (void)commit(Json::array({
        {{"op", "spawn"}, {"scene_id", "main"},
         {"object", plain_object("BatchParent")}},
        {{"op", "spawn"}, {"scene_id", "main"},
         {"object", Json{{"name", "BatchChild"},
                         {"parent", "BatchParent"},
                         {"components", Json::array({transform()})}}}}
    }));
    const auto parent_runtime =
        rpc("get_components", {{"scene_id", "main"},
                               {"name", "BatchParent"}});
    const auto child_runtime =
        rpc("get_components", {{"scene_id", "main"},
                               {"name", "BatchChild"}});
    REQUIRE(parent_runtime.contains("entity_id"));
    REQUIRE(child_runtime.contains("entity_id"));
    const auto parent_entity = GameObjectId{
        parent_runtime.at("entity_id").at("index").get<std::uint32_t>(),
        parent_runtime.at("entity_id").at("gen").get<std::uint32_t>()};
    const auto child_entity = GameObjectId{
        child_runtime.at("entity_id").at("index").get<std::uint32_t>(),
        child_runtime.at("entity_id").at("gen").get<std::uint32_t>()};
    REQUIRE(ecs.component<LocalTransformComponent>(child_entity).parent ==
            parent_entity);

    const auto destroyed_probe_entity = *probe_entity;
    wp361b_lifetime_destroy_alive.clear();
    (void)commit(Json::array({
        {{"op", "destroy"}, {"object_id", probe_id.value}}
    }));
    REQUIRE(wp361b_lifetime_destroy_alive == std::vector<bool>{true});
    REQUIRE_FALSE(ecs.isAlive(destroyed_probe_entity));

    service.reset();
    arena.deactivateAll();
    GameObjects::removeAll();
    REQUIRE(arena.snapshot().empty());
    REQUIRE(ecs.liveCount() == 0);
}

TEST_CASE("WP361 generated identity survives edit undo redo removal and authored insertion",
          "[wp361][prefab][identity]") {
    auto fixture = prepareFixture();
    const auto gid = generatedByKey(instanceObject(fixture.resolved), "sprite")
                         .origin.generated()->stable_generated_id;
    const auto base_behavior_identity = behaviorOriginIdentity(fixture.resolved);
    REQUIRE(base_behavior_identity.size() == 3);
    REQUIRE_FALSE(base_behavior_identity[0].first);
    REQUIRE(base_behavior_identity[1].first);
    REQUIRE(base_behavior_identity[2].first);

    auto edited_raw = AuthoringSceneAuthority::rawView(fixture.document).documentJson();
    edited_raw["scenes"]["main"]["objects"][1]["components"][0]["pos"] = {2.0, 0.0, 0.0};
    auto edited = AuthoringSceneAuthority::stage(fixture.document, edited_raw,
                                                 SceneRevision{12});
    auto edited_resolved = ResolvedSceneResolver::resolve(
        edited, SceneResolverGeneration{13}, {}, fixture.registry, fixture.provider);
    auto undone = AuthoringSceneAuthority::stage(
        edited, AuthoringSceneAuthority::rawView(fixture.document).documentJson(),
        SceneRevision{13});
    auto undone_resolved = ResolvedSceneResolver::resolve(
        undone, SceneResolverGeneration{14}, {}, fixture.registry, fixture.provider);
    auto redone = AuthoringSceneAuthority::stage(undone, edited_raw,
                                                 SceneRevision{14});
    auto redone_resolved = ResolvedSceneResolver::resolve(
        redone, SceneResolverGeneration{15}, {}, fixture.registry, fixture.provider);
    REQUIRE(generatedByKey(instanceObject(edited_resolved), "sprite")
                .origin.generated()->stable_generated_id == gid);
    REQUIRE(generatedByKey(instanceObject(undone_resolved), "sprite")
                .origin.generated()->stable_generated_id == gid);
    REQUIRE(generatedByKey(instanceObject(redone_resolved), "sprite")
                .origin.generated()->stable_generated_id == gid);
    REQUIRE(behaviorOriginIdentity(edited_resolved) == base_behavior_identity);
    REQUIRE(behaviorOriginIdentity(undone_resolved) == base_behavior_identity);
    REQUIRE(behaviorOriginIdentity(redone_resolved) == base_behavior_identity);

    auto inserted_raw = AuthoringSceneAuthority::rawView(fixture.document).documentJson();
    inserted_raw["scenes"]["main"]["objects"][1]["components"].insert(
        inserted_raw["scenes"]["main"]["objects"][1]["components"].begin() + 1,
        Json{{"name", "light"}, {"type", "directional"},
             {"direction", {0.0, -1.0, 0.0}}, {"intensity", 1.0},
             {"color", {1.0, 1.0, 1.0}}});
    auto inserted = AuthoringSceneAuthority::stage(fixture.document, inserted_raw,
                                                   SceneRevision{12});
    auto inserted_resolved = ResolvedSceneResolver::resolve(
        inserted, SceneResolverGeneration{16}, {}, fixture.registry, fixture.provider);
    const auto &inserted_sprite = generatedByKey(instanceObject(inserted_resolved), "sprite");
    REQUIRE(inserted_sprite.origin.generated()->stable_generated_id == gid);
    REQUIRE(inserted_sprite.merged_component_index == 3);
    REQUIRE(behaviorOriginIdentity(inserted_resolved) == base_behavior_identity);
    std::vector<std::size_t> inserted_behavior_indices;
    for (const auto &attachment : prepareResolvedSceneBehaviorAttachments(
             inserted_resolved.findScene("main")->objects,
             BehaviorRegistryAvailability::active)) {
        if (attachment.object_name == "Instance") {
            inserted_behavior_indices.push_back(attachment.component_index);
        }
    }
    REQUIRE(inserted_behavior_indices == std::vector<std::size_t>{2, 5, 6});

    auto structural = AuthoringSceneAuthority::structuralStage(fixture.document);
    const auto closure = structural.removeObject(
        instanceObject(fixture.resolved).authoring_object_id);
    structural.restoreObject(closure);
    auto restored = std::move(structural).finish(SceneRevision{12});
    auto restored_resolved = ResolvedSceneResolver::resolve(
        restored, SceneResolverGeneration{17}, {}, fixture.registry, fixture.provider);
    REQUIRE(generatedByKey(instanceObject(restored_resolved), "sprite")
                .origin.generated()->stable_generated_id == gid);
    REQUIRE(behaviorOriginIdentity(restored_resolved) == base_behavior_identity);

    auto second_session = AuthoringSceneDocument::load(
        AuthoringSceneAuthority::encodeSemantic(fixture.document),
        SceneRevision{99}, 900);
    auto second_resolved = ResolvedSceneResolver::resolve(
        second_session, SceneResolverGeneration{99}, {}, fixture.registry,
        fixture.provider);
    REQUIRE(generatedByKey(instanceObject(second_resolved), "sprite").effective_json ==
            generatedByKey(instanceObject(fixture.resolved), "sprite").effective_json);
    REQUIRE(generatedByKey(instanceObject(second_resolved), "sprite")
                .origin.generated()->stable_generated_id == gid);
    REQUIRE(instanceObject(second_resolved).authoring_object_id !=
            instanceObject(fixture.resolved).authoring_object_id);
    REQUIRE(behaviorOriginIdentity(second_resolved) == base_behavior_identity);
}

TEST_CASE("WP361 journal preserves generated identity and records prefab spawn authority",
          "[wp361][prefab][journal][identity][undo-redo]") {
    auto fixture = prepareFixture();
    REQUIRE(fixture.registry.find("unit") != nullptr);
    PrefabProjectionTarget target{fixture.document, fixture.registry,
                                  fixture.provider};
    REQUIRE(target.projectionResolvedScene().prefabRegistry().find("unit") !=
            nullptr);
    EditorEditCoordinator edits{EditorEditRuntimeDependencies{
        .document = [&]() -> const AuthoringSceneDocument & {
            return target.projectionDocument();
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .execute = [&](const EditorEditExecutionRequest &request) {
            EditorProjectionTransaction transaction{target,
                                                    request.base_revision};
            std::vector<EditorProjectionAdapter *> adapters;
            return transaction.commit(request.commands, adapters);
        },
        .gate = [] { return EditorGateObservation{}; },
        .prefab_registry = [&] {
            return target.projectionResolvedScene().prefabRegistry();
        },
        .bindable_provider = [&] {
            return target.projectionResolvedScene().bindableProvider();
        },
    }};
    const auto session = edits.openSession({{"display_name", "wp361 journal"}});
    const auto actor = session.at("actor_id").get<std::uint64_t>();
    const auto commit = [&](Json operation) {
        const auto accepted = edits.enqueue(
            {{"actor_id", actor},
             {"base_revision", target.projectionDocument().revision().value},
             {"operations", Json::array({std::move(operation)})}});
        REQUIRE(accepted.at("status") == "accepted");
        edits.commitPending();
        const auto result = edits.getResult({{"ticket", accepted.at("ticket")}});
        REQUIRE(result.at("status") == "committed");
        return result;
    };
    const auto gid = generatedByKey(
        instanceObject(target.projectionResolvedScene()), "sprite")
                         .origin.generated()->stable_generated_id;

    const auto set = commit(
        {{"op", "set_component_value"},
         {"object_id",
          instanceObject(target.projectionResolvedScene())
              .authoring_object_id.value},
         {"component_slot", "transform"}, {"field_path", "/pos"},
         {"value", {2.0, 0.0, 0.0}}});
    REQUIRE(generatedByKey(instanceObject(target.projectionResolvedScene()),
                           "sprite")
                .origin.generated()->stable_generated_id == gid);
    const auto undo_set = edits.enqueueUndo(
        {{"actor_id", actor},
         {"base_revision", target.projectionDocument().revision().value},
         {"transaction_id", set.at("transaction_id")}});
    edits.commitPending();
    const auto undone_set =
        edits.getResult({{"ticket", undo_set.at("ticket")}});
    REQUIRE(undone_set.at("status") == "committed");
    REQUIRE(generatedByKey(instanceObject(target.projectionResolvedScene()),
                           "sprite")
                .origin.generated()->stable_generated_id == gid);
    const auto redo_set = edits.enqueueRedo(
        {{"actor_id", actor},
         {"base_revision", target.projectionDocument().revision().value},
         {"transaction_id", undone_set.at("transaction_id")}});
    edits.commitPending();
    REQUIRE(edits.getResult({{"ticket", redo_set.at("ticket")}})
                .at("status") == "committed");
    REQUIRE(generatedByKey(instanceObject(target.projectionResolvedScene()),
                           "sprite")
                .origin.generated()->stable_generated_id == gid);

    auto clone = AuthoringSceneAuthority::rawView(target.projectionDocument())
                     .documentJson()
                     .at("scenes")
                     .at("main")
                     .at("objects")
                     .at(1);
    clone["name"] = "Clone";
    const Json spawn_without_authority{
        {"op", "spawn"}, {"scene_id", "main"},
        {"declaration_index", 2}, {"object", clone}};
    const auto rejected = edits.enqueue(
        {{"actor_id", actor},
         {"base_revision", target.projectionDocument().revision().value},
         {"operations", Json::array({spawn_without_authority})}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("error").at("code") ==
            "prefab_instance_id_collision");

    auto spawn = spawn_without_authority;
    spawn["new_instance_id"] = "gi_clone";
    const auto spawned = commit(std::move(spawn));
    REQUIRE(edits.journal().back().ordered_forward.front().at(
                "new_instance_id") == "gi_clone");
    const auto &clone_object = *std::find_if(
        target.projectionResolvedScene().findScene("main")->objects.begin(),
        target.projectionResolvedScene().findScene("main")->objects.end(),
        [](const auto &object) { return object.name == "Clone"; });
    REQUIRE(clone_object.prefab_instance->instance_id == "gi_clone");
    const auto clone_gid = generatedByKey(clone_object, "sprite")
                               .origin.generated()->stable_generated_id;

    const auto undo_spawn = edits.enqueueUndo(
        {{"actor_id", actor},
         {"base_revision", target.projectionDocument().revision().value},
         {"transaction_id", spawned.at("transaction_id")}});
    edits.commitPending();
    const auto undone_spawn =
        edits.getResult({{"ticket", undo_spawn.at("ticket")}});
    REQUIRE(undone_spawn.at("status") == "committed");
    REQUIRE(std::none_of(
        target.projectionResolvedScene().findScene("main")->objects.begin(),
        target.projectionResolvedScene().findScene("main")->objects.end(),
        [](const auto &object) { return object.name == "Clone"; }));
    const auto redo_spawn = edits.enqueueRedo(
        {{"actor_id", actor},
         {"base_revision", target.projectionDocument().revision().value},
         {"transaction_id", undone_spawn.at("transaction_id")}});
    edits.commitPending();
    REQUIRE(edits.getResult({{"ticket", redo_spawn.at("ticket")}})
                .at("status") == "committed");
    const auto &restored_clone = *std::find_if(
        target.projectionResolvedScene().findScene("main")->objects.begin(),
        target.projectionResolvedScene().findScene("main")->objects.end(),
        [](const auto &object) { return object.name == "Clone"; });
    REQUIRE(restored_clone.prefab_instance->instance_id == "gi_clone");
    REQUIRE(generatedByKey(restored_clone, "sprite")
                .origin.generated()->stable_generated_id == clone_gid);
}

#if PELICAN_WITH_RPC
TEST_CASE("WP361 real RpcServer exposes generated wire and preserves V1 gates",
          "[wp361][prefab][rpc][readonly][persistence]") {
    auto rpc_scene = sceneFixture(false, true);
    rpc_scene["scenes"]["main"]["objects"].push_back(
        prefabObject("DefaultInstance", "gi_default", "Target"));
    auto fixture = prepareFixture(std::move(rpc_scene));
    FastModuleContainer modules;
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &arena = GET_MODULE(BehaviorAttachmentArena);
    auto bound = bindPreparedToEcs(prepareResolvedSceneBehaviorAttachments(
        fixture.resolved.findScene("main")->objects,
        BehaviorRegistryAvailability::active));
    arena.publishSceneAttachments(std::move(bound));
    PrefabProjectionTarget target{fixture.document, fixture.registry,
                                  fixture.provider};
    wp361_callback_trace.clear();
    arena.activatePublished();
    REQUIRE(wp361_callback_trace ==
            std::vector<std::string>{"0:none", "30:none", "0:Target",
                                     "0:none", "30:none", "0:Target"});
    REQUIRE(wp361_callback_trace !=
            std::vector<std::string>{"30:none", "0:none", "0:Target",
                                     "30:none", "0:none", "0:Target"});
    const auto before_arena_snapshot = arena.snapshot();
    const auto before_arena_identity = arenaIdentity(before_arena_snapshot);
    for (const auto &attachment : before_arena_snapshot) {
        REQUIRE(GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(
            attachment.entity));
    }
    const auto before_bytes =
        AuthoringSceneAuthority::encodeSemantic(target.projectionDocument());
    const auto before_revision = target.projectionDocument().revision();
    const auto &object = instanceObject(target.projectionResolvedScene());
    const auto gid = generatedByKey(object, "sprite")
                         .origin.generated()->stable_generated_id;
    const auto &target_object = *std::find_if(
        target.projectionResolvedScene().findScene("main")->objects.begin(),
        target.projectionResolvedScene().findScene("main")->objects.end(),
        [](const auto &candidate) { return candidate.name == "Target"; });

    EditorEditRuntimeDependencies edit_dependencies{
        .document = [&]() -> const AuthoringSceneDocument & {
            return target.projectionDocument();
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .execute = [&](const EditorEditExecutionRequest &request) {
            EditorProjectionTransaction transaction{target,
                                                    request.base_revision};
            return transaction.commit(request.commands, {});
        },
        .gate = [] { return EditorGateObservation{}; },
        .allocate_behavior_attachment =
            [&](std::uint64_t commit_seq, std::size_t command_index,
                std::size_t attachment_index) {
                const auto identity = arena.reserveRuntimeAttachmentIdentity(
                    commit_seq, command_index, attachment_index);
                return EditorBehaviorAttachmentIdentity{
                    .handle = identity.handle.value,
                    .attachment_seq = identity.attachment_seq};
            },
        .resolve_behavior_attachment =
            [&](AuthoringObjectId object_id, std::size_t attachment_index)
            -> std::optional<EditorBehaviorAttachmentIdentity> {
                const auto &objects =
                    target.projectionResolvedScene().findScene("main")->objects;
                const auto object_it = std::find_if(
                    objects.begin(), objects.end(), [&](const auto &candidate) {
                        return candidate.authoring_object_id == object_id;
                    });
                if (object_it == objects.end()) return std::nullopt;
                const auto snapshot = arena.snapshot();
                const auto found = std::find_if(
                    snapshot.begin(), snapshot.end(), [&](const auto &entry) {
                        return entry.scene_object_index ==
                                   object_it->authoring_object_index &&
                               entry.component_index == attachment_index;
                    });
                if (found == snapshot.end()) return std::nullopt;
                return EditorBehaviorAttachmentIdentity{
                    .handle = found->handle.value,
                    .attachment_seq = found->attachment_seq};
            },
        .prefab_registry = [&] {
            return target.projectionResolvedScene().prefabRegistry();
        },
        .bindable_provider = [&] {
            return target.projectionResolvedScene().bindableProvider();
        },
        .closure_generation = [&] {
            return target.projectionResolvedScene().prefabClosure().generation;
        },
    };
    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & {
            return target.projectionDocument();
        },
        .resolved_scene = [&]() -> const ResolvedScene & {
            return target.projectionResolvedScene();
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .runtime_query = [&](const ResolvedSceneView &,
                             const ResolvedObject &resolved_object) {
            EditorRuntimeObjectState state;
            state.component_runtime_json.resize(
                resolved_object.components.size());
            state.component_pending.resize(resolved_object.components.size(),
                                           false);
            state.behavior_attachments.resize(
                resolved_object.components.size());
            for (const auto &attachment : arena.snapshot()) {
                if (attachment.scene_object_index !=
                    resolved_object.authoring_object_index) {
                    continue;
                }
                state.entity_id = attachment.entity;
                const auto component = std::find_if(
                    resolved_object.components.begin(),
                    resolved_object.components.end(),
                    [&](const auto &candidate) {
                        return candidate.merged_component_index ==
                               attachment.component_index;
                    });
                if (component == resolved_object.components.end()) continue;
                const auto index = static_cast<std::size_t>(
                    component - resolved_object.components.begin());
                state.behavior_attachments[index] =
                    EditorRuntimeBehaviorAttachmentState{
                        .handle = attachment.handle.value,
                        .attachment_seq = attachment.attachment_seq,
                        .owner = attachment.owner,
                        .owner_generation =
                            internal::registrationOwnerGeneration(
                                attachment.owner),
                        .pending = attachment.pending,
                        .active = attachment.active,
                    };
            }
            return state;
        },
        .edit = std::move(edit_dependencies),
    }};
    const auto rpc_session = service.openEditorSession(
        {{"display_name", "wp361b real rpc fixture"}});
    REQUIRE(rpc_session.at("actor_id") == 1);
    const auto typed_result = service.getComponents(
        EditorGetComponentsRequest{.name = std::string{"Instance"}});
    std::vector<std::size_t> generated_behavior_indices;
    for (const auto &component : typed_result.components) {
        if (!component.generated || component.name != "behavior") continue;
        REQUIRE_FALSE(component.editable);
        REQUIRE(component.schema_state == EditorComponentSchemaState::Missing);
        generated_behavior_indices.push_back(component.component_index);
    }
    REQUIRE(generated_behavior_indices == std::vector<std::size_t>{4, 5});
    EditorCommandRpcAdapter editor{service};
    const auto import_bytes = before_bytes;
    const auto digest = picosha2::hash256_hex_string(import_bytes.begin(),
                                                     import_bytes.end());
    std::vector<Json> requests{
        {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "get_components"},
         {"params", {{"name", "Instance"}}}},
        {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"generated_id", gid}, {"field_path", "/color"},
                          {"value", {0.0, 0.0, 0.0, 1.0}}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "save_scene"},
         {"params", Json::object()}},
        {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "export_scene_snapshot"},
         {"params", {{"schema_version", 1}}}},
        {{"jsonrpc", "2.0"}, {"id", 5}, {"method", "export_scene_snapshot"},
         {"params", {{"schema_version", 2}}}},
        {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "import_scene_snapshot"},
         {"params", {{"schema_version", 1}, {"semantic_scene_bytes", import_bytes},
                     {"digest", {{"algorithm", "sha256"}, {"hex", digest}}},
                     {"current_scene_id", "main"}}}},
        {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "import_scene_snapshot"},
         {"params", {{"schema_version", 2}, {"semantic_scene_bytes", import_bytes},
                     {"digest", {{"algorithm", "sha256"}, {"hex", digest}}},
                     {"current_scene_id", "main"}}}},
        {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"component_slot", "sprite_view"},
                          {"generated_id", gid}, {"field_path", "/color"},
                          {"value", {0.0, 0.0, 0.0, 1.0}}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"generated_id", "gid_unknown"},
                          {"field_path", "/color"}, {"value", nullptr}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"generated_id", gid}, {"field_path", "/color"},
                          {"value", nullptr}, {"unknown", true}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "get_components"},
         {"params", {{"name", "DefaultInstance"}}}},
        {{"jsonrpc", "2.0"}, {"id", 12}, {"method", "set_prefab_parameter"},
         {"params", {{"object", 12}, {"parameter", "hp"}, {"value", 55}}}},
        {{"jsonrpc", "2.0"}, {"id", 13}, {"method", "unset_prefab_parameter"},
         {"params", {{"object", 12}, {"parameter", "hp"}}}},
        {{"jsonrpc", "2.0"}, {"id", 14}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({{{"op", 42}}})}}}},
        {{"jsonrpc", "2.0"}, {"id", 15}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"generated_id", gid}, {"field_path", "/bad~2"},
                          {"value", nullptr}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 16}, {"method", "open_preview"},
         {"params", {{"actor_id", 1},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"generated_id", gid}, {"field_path", "/color"},
                          {"value", {0.0, 0.0, 0.0, 1.0}}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 17}, {"method", "update_preview"},
         {"params", {{"actor_id", 1}, {"ticket", "preview:fixture"},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"generated_id", gid}, {"field_path", "/color"},
                          {"value", {0.0, 0.0, 0.0, 1.0}}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 18}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({
                         {{"op", "set_component_value"},
                          {"object_id", target_object.authoring_object_id.value},
                          {"component_slot", "transform"},
                          {"field_path", "/pos"},
                          {"value", {1.0F, 2.0F, 3.0F}}}
                     })}}}},
        {{"jsonrpc", "2.0"}, {"id", 19}, {"method", "edit"},
         {"params", {{"actor_id", 1}, {"base_revision", 11},
                     {"operations", Json::array({
                         {{"op", "spawn"}, {"scene_id", "main"},
                          {"object", 42}},
                         {{"op", "set_component_value"},
                          {"object_id", object.authoring_object_id.value},
                          {"generated_id", gid}, {"field_path", "/color"},
                          {"value", {0.0, 0.0, 0.0, 1.0}}}
                     })}}}},
    };
    std::ostringstream request_bytes;
    for (const auto &request : requests) request_bytes << request.dump() << '\n';
    std::istringstream input{request_bytes.str()};
    std::ostringstream output;
    RpcServer server{input, output};
    configureEditorRpcHandlers(
        server, editor,
        {.snapshot_imported = [] {}, .save_busy = [] { return false; }});
    server.run();

    std::istringstream responses{output.str()};
    std::vector<Json> parsed;
    for (std::string line; std::getline(responses, line);) {
        parsed.push_back(Json::parse(line));
    }
    REQUIRE(parsed.size() == requests.size());
    const auto &result = parsed[0].at("result");
    REQUIRE(result.at("prefab_instance").at("parameters")[0].at("source") ==
            "override");
    REQUIRE(result.at("prefab_instance").at("parameters")[0].at("value_resolved") ==
            30);
    REQUIRE(result.at("prefab_instance").at("parameters")[0].at("value_authored") == 30);
    const auto generated = std::find_if(
        result.at("components").begin(), result.at("components").end(),
        [](const auto &component) { return component.contains("generated"); });
    REQUIRE(generated != result.at("components").end());
    REQUIRE(generated->size() == 1);
    REQUIRE_FALSE(generated->at("generated").contains("component_index"));
    REQUIRE(generated->at("generated").at("editable") == false);
    REQUIRE(generated->at("generated").at("source").at("component_key") == "sprite");
    REQUIRE(result.contains("entity_id"));
    const auto authored_behavior = std::find_if(
        result.at("components").begin(), result.at("components").end(),
        [](const auto &component) {
            return component.contains("name") &&
                   component.at("name") == "behavior";
        });
    REQUIRE(authored_behavior != result.at("components").end());
    REQUIRE(authored_behavior->at("behavior").at("attachment_handle")
                .is_number_unsigned());
    REQUIRE(parsed[1].at("error").at("data").at("code") ==
            "prefab_generated_read_only");
    REQUIRE(parsed[2].at("error").at("data").at("code") ==
            "save_unavailable");
    REQUIRE(parsed[3].at("error").at("data").at("code") ==
            "prefab_persistence_unsupported");
    REQUIRE(parsed[4].at("result").at("schema_version") == 2);
    REQUIRE(parsed[4].at("result").contains("prefab_closure"));
    REQUIRE(parsed[5].at("error").at("data").at("code") ==
            "prefab_persistence_unsupported");
    REQUIRE(parsed[6].at("error").at("data").at("code") ==
            "invalid_params");
    REQUIRE(parsed[7].at("error").at("data").at("code") ==
            "invalid_params");
    REQUIRE(parsed[8].at("error").at("data").at("code") ==
            "object_not_found");
    REQUIRE(parsed[9].at("error").at("data").at("code") ==
            "invalid_params");
    const auto &default_result = parsed[10].at("result");
    const auto &default_hp =
        default_result.at("prefab_instance").at("parameters")[0];
    REQUIRE(default_hp.at("name") == "hp");
    REQUIRE(default_hp.at("value_resolved") == 30);
    REQUIRE(default_hp.at("source") == "default");
    REQUIRE_FALSE(default_hp.contains("value_authored"));
    REQUIRE(parsed[11].at("error").at("data").at("code") ==
            "invalid_params");
    REQUIRE(parsed[12].at("error").at("data").at("code") ==
            "invalid_params");
    REQUIRE(parsed[13].at("error").at("data").at("code") ==
            "invalid_params");
    REQUIRE(parsed[14].at("error").at("data").at("code") ==
            "invalid_params");
    REQUIRE(parsed[15].at("error").at("data").at("code") ==
            "prefab_generated_read_only");
    REQUIRE(parsed[16].at("error").at("data").at("code") ==
            "prefab_generated_read_only");
    REQUIRE(parsed[17].at("result").at("status") == "accepted");
    REQUIRE(parsed[18].at("error").at("data").at("code") ==
            "invalid_params");
    REQUIRE(AuthoringSceneAuthority::encodeSemantic(target.projectionDocument()) ==
            before_bytes);
    REQUIRE(target.projectionDocument().revision() == before_revision);
    REQUIRE(generatedByKey(instanceObject(target.projectionResolvedScene()), "sprite")
                .origin.generated()->stable_generated_id == gid);
    const auto after_arena_snapshot = arena.snapshot();
    REQUIRE(arenaIdentity(after_arena_snapshot) == before_arena_identity);
    REQUIRE(after_arena_snapshot.size() == before_arena_snapshot.size());
    for (const auto &attachment : after_arena_snapshot) {
        REQUIRE(GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(
            attachment.entity));
    }
    service.commitPendingEdits();
    const auto authored_result = service.getEditResult(
        {{"ticket", parsed[17].at("result").at("ticket")}});
    REQUIRE(authored_result.at("status") == "committed");
    REQUIRE(target.projectionDocument().revision().value ==
            before_revision.value + 1U);
    const auto &authored_target =
        AuthoringSceneAuthority::rawView(target.projectionDocument())
            .documentJson()
            .at("scenes").at("main").at("objects").at(0);
    REQUIRE(authored_target.at("components").at(0).at("pos") ==
            Json::array({1.0F, 2.0F, 3.0F}));
    REQUIRE(arenaIdentity(arena.snapshot()) == before_arena_identity);
    arena.deactivateAll();
    std::unordered_set<std::uint64_t> removed_entities;
    for (const auto &attachment : before_arena_snapshot) {
        const auto key = (static_cast<std::uint64_t>(attachment.entity.index) << 32U) |
                         attachment.entity.generation;
        if (removed_entities.insert(key).second) {
            REQUIRE(GameObjects::remove(attachment.entity));
        }
    }
}
#endif

TEST_CASE("WP361a SnapshotV1 and lower import gates reject every prefab persistence escape",
          "[wp361a][prefab][persistence][generation]") {
    ensureWp361Logger();
    const Json plain_scene{
        {"schema", "pelican.scene"}, {"version", 1},
        {"scenes", {{"main", {{"objects", Json::array({
             {{"name", "Plain"}, {"components", Json::array({transform()})}}
         })}}}}}};
    const auto make_service = [](const AuthoringSceneDocument &document,
                                 const ResolvedScene &resolved) {
        return EditorCommandService{EditorCommandServiceDependencies{
            .document = [&]() -> const AuthoringSceneDocument & {
                return document;
            },
            .resolved_scene = [&]() -> const ResolvedScene & {
                return resolved;
            },
            .current_scene_id = [] { return std::string{"main"}; },
        }};
    };

    const auto live_v1 = AuthoringSceneDocument::load(
        plain_scene.dump(), SceneRevision{1});
    const auto resolved_v1 = ResolvedSceneResolver::resolve(
        live_v1, SceneResolverGeneration{1}, {}, {}, {});
    auto service_v1 = make_service(live_v1, resolved_v1);
    REQUIRE(service_v1.exportSceneSnapshot(
                          ExportSceneSnapshotRequestV1{.schema_version = 1})
                .schema_version == 1);

    auto plain_v2_json = plain_scene;
    plain_v2_json["version"] = 2;
    const auto live_v2 = AuthoringSceneDocument::load(
        plain_v2_json.dump(), SceneRevision{1});
    const auto resolved_v2 = ResolvedSceneResolver::resolve(
        live_v2, SceneResolverGeneration{1}, {}, {}, {});
    auto service_v2 = make_service(live_v2, resolved_v2);
    try {
        (void)service_v2.exportSceneSnapshot(
            ExportSceneSnapshotRequestV1{.schema_version = 1});
        FAIL("SnapshotV1 accepted a formatVersion 2 document without prefabs");
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() ==
                EditorCommandErrorCode::PrefabPersistenceUnsupported);
    }

    const auto incoming_prefab_bytes = sceneFixture().dump();
    try {
        (void)service_v1.importSceneSnapshot(ImportSceneSnapshotRequestV1{
            .schema_version = 99,
            .semantic_scene_bytes = incoming_prefab_bytes,
            .digest = {.algorithm = "sha256", .hex = "deliberately-later"},
            .current_scene_id = "main",
        });
        FAIL("incoming prefab escaped through the snapshot version gate");
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() ==
                EditorCommandErrorCode::PrefabPersistenceUnsupported);
    }

    Wp361TemporaryProject temporary;
    temporary.write("scene.json", plain_scene.dump());
    const Json project{
        {"schema", "pelican.project"}, {"version", 1},
        {"name", "wp361a-persistence"}, {"engine_min_version", "0.1.0"},
        {"basic_config", {{"default_scene_id", "main"},
                          {"scene_data_json", "scene.json"}}}};
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(temporary.root(), false);
    GET_MODULE(ProjectSource).setProjectData(project.dump());
    auto &config = GET_MODULE(ProjectBasicConfig);
    (void)config.sceneProjectionState();
    requirePrefabError(PrefabErrorCode::PrefabPersistenceUnsupported, [&] {
        (void)config.importSceneDocument(incoming_prefab_bytes, [] {});
    });

    const auto initial_provider_generation =
        config.resolvedScene().bindableProvider().generation();
    REQUIRE(initial_provider_generation != 0);
    try {
        config.refreshResolvedScene([] {
            throw std::runtime_error("injected reload failure");
        });
        FAIL("failed refresh unexpectedly succeeded");
    } catch (const std::runtime_error &error) {
        REQUIRE(std::string{error.what()} == "injected reload failure");
    }
    REQUIRE(config.resolvedScene().bindableProvider().generation() ==
            initial_provider_generation);
    config.refreshResolvedScene([] {});
    REQUIRE(config.resolvedScene().bindableProvider().generation() ==
            initial_provider_generation + 1U);
}

TEST_CASE("WP361 authored detector remains and ImGui never creates generated mutation UI",
          "[wp361][prefab][detector][imgui]") {
    STATIC_REQUIRE_FALSE(std::is_constructible_v<ResolvedComponentOrigin,
                                                 GeneratedComponentOrigin>);

    auto fixture = prepareFixture();
    const auto &component = generatedByKey(instanceObject(fixture.resolved), "sprite");
    EditorComponentQueryResult query{
        .name = component.name,
        .component_index = component.merged_component_index,
        .authored_json = component.effective_json,
        .editable = false,
        .codec_state = ComponentCodecState::Registered,
        .codec_name = "sprite_view",
        .schema_state = EditorComponentSchemaState::Available,
        .generated = *component.origin.generated(),
        .generated_resolved_json = component.effective_json,
    };
    REQUIRE(makeInspectorWidgetPlan(query).empty());

    const auto forged = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"components":[{"name":"external","generated":true}]}]}}})json",
        SceneRevision{1});
    REQUIRE_THROWS_AS(ResolvedSceneResolver::resolve(
                          forged, SceneResolverGeneration{1}),
                      ResolvedSceneError);
}

TEST_CASE("WP361 and WP362 v4 JSON examples remain syntax parse gates",
          "[wp361][prefab][json-examples]") {
    const std::vector<std::string> examples{
        R"json({"generated":{"stable_generated_id":"gid_0123456789abcdef0123456789abcdef","source":{"prefab":"enemy_grunt","instance_id":"gi_7f3a","component_key":"sprite_view","digest":{"algorithm":"sha256","hex":"0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345"}},"resolved_json":{"name":"sprite_view","texture":"white"},"editable":false}})json",
        R"json({"prefab_instance":{"ref":"enemy_grunt","instance_id":"gi_7f3a","closure_generation":3,"provider_generation":1,"parameters":[{"name":"hp","value_resolved":55,"source":"override","value_authored":55},{"name":"tint","value_resolved":[1,1,1,1],"source":"default"}]}})json",
        R"json({"schema_version":2,"allow_pending":false})json",
        R"json({"prefab_closure":{"scenes":[{"scene_id":"main","digest":{"algorithm":"sha256","hex":"0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345"}}],"prefabs":[{"name":"enemy_grunt","digest":{"algorithm":"sha256","hex":"0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345"}}],"behaviors":[{"stable_name":"grunt_ai","schema_version":1,"params_schema_fingerprint":"585b..."}],"provider_fingerprint":{"algorithm":"sha256","hex":"0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345"},"resolver_inputs_fingerprint":{"algorithm":"sha256","hex":"0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345"}}})json",
        R"json({"op":"set_prefab_parameter","object":12,"parameter":"hp","value":55})json",
        R"json({"op":"unset_prefab_parameter","object":12,"parameter":"hp"})json",
    };
    for (const auto example : examples) REQUIRE_NOTHROW(Json::parse(example));
}

TEST_CASE("WP362 authored behavior absent from provider remains pending without prefab",
          "[wp362][snapshot-v2][closure][behavior][regression]") {
    ensureBehavior();
    const std::string authored_name =
        "wp362_authored_provider_absent";
    const auto document = AuthoringSceneDocument::load(
        R"json({"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[{"name":"AuthoredOnly","components":[{"name":"behavior","type":"wp362_authored_provider_absent","params":{}}]}]}}})json",
        SceneRevision{30});
    const auto provider = buildProductionBindableProviderSnapshot(40);
    REQUIRE(std::none_of(
        provider.behaviorDependencies().begin(),
        provider.behaviorDependencies().end(), [authored_name](const auto &dependency) {
            return dependency.stable_name == authored_name;
        }));

    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{50}, {}, {}, provider);

    REQUIRE(resolved.prefabClosure().behaviors.empty());
    REQUIRE(resolved.behaviorReloadSources().size() == 1);
    REQUIRE(resolved.behaviorReloadSources()[0].stable_name == authored_name);
    const auto *scene = resolved.findScene("main");
    REQUIRE(scene != nullptr);
    REQUIRE(scene->objects.size() == 1);
    REQUIRE(scene->objects[0].components.size() == 1);
    REQUIRE(scene->objects[0].components[0].origin.authored() != nullptr);
    REQUIRE(scene->objects[0].components[0].behavior_pending);
}

TEST_CASE("WP362 generated behavior absent from provider remains named fatal",
          "[wp362][snapshot-v2][closure][behavior][negative]") {
    ensureBehavior();
    const auto production = buildProductionBindableProviderSnapshot(41);
    const BindableProviderSnapshot without_behavior_dependency{
        42,
        std::vector<BindableDescriptor>{production.descriptors().begin(),
                                        production.descriptors().end()}};
    const auto registry = registryFor(prefabFixture());
    const auto document = AuthoringSceneDocument::load(
        sceneFixture().dump(), SceneRevision{31});

    requirePrefabError(PrefabErrorCode::PrefabProviderStale, [&] {
        (void)ResolvedSceneResolver::resolve(
            document, SceneResolverGeneration{51}, {}, registry,
            without_behavior_dependency);
    });
}

TEST_CASE("WP362 closure contains only generated behavior dependency tuples",
          "[wp362][snapshot-v2][closure][behavior]") {
    ensureBehavior();
    const std::string authored_name =
        "wp362_authored_not_in_prefab_closure";
    auto scene = sceneFixture();
    scene["scenes"]["main"]["objects"][1]["components"][1]["type"] =
        authored_name;
    const auto document = AuthoringSceneDocument::load(
        scene.dump(), SceneRevision{32});
    const auto registry = registryFor(prefabFixture());
    const auto provider = buildProductionBindableProviderSnapshot(43);
    const auto expected = std::find_if(
        provider.behaviorDependencies().begin(),
        provider.behaviorDependencies().end(), [](const auto &dependency) {
            return dependency.stable_name == "wp361_behavior";
        });
    REQUIRE(expected != provider.behaviorDependencies().end());
    REQUIRE(std::none_of(
        provider.behaviorDependencies().begin(),
        provider.behaviorDependencies().end(), [authored_name](const auto &dependency) {
            return dependency.stable_name == authored_name;
        }));

    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{52}, {}, registry, provider);
    const auto &behaviors = resolved.prefabClosure().behaviors;

    REQUIRE(behaviors.size() == 1);
    REQUIRE(behaviors[0].stable_name == expected->stable_name);
    REQUIRE(behaviors[0].schema_version == expected->schema_version);
    REQUIRE(behaviors[0].params_schema_fingerprint ==
            expected->params_schema_fingerprint);
    REQUIRE(std::none_of(behaviors.begin(), behaviors.end(),
                         [authored_name](const auto &dependency) {
                             return dependency.stable_name == authored_name;
                         }));
}

TEST_CASE("WP362 resolver input bytes and dependency closure are complete",
          "[wp362][snapshot-v2][closure][fingerprint]") {
    ensureBehavior();
    ResolvedSceneDefaults defaults;
    REQUIRE(resolverInputsFingerprint(defaults) ==
            independentResolverInputsFingerprint(defaults));

    const auto baseline = resolverInputsFingerprint(defaults);
    REQUIRE(baseline ==
            "e36e95129b8b130f625c0d104e4a75563d74812fdcd9cebbf3b658c160a624a0");
    const auto require_changed = [&](const auto &mutate) {
        auto candidate = defaults;
        mutate(candidate);
        REQUIRE(resolverInputsFingerprint(candidate) != baseline);
        REQUIRE(resolverInputsFingerprint(candidate) ==
                independentResolverInputsFingerprint(candidate));
    };
    require_changed([](auto &value) {
        value.camera_projection.kind = CameraProjectionKind::Orthographic;
    });
    require_changed([](auto &value) { value.camera_projection.yfov += 0.125F; });
    require_changed([](auto &value) { value.camera_projection.znear += 0.125F; });
    require_changed([](auto &value) { value.camera_projection.zfar += 1.0F; });
    require_changed([](auto &value) { value.camera_projection.aspect = 1.25F; });
    require_changed([](auto &value) { value.camera_projection.xmag += 0.25F; });
    require_changed([](auto &value) { value.camera_projection.ymag += 0.25F; });
    require_changed([](auto &value) {
        value.camera_sprite.pixel_perfect = CameraPixelPerfectMode::strict;
    });
    require_changed([](auto &value) {
        value.camera_sprite.sort = CameraSpriteSortPolicy::declaration;
    });

    auto positive_zero = defaults;
    positive_zero.camera_projection.aspect = 0.0F;
    auto negative_zero = positive_zero;
    negative_zero.camera_projection.aspect = -0.0F;
    REQUIRE(resolverInputsFingerprint(positive_zero) ==
            resolverInputsFingerprint(negative_zero));
    positive_zero.camera_projection.xmag = 0.0F;
    negative_zero.camera_projection.xmag = -0.0F;
    REQUIRE(resolverInputsFingerprint(positive_zero) ==
            resolverInputsFingerprint(negative_zero));

    auto nan = defaults;
    nan.camera_projection.znear = std::numeric_limits<float>::quiet_NaN();
    requirePrefabError(PrefabErrorCode::PrefabDependencyMismatch, [&] {
        (void)resolverInputsFingerprint(nan);
    });
    nan = defaults;
    nan.camera_projection.aspect =
        std::numeric_limits<float>::quiet_NaN();
    requirePrefabError(PrefabErrorCode::PrefabDependencyMismatch, [&] {
        (void)resolverInputsFingerprint(nan);
    });

    auto scene = sceneFixture();
    scene["scenes"]["inactive"] = scene["scenes"]["main"];
    scene["scenes"]["main"] = {
        {"objects", Json::array({
             {{"name", "OnlyActive"},
              {"components", Json::array({transform()})}}
         })}};
    const auto document = AuthoringSceneDocument::load(
        scene.dump(), SceneRevision{31});
    const auto provider = buildProductionBindableProviderSnapshot(41);
    const auto used_registry = registryFor(prefabFixture());
    const auto used = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{51}, defaults, used_registry,
        provider);
    REQUIRE(used.prefabClosure().scenes.size() == 2);
    REQUIRE(used.prefabClosure().scenes[0].scene_id == "inactive");
    REQUIRE(used.prefabClosure().scenes[1].scene_id == "main");
    REQUIRE(used.prefabClosure().prefabs.size() == 1);
    REQUIRE(used.prefabClosure().prefabs[0].name == "unit");
    REQUIRE(used.prefabClosure().behaviors.size() == 1);
    REQUIRE(used.prefabClosure().behaviors[0].stable_name ==
            "wp361_behavior");
    REQUIRE(used.prefabClosure().provider_fingerprint_sha256.size() == 64);
    REQUIRE(used.prefabClosure().resolver_inputs_fingerprint_sha256 ==
            baseline);

    for (const auto &authored_scene : AuthoringSceneAuthority::query(document)) {
        const auto scene_bytes = authored_scene.authoredJson().dump();
        const auto expected = picosha2::hash256_hex_string(
            scene_bytes.begin(), scene_bytes.end());
        const auto found = std::find_if(
            used.prefabClosure().scenes.begin(),
            used.prefabClosure().scenes.end(), [&](const auto &candidate) {
                return candidate.scene_id == authored_scene.scene_id;
            });
        REQUIRE(found != used.prefabClosure().scenes.end());
        REQUIRE(found->digest_sha256 == expected);
    }

    const auto with_unused = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{52}, defaults,
        registryWithUnused(prefabFixture()), provider);
    REQUIRE(used.prefabClosure().sameWireIdentity(
        with_unused.prefabClosure()));
    REQUIRE(with_unused.prefabClosure().prefabs.size() == 1);

    std::vector<BindableDescriptor> changed_descriptors(
        provider.descriptors().begin(), provider.descriptors().end());
    const auto defaulted_descriptor = std::find_if(
        changed_descriptors.begin(), changed_descriptors.end(),
        [](const auto &descriptor) {
            return descriptor.provider_name == "collider" &&
                   descriptor.json_pointer == "/radius" &&
                   descriptor.canonical_default.has_value();
        });
    REQUIRE(defaulted_descriptor != changed_descriptors.end());
    defaulted_descriptor->canonical_default = OJson(0.75);
    const BindableProviderSnapshot changed_provider{
        42, std::move(changed_descriptors),
        std::vector<BehaviorDependencyDescriptor>(
            provider.behaviorDependencies().begin(),
            provider.behaviorDependencies().end())};
    REQUIRE(std::string{changed_provider.fingerprint()} !=
            std::string{provider.fingerprint()});
}

TEST_CASE("WP362 SnapshotV2 routes all four wire shapes and checks closure before scene selection",
          "[wp362][snapshot-v2][rpc][parse-gate]") {
    ensureBehavior();
    const auto document = AuthoringSceneDocument::load(
        sceneFixture().dump(), SceneRevision{61});
    auto defaults = ResolvedSceneDefaults{};
    const auto registry = registryFor(prefabFixture());
    const auto provider = buildProductionBindableProviderSnapshot(71);
    const auto resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{81}, defaults, registry, provider);
    bool imported = false;
    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = [&]() -> const ResolvedScene & { return resolved; },
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot_versioned =
            [&](std::string_view bytes, std::string_view scene_id,
                std::uint64_t version) {
                REQUIRE(std::string{bytes} ==
                        AuthoringSceneAuthority::encodeSemantic(document));
                REQUIRE(std::string{scene_id} == "main");
                REQUIRE(version == 2);
                imported = true;
                return SceneRevision{62};
            },
    }};
    EditorCommandRpcAdapter adapter{service};

    const auto export_request = Json::parse(
        R"json({"schema_version":2,"allow_pending":false})json");
    const auto exported = adapter.exportSceneSnapshot(export_request);
    REQUIRE(exported.at("schema_version") == 2);
    REQUIRE(exported.at("prefab_closure").at("scenes").size() == 1);
    REQUIRE(exported.at("prefab_closure").at("prefabs")[0].at("name") ==
            "unit");
    const auto reparsed_export_response = Json::parse(exported.dump());
    REQUIRE(reparsed_export_response == exported);

    Json import_request{
        {"schema_version", 2},
        {"semantic_scene_bytes", exported.at("semantic_scene_bytes")},
        {"digest", exported.at("digest")},
        {"current_scene_id", "main"},
        {"prefab_closure", exported.at("prefab_closure")},
    };

    const auto require_import_code = [&](Json request,
                                         EditorCommandErrorCode code) {
        try {
            (void)adapter.importSceneSnapshot(request);
            FAIL("SnapshotV2 validation gate accepted invalid input");
        } catch (const EditorCommandError &error) {
            REQUIRE(error.code() == code);
        }
    };
    auto unsupported = import_request;
    unsupported["schema_version"] = 3;
    require_import_code(std::move(unsupported),
                        EditorCommandErrorCode::UnsupportedSnapshotVersion);
    auto oversized = import_request;
    oversized["semantic_scene_bytes"] =
        std::string(maxSceneSnapshotBytes + 1U, 'x');
    require_import_code(std::move(oversized),
                        EditorCommandErrorCode::SnapshotTooLarge);
    auto bad_digest = import_request;
    bad_digest["digest"]["algorithm"] = "sha512";
    require_import_code(std::move(bad_digest),
                        EditorCommandErrorCode::DigestMismatch);
    auto malformed = import_request;
    malformed["semantic_scene_bytes"] = "{not-json";
    malformed["digest"]["hex"] = picosha2::hash256_hex_string(
        malformed.at("semantic_scene_bytes").get_ref<const std::string &>());
    require_import_code(std::move(malformed),
                        EditorCommandErrorCode::SnapshotInvalid);

    const auto import_result = adapter.importSceneSnapshot(import_request);
    REQUIRE(imported);
    REQUIRE(import_result == OJson::parse(
        R"json({"status":"imported","scene_revision":62,"current_scene_id":"main"})json"));

    auto invalid_scene = Json::parse(
        exported.at("semantic_scene_bytes").get<std::string>());
    invalid_scene["scenes"]["main"]["objects"][1]["prefab"]
                 ["parameters"]["hp"] = "not-an-i32";
    auto invalid_request = import_request;
    invalid_request["semantic_scene_bytes"] = invalid_scene.dump();
    invalid_request["digest"]["hex"] = picosha2::hash256_hex_string(
        invalid_request.at("semantic_scene_bytes").get_ref<const std::string &>());
    // The changed scene also makes the supplied closure stale. Gate 4 must
    // win over closure gate 5', proving the inherited V1 ordering.
    try {
        (void)adapter.importSceneSnapshot(invalid_request);
        FAIL("invalid prefab payload reached closure comparison");
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() == EditorCommandErrorCode::SnapshotInvalid);
        REQUIRE(error.detail() ==
                std::optional<std::string>{"/prefab/parameters/hp"});
    }

    const auto require_dependency_detail = [&](Json request,
                                               std::string_view detail) {
        try {
            (void)adapter.importSceneSnapshot(request);
            FAIL("mismatched closure element was accepted");
        } catch (const EditorCommandError &error) {
            REQUIRE(error.code() ==
                    EditorCommandErrorCode::PrefabDependencyMismatch);
            REQUIRE(error.detail() ==
                    std::optional<std::string>{std::string{detail}});
        }
    };
    auto scene_mismatch = import_request;
    scene_mismatch["prefab_closure"]["scenes"][0]["digest"]["hex"] =
        std::string(64, '0');
    require_dependency_detail(
        std::move(scene_mismatch), "/prefab_closure/scenes/0/digest");
    auto prefab_mismatch = import_request;
    prefab_mismatch["prefab_closure"]["prefabs"][0]["digest"]["hex"] =
        std::string(64, '0');
    require_dependency_detail(
        std::move(prefab_mismatch), "/prefab_closure/prefabs/0/digest");
    auto behavior_mismatch = import_request;
    behavior_mismatch["prefab_closure"]["behaviors"][0]
                     ["schema_version"] =
        behavior_mismatch["prefab_closure"]["behaviors"][0]
                         ["schema_version"].get<std::uint64_t>() + 1U;
    require_dependency_detail(
        std::move(behavior_mismatch),
        "/prefab_closure/behaviors/0/schema_version");
    auto provider_mismatch = import_request;
    provider_mismatch["prefab_closure"]["provider_fingerprint"]["hex"] =
        std::string(64, '0');
    require_dependency_detail(
        std::move(provider_mismatch),
        "/prefab_closure/provider_fingerprint");
    auto missing_prefab = import_request;
    missing_prefab["prefab_closure"]["prefabs"] = Json::array();
    require_dependency_detail(
        std::move(missing_prefab), "/prefab_closure/prefabs/0");

    const auto unused_resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{82}, defaults,
        registryWithUnused(prefabFixture()), provider);
    EditorCommandService unused_service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = [&]() -> const ResolvedScene & {
            return unused_resolved;
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot_versioned =
            [](std::string_view, std::string_view, std::uint64_t) {
                return SceneRevision{63};
            },
    }};
    EditorCommandRpcAdapter unused_adapter{unused_service};
    REQUIRE(unused_adapter.importSceneSnapshot(import_request).at("status") ==
            "imported");

    auto other_defaults = defaults;
    other_defaults.camera_projection.yfov += 0.25F;
    const auto other_resolved = ResolvedSceneResolver::resolve(
        document, SceneResolverGeneration{83}, other_defaults, registry,
        provider);
    EditorCommandService other_service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & { return document; },
        .resolved_scene = [&]() -> const ResolvedScene & {
            return other_resolved;
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .import_scene_snapshot_versioned =
            [](std::string_view, std::string_view, std::uint64_t) {
                FAIL("closure mismatch reached the import publication callback");
                return SceneRevision{0};
            },
    }};
    EditorCommandRpcAdapter other_adapter{other_service};
    auto wrong_scene = import_request;
    wrong_scene["current_scene_id"] = "missing";
    try {
        (void)other_adapter.importSceneSnapshot(wrong_scene);
        FAIL("camera-only dependency change was accepted");
    } catch (const EditorCommandError &error) {
        REQUIRE(error.code() ==
                EditorCommandErrorCode::PrefabDependencyMismatch);
        REQUIRE(error.detail() ==
                std::optional<std::string>{
                    "/prefab_closure/resolver_inputs_fingerprint"});
    }
    auto absent_scene = import_request;
    absent_scene["current_scene_id"] = "missing";
    require_import_code(std::move(absent_scene),
                        EditorCommandErrorCode::SceneNotFound);

    const auto set_ast = parseEditorMutationRequest(
        {{"actor_id", 1}, {"scene_revision", 61},
         {"closure_generation", 81}, {"provider_generation", 71},
         {"operations", Json::array({Json::parse(
              R"json({"op":"set_prefab_parameter","object":12,"parameter":"hp","value":55})json")})}},
        "edit");
    REQUIRE(set_ast.prefab_mutation);
    const auto unset_ast = parseEditorMutationRequest(
        {{"actor_id", 1}, {"scene_revision", 61},
         {"closure_generation", 81}, {"provider_generation", 71},
         {"operations", Json::array({Json::parse(
              R"json({"op":"unset_prefab_parameter","object":12,"parameter":"hp"})json")})}},
        "edit");
    REQUIRE(unset_ast.prefab_mutation);
}

TEST_CASE("WP362 real RpcServer run edits prefab parameters with three-token CAS and journal replay",
          "[wp362][prefab-edit][rpc][cas][undo-redo]") {
    ensureBehavior();
    auto fixture = prepareFixture();
    PrefabProjectionTarget target{fixture.document, fixture.registry,
                                  fixture.provider};
    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & {
            return target.projectionDocument();
        },
        .resolved_scene = [&]() -> const ResolvedScene & {
            return target.projectionResolvedScene();
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .edit = EditorEditRuntimeDependencies{
            .document = [&]() -> const AuthoringSceneDocument & {
                return target.projectionDocument();
            },
            .current_scene_id = [] { return std::string{"main"}; },
            .execute = [&](const EditorEditExecutionRequest &request) {
                EditorProjectionTransaction transaction{target,
                                                        request.base_revision};
                std::vector<EditorProjectionAdapter *> adapters;
                return transaction.commit(request.commands, adapters);
            },
            .gate = [] { return EditorGateObservation{}; },
            .prefab_registry = [&] {
                return target.projectionResolvedScene().prefabRegistry();
            },
            .bindable_provider = [&] {
                return target.projectionResolvedScene().bindableProvider();
            },
            .closure_generation = [&] {
                return target.projectionResolvedScene()
                    .prefabClosure().generation;
            },
        },
    }};
    EditorCommandRpcAdapter adapter{service};
    std::uint64_t request_id = 1;
    const auto run_rpc = [&](std::string method, Json params) {
        const Json request{{"jsonrpc", "2.0"}, {"id", request_id++},
                           {"method", std::move(method)},
                           {"params", std::move(params)}};
        std::istringstream input{request.dump() + "\n"};
        std::ostringstream output;
        RpcServer server{input, output};
        configureEditorRpcHandlers(
            server, adapter,
            {.snapshot_imported = [] {},
             .save_busy = [] { return false; }});
        server.run();
        std::string line;
        std::istringstream response_lines{output.str()};
        REQUIRE(std::getline(response_lines, line));
        REQUIRE_FALSE(line.empty());
        return Json::parse(line);
    };

    const auto session = run_rpc(
        "open_editor_session", {{"display_name", "wp362 rpc"}});
    const auto actor = session.at("result").at("actor_id").get<std::uint64_t>();
    const auto object_id =
        instanceObject(target.projectionResolvedScene()).authoring_object_id.value;
    const auto tokens = [&] {
        const auto &resolved = target.projectionResolvedScene();
        return Json{{"actor_id", actor},
                    {"scene_revision",
                     target.projectionDocument().revision().value},
                    {"closure_generation",
                     resolved.prefabClosure().generation},
                    {"provider_generation",
                     resolved.bindableProvider().generation()},
                    {"object", object_id},
                    {"parameter", "hp"}};
    };
    const auto commit_ticket = [&](const Json &accepted_response) {
        const auto &accepted = accepted_response.at("result");
        REQUIRE(accepted.at("status") == "accepted");
        const auto ticket = accepted.at("ticket").get<std::string>();
        service.commitPendingEdits();
        return run_rpc("get_edit_result", {{"ticket", ticket}}).at("result");
    };
    const auto parameter = [&](std::string_view name) -> Json {
        const auto queried = run_rpc(
            "get_components", {{"authoring_object_id", object_id}});
        const auto &parameters = queried.at("result")
                                     .at("prefab_instance")
                                     .at("parameters");
        const auto found = std::find_if(
            parameters.begin(), parameters.end(), [&](const auto &candidate) {
                return candidate.at("name").template get<std::string>() ==
                       name;
            });
        REQUIRE(found != parameters.end());
        return *found;
    };

    auto set_params = tokens();
    set_params["value"] = 55;
    const auto set = commit_ticket(run_rpc("set_prefab_parameter", set_params));
    REQUIRE(set.at("status") == "committed");
    REQUIRE(parameter("hp").at("value_resolved") == 55);
    REQUIRE(parameter("hp").at("source") == "override");
    const auto &set_record = service.editCoordinator()->journal().back();
    REQUIRE(set_record.ordered_forward == std::vector<OJson>{
        OJson{{"op", "set_prefab_parameter"}, {"object", object_id},
              {"parameter", "hp"}, {"value", 55}}});
    REQUIRE(set_record.ordered_inverse == std::vector<OJson>{
        OJson{{"op", "unset_prefab_parameter"}, {"object", object_id},
              {"parameter", "hp"}}});

    auto undo_params = tokens();
    undo_params.erase("object");
    undo_params.erase("parameter");
    undo_params["transaction_id"] = set.at("transaction_id");
    const auto undone = commit_ticket(run_rpc("undo", undo_params));
    REQUIRE(undone.at("status") == "committed");
    REQUIRE(parameter("hp").at("value_resolved") == 30);
    REQUIRE(parameter("hp").at("source") == "default");

    auto redo_params = tokens();
    redo_params.erase("object");
    redo_params.erase("parameter");
    redo_params["transaction_id"] = undone.at("transaction_id");
    const auto redone = commit_ticket(run_rpc("redo", redo_params));
    REQUIRE(redone.at("status") == "committed");
    REQUIRE(parameter("hp").at("value_resolved") == 55);
    REQUIRE(parameter("hp").at("source") == "override");

    auto stale_scene = tokens();
    stale_scene["scene_revision"] =
        target.projectionDocument().revision().value - 1U;
    stale_scene["value"] = 56;
    REQUIRE(run_rpc("set_prefab_parameter", stale_scene)
                .at("result").at("error").at("code") == "stale_revision");

    auto stale_closure = tokens();
    stale_closure["closure_generation"] =
        target.projectionResolvedScene().prefabClosure().generation - 1U;
    stale_closure["value"] = 56;
    REQUIRE(run_rpc("set_prefab_parameter", stale_closure)
                .at("result").at("error").at("code") ==
            "prefab_dependency_mismatch");

    auto stale_provider = tokens();
    stale_provider["provider_generation"] =
        target.projectionResolvedScene().bindableProvider().generation() - 1U;
    stale_provider["value"] = 56;
    REQUIRE(run_rpc("set_prefab_parameter", stale_provider)
                .at("result").at("error").at("code") ==
            "prefab_provider_stale");

    auto required_unset = tokens();
    required_unset["parameter"] = "target";
    REQUIRE(run_rpc("unset_prefab_parameter", required_unset)
                .at("result").at("error").at("code") ==
            "prefab_parameter_required");

    const auto unset =
        commit_ticket(run_rpc("unset_prefab_parameter", tokens()));
    REQUIRE(unset.at("status") == "committed");
    REQUIRE(parameter("hp").at("value_resolved") == 30);
    REQUIRE(parameter("hp").at("source") == "default");
    const auto &unset_record = service.editCoordinator()->journal().back();
    REQUIRE(unset_record.ordered_forward == std::vector<OJson>{
        OJson{{"op", "unset_prefab_parameter"}, {"object", object_id},
              {"parameter", "hp"}}});
    REQUIRE(unset_record.ordered_inverse == std::vector<OJson>{
        OJson{{"op", "set_prefab_parameter"}, {"object", object_id},
              {"parameter", "hp"}, {"value", 55}}});
}

TEST_CASE("WP362 closure CAS closes kind-object indirect prefab dependencies",
          "[wp362][prefab-edit][cas][indirect]") {
    ensureBehavior();
    auto a = prefabFixture();
    a["parameters"][1]["required_components"] =
        Json::array({"sprite_view"});
    Json b{{"schema", "pelican.prefab"}, {"version", 1},
           {"name", "targetunit"},
           {"components", Json::array({
                {{"name", "sprite_view"}, {"id", "target_sprite"},
                 {"texture", "white"}}
            })}};
    const auto make_registry = [&](const Json &target_prefab,
                                   std::uint64_t generation) {
        const auto a_bytes = a.dump();
        const auto b_bytes = target_prefab.dump();
        return buildPrefabRegistrySnapshot(
            std::vector<PrefabRegistryEntry>{
                {"unit", "a.json"}, {"targetunit", "b.json"}},
            [a_bytes, b_bytes](std::string_view path) {
                return path == "a.json" ? a_bytes : b_bytes;
            }, generation);
    };
    const Json scene{
        {"schema", "pelican.scene"}, {"version", 2},
        {"scenes", {{"main", {{"objects", Json::array({
             {{"name", "Target"}, {"components", Json::array({transform()})},
              {"prefab", {{"ref", "targetunit"},
                          {"instance_id", "gi_target"}}}},
             prefabObject("Instance", "gi_a", "Target")
         })}}}}}};
    auto registry = make_registry(b, 101);
    auto provider = buildProductionBindableProviderSnapshot(102);
    auto document = AuthoringSceneDocument::load(scene.dump(), SceneRevision{103});
    PrefabProjectionTarget target{document, registry, provider};
    const auto old_generation =
        target.projectionResolvedScene().prefabClosure().generation;
    const auto old_a_digest =
        target.projectionResolvedScene().prefabRegistry().find("unit")
            ->digest_sha256;

    b["components"][0]["color"] = {0.25, 0.5, 0.75, 1.0};
    target.refreshDependencies(make_registry(b, 104), provider);
    REQUIRE(target.projectionResolvedScene().prefabRegistry().find("unit")
                ->digest_sha256 == old_a_digest);
    REQUIRE(target.projectionResolvedScene().prefabClosure().generation !=
            old_generation);

    EditorEditCoordinator edits{EditorEditRuntimeDependencies{
        .document = [&]() -> const AuthoringSceneDocument & {
            return target.projectionDocument();
        },
        .current_scene_id = [] { return std::string{"main"}; },
        .execute = [&](const EditorEditExecutionRequest &request) {
            EditorProjectionTransaction transaction{target,
                                                    request.base_revision};
            std::vector<EditorProjectionAdapter *> adapters;
            return transaction.commit(request.commands, adapters);
        },
        .gate = [] { return EditorGateObservation{}; },
        .prefab_registry = [&] {
            return target.projectionResolvedScene().prefabRegistry();
        },
        .bindable_provider = [&] {
            return target.projectionResolvedScene().bindableProvider();
        },
        .closure_generation = [&] {
            return target.projectionResolvedScene().prefabClosure().generation;
        },
    }};
    const auto actor = edits.openSession({{"display_name", "indirect"}})
                           .at("actor_id").get<std::uint64_t>();
    const auto instance = instanceObject(target.projectionResolvedScene());
    const auto rejected = edits.enqueue(
        {{"actor_id", actor},
         {"scene_revision", target.projectionDocument().revision().value},
         {"closure_generation", old_generation},
         {"provider_generation", provider.generation()},
         {"operations", Json::array({
              {{"op", "set_prefab_parameter"},
               {"object", instance.authoring_object_id.value},
               {"parameter", "hp"}, {"value", 44}}
          })}});
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("error").at("code") ==
            "prefab_dependency_mismatch");
}

TEST_CASE("WP362 save baseline rejects prefab changes before and throughout SAVE0",
          "[wp362][save][prefab-baseline][fault]") {
    ensureBehavior();
    ensureWp361Logger();
    const std::array<std::optional<SceneSaveFaultPoint>, 7> points{
        std::nullopt,
        SceneSaveFaultPoint::AfterEncode,
        SceneSaveFaultPoint::AfterDiskDigest,
        SceneSaveFaultPoint::AfterTemporaryWrite,
        SceneSaveFaultPoint::AfterTemporaryValidation,
        SceneSaveFaultPoint::AfterCachePrepare,
        SceneSaveFaultPoint::BeforeReplace,
    };
    for (const auto point : points) {
        const auto section_name =
            point ? "save fault point " +
                        std::to_string(static_cast<unsigned>(*point))
                  : std::string{"prefab changed before save"};
        DYNAMIC_SECTION(section_name) {
            Wp361TemporaryProject temporary;
            const auto scene_bytes = sceneFixture().dump();
            const auto prefab_bytes = prefabFixture().dump();
            const auto changed_prefab_bytes = prefab_bytes + "\n";
            temporary.write("scene.json", scene_bytes);
            temporary.write("unit.prefab.json", prefab_bytes);
            const Json project{
                {"schema", "pelican.project"}, {"version", 1},
                {"name", "wp362-save"}, {"engine_min_version", "0.1.0"},
                {"prefabs", Json::array({
                     {{"name", "unit"}, {"path", "unit.prefab.json"}}
                 })},
                {"basic_config", {{"default_scene_id", "main"},
                                  {"scene_data_json", "scene.json"}}}};

            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(temporary.root(), false);
            GET_MODULE(ProjectSource).setProjectData(project.dump());
            auto &config = GET_MODULE(ProjectBasicConfig);
            const auto &old_state = config.sceneProjectionState();
            const auto *old_document_address = &old_state.authoring();
            const auto old_semantic =
                AuthoringSceneAuthority::encodeSemantic(old_state.authoring());
            const auto old_revision = old_state.revision();
            const auto old_resolver_generation =
                old_state.resolverGeneration();
            const auto old_closure = old_state.resolved().prefabClosure();
            const auto old_registry_generation =
                old_state.resolved().prefabRegistry().generation();
            const auto old_provider_generation =
                old_state.resolved().bindableProvider().generation();

            bool mutation_ran = false;
            if (!point) {
                temporary.write("unit.prefab.json", changed_prefab_bytes);
                mutation_ran = true;
            } else {
                config.setSceneSaveHookForTesting(
                    [&](SceneSaveFaultPoint observed) {
                        if (!mutation_ran && observed == *point) {
                            temporary.write("unit.prefab.json",
                                            changed_prefab_bytes);
                            mutation_ran = true;
                        }
                    });
            }
            EditorCommandService service{EditorCommandServiceDependencies{
                .document = [&]() -> const AuthoringSceneDocument & {
                    return config.sceneDocument();
                },
                .resolved_scene = [&]() -> const ResolvedScene & {
                    return config.resolvedScene();
                },
                .current_scene_id = [] { return std::string{"main"}; },
                .save_scene = [&] {
                    const auto saved = config.saveSceneDocument();
                    return SaveSceneResult{
                        .scene_revision = saved.scene_revision,
                        .digest = {.algorithm = "sha256",
                                   .hex = saved.digest},
                        .byte_count = saved.byte_count,
                        .scene_hot_reload = false,
                    };
                },
            }};
            try {
                (void)service.saveScene();
                FAIL("SAVE0 accepted a changed prefab dependency");
            } catch (const EditorCommandError &error) {
                REQUIRE(error.code() ==
                        EditorCommandErrorCode::PrefabDependencyMismatch);
                REQUIRE(error.detail().has_value());
            }
            REQUIRE(mutation_ran);
            REQUIRE(temporary.read("scene.json") == scene_bytes);
            REQUIRE(temporary.read("unit.prefab.json") ==
                    changed_prefab_bytes);
            REQUIRE(&config.sceneDocument() == old_document_address);
            REQUIRE(config.sceneDocument().revision() == old_revision);
            REQUIRE(config.resolvedScene().resolverGeneration() ==
                    old_resolver_generation);
            REQUIRE(AuthoringSceneAuthority::encodeSemantic(
                        config.sceneDocument()) == old_semantic);
            REQUIRE(config.resolvedScene().prefabClosure().generation ==
                    old_closure.generation);
            REQUIRE(config.resolvedScene().prefabClosure().sameWireIdentity(
                old_closure));
            REQUIRE(config.resolvedScene().prefabRegistry().generation() ==
                    old_registry_generation);
            REQUIRE(config.resolvedScene().bindableProvider().generation() ==
                    old_provider_generation);
        }
    }
}

TEST_CASE("WP362 prefab set unset undo redo roll back every projection fault point",
          "[wp362][prefab-edit][fault][atomic][ecs][arena]") {
    ensureBehavior();
    ensureWp361Logger();
    enum class Action : std::uint8_t { Set, Unset, Undo, Redo };
    const std::array actions{Action::Set, Action::Unset, Action::Undo,
                             Action::Redo};
    const std::array fault_points{
        EditorProjectionFaultPoint::Prepare,
        EditorProjectionFaultPoint::Publish,
        EditorProjectionFaultPoint::AfterPublication,
    };
    const auto action_name = [](Action action) {
        switch (action) {
        case Action::Set: return "set";
        case Action::Unset: return "unset";
        case Action::Undo: return "undo";
        case Action::Redo: return "redo";
        }
        return "unknown";
    };

    for (const auto action : actions) {
        for (const auto point : fault_points) {
            const auto section_name =
                std::string{action_name(action)} + " point " +
                std::to_string(static_cast<unsigned>(point));
            DYNAMIC_SECTION(section_name) {
                const Json prefab{
                    {"schema", "pelican.prefab"}, {"version", 1},
                    {"name", "unit"},
                    {"parameters", Json::array({
                         {{"name", "mode"}, {"type", "enum"},
                          {"enum_values", Json::array(
                               {"none", "y_axis", "full"})},
                          {"default", "none"}}
                     })},
                    {"components", Json::array({
                         {{"name", "sprite_view"}, {"id", "sprite"},
                          {"texture", "white"},
                          {"billboard", tagged("mode")}},
                         {{"name", "behavior"}, {"id", "logic"},
                          {"type", "wp361_behavior"},
                          {"params", {{"mode", tagged("mode")}}}}
                     })}};
                const Json scene{
                    {"schema", "pelican.scene"}, {"version", 2},
                    {"scenes", {{"main", {{"objects", Json::array({
                         {{"name", "Instance"},
                          {"components", Json::array({transform()})},
                          {"prefab", {{"ref", "unit"},
                                      {"instance_id", "gi_atomic"}}}}
                     })}}}}}};
                auto registry = registryFor(prefab);
                auto provider = buildProductionBindableProviderSnapshot(202);
                auto document = AuthoringSceneDocument::load(
                    scene.dump(), SceneRevision{201});
                PrefabProjectionTarget target{document, registry, provider};

                FastModuleContainer modules;
                GET_MODULE(ECSPredefinedRegistration).reg();
                auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
                auto &arena = GET_MODULE(BehaviorAttachmentArena);
                const auto &object =
                    instanceObject(target.projectionResolvedScene());
                const auto authoring_object_id = object.authoring_object_id;
                const auto &resolved_sprite = generatedByKey(object, "sprite");
                const auto initial_sprite =
                    std::any_cast<const SpriteViewComponent &>(
                        resolved_sprite.requireRuntimeValue());
                const std::array component_ids{
                    ComponentIdByType<SpriteViewComponent>::value};
                const auto entity = GameObjects::createWithComponents(
                    component_ids, [&](std::span<void *> components) {
                        *static_cast<SpriteViewComponent *>(components[0]) =
                            initial_sprite;
                    });
                const std::vector<EditorProjectionRuntimeObjectBinding> bindings{
                    {.authoring_object_id = authoring_object_id,
                     .entity = entity}};
                auto prepared = prepareResolvedSceneBehaviorAttachments(
                    target.projectionResolvedScene()
                        .findScene("main")->objects,
                    BehaviorRegistryAvailability::active);
                std::vector<BoundSceneBehaviorAttachment> desired;
                for (auto &attachment : prepared) {
                    desired.push_back({.prepared = std::move(attachment),
                                       .entity = entity});
                }
                arena.publishSceneAttachments(std::move(desired));
                arena.activatePublished();

                struct OneFault final : EditorProjectionFaultInjector {
                    EditorProjectionFaultPoint point =
                        EditorProjectionFaultPoint::Prepare;
                    std::string adapter;
                    bool armed = false;
                    bool fired = false;
                    bool shouldFail(EditorProjectionFaultPoint observed,
                                    std::string_view name) noexcept override {
                        if (!armed || fired || observed != point ||
                            name != adapter) {
                            return false;
                        }
                        fired = true;
                        armed = false;
                        return true;
                    }
                } fault;
                fault.point = point;
                fault.adapter =
                    point == EditorProjectionFaultPoint::AfterPublication
                        ? "scene_pair"
                        : "behavior.scene.main";

                EditorEditCoordinator edits{EditorEditRuntimeDependencies{
                    .document = [&]() -> const AuthoringSceneDocument & {
                        return target.projectionDocument();
                    },
                    .current_scene_id = [] { return std::string{"main"}; },
                    .execute = [&](const EditorEditExecutionRequest &request) {
                        EcsCodecProjectionAdapter ecs_adapter{
                            ecs, "main", bindings};
                        BehaviorSceneProjectionAdapter behavior_adapter{
                            arena, "main", bindings, {}};
                        std::array<EditorProjectionAdapter *, 2> adapters{
                            &ecs_adapter, &behavior_adapter};
                        EditorProjectionTransaction transaction{
                            target, request.base_revision, &fault};
                        return transaction.commit(request.commands, adapters);
                    },
                    .gate = [] { return EditorGateObservation{}; },
                    .prefab_registry = [&] {
                        return target.projectionResolvedScene()
                            .prefabRegistry();
                    },
                    .bindable_provider = [&] {
                        return target.projectionResolvedScene()
                            .bindableProvider();
                    },
                    .closure_generation = [&] {
                        return target.projectionResolvedScene()
                            .prefabClosure().generation;
                    },
                }};
                const auto actor =
                    edits.openSession({{"display_name", "fault"}})
                        .at("actor_id").get<std::uint64_t>();
                const auto cas = [&] {
                    const auto &resolved = target.projectionResolvedScene();
                    return Json{
                        {"actor_id", actor},
                        {"scene_revision",
                         target.projectionDocument().revision().value},
                        {"closure_generation",
                         resolved.prefabClosure().generation},
                        {"provider_generation",
                         resolved.bindableProvider().generation()},
                    };
                };
                const auto commit = [&](Json request,
                                        bool revert = false,
                                        bool redo = false) {
                    const auto accepted =
                        revert ? (redo ? edits.enqueueRedo(request)
                                       : edits.enqueueUndo(request))
                               : edits.enqueue(request);
                    REQUIRE(accepted.at("status") == "accepted");
                    edits.commitPending();
                    return edits.getResult(
                        {{"ticket", accepted.at("ticket")}});
                };
                const auto set_mode = [&] {
                    auto request = cas();
                    request["operations"] = Json::array({
                        {{"op", "set_prefab_parameter"},
                         {"object", authoring_object_id.value},
                         {"parameter", "mode"}, {"value", "full"}}
                    });
                    return commit(std::move(request));
                };
                const auto undo = [&] {
                    return commit(cas(), true, false);
                };

                if (action != Action::Set) {
                    REQUIRE(set_mode().at("status") == "committed");
                }
                if (action == Action::Redo) {
                    REQUIRE(undo().at("status") == "committed");
                }

                const auto snapshot = [&] {
                    Json result{
                        {"authoring", AuthoringSceneAuthority::encodeSemantic(
                                          target.projectionDocument())},
                        {"revision",
                         target.projectionDocument().revision().value},
                        {"resolver_generation",
                         target.projectionResolvedScene()
                             .resolverGeneration().value},
                    };
                    Json projection = Json::array();
                    for (const auto &scene_view :
                         target.projectionResolvedScene().scenes()) {
                        for (const auto &resolved_object : scene_view.objects) {
                            for (const auto &component :
                                 resolved_object.components) {
                                Json origin;
                                if (const auto *generated =
                                        component.origin.generated()) {
                                    origin = {{"generated",
                                               generated->stable_generated_id},
                                              {"prefab", generated->prefab},
                                              {"key",
                                               generated->component_key}};
                                } else {
                                    origin = {{"authored",
                                               component.origin.authored()
                                                   ->authoring_component_index}};
                                }
                                projection.push_back(
                                    {{"scene", scene_view.scene_id},
                                     {"object",
                                      resolved_object.authoring_object_id.value},
                                     {"index",
                                      component.merged_component_index},
                                     {"origin", std::move(origin)},
                                     {"effective", component.effective_json}});
                            }
                        }
                    }
                    result["projection"] = std::move(projection);
                    const auto &resolved_state =
                        target.projectionResolvedScene();
                    const auto &closure = resolved_state.prefabClosure();
                    result["registry_generation"] =
                        resolved_state.prefabRegistry().generation();
                    result["provider_snapshot_generation"] =
                        resolved_state.bindableProvider().generation();
                    result["provider_snapshot_fingerprint"] =
                        std::string{
                            resolved_state.bindableProvider().fingerprint()};
                    result["closure"] = {
                        {"generation", closure.generation},
                        {"registry_generation",
                         closure.registry_generation},
                        {"provider_generation",
                         closure.provider_generation},
                        {"provider_fingerprint",
                         closure.provider_fingerprint_sha256},
                        {"resolver_fingerprint",
                         closure.resolver_inputs_fingerprint_sha256},
                    };
                    for (const auto &entry : closure.scenes) {
                        result["closure"]["scenes"].push_back(
                            {{"id", entry.scene_id},
                             {"digest", entry.digest_sha256}});
                    }
                    for (const auto &entry : closure.prefabs) {
                        result["closure"]["prefabs"].push_back(
                            {{"name", entry.name},
                             {"digest", entry.digest_sha256}});
                    }
                    for (const auto &entry : closure.behaviors) {
                        result["closure"]["behaviors"].push_back(
                            {{"name", entry.stable_name},
                             {"version", entry.schema_version},
                             {"fingerprint",
                              entry.params_schema_fingerprint}});
                    }
                    const auto *runtime_sprite =
                        ecs.tryComponent<SpriteViewComponent>(entity);
                    REQUIRE(runtime_sprite != nullptr);
                    result["ecs"] = requireComponentCodec("sprite_view")
                        .encodeCanonical(requireComponentCodec("sprite_view")
                                             .projectRuntime(runtime_sprite));
                    Json arena_json = Json::array();
                    for (const auto &attachment : arena.snapshot()) {
                        Json origin;
                        if (const auto *generated =
                                attachment.component_origin.generated()) {
                            origin = {{"generated",
                                       generated->stable_generated_id}};
                        } else {
                            origin = {{"authored",
                                       attachment.component_origin.authored()
                                           ->authoring_component_index}};
                        }
                        arena_json.push_back(
                            {{"handle", attachment.handle.value},
                             {"seq", attachment.attachment_seq},
                             {"entity_index", attachment.entity.index},
                             {"entity_generation",
                              attachment.entity.generation},
                             {"component_index",
                              attachment.component_index},
                             {"scene_object_index",
                              attachment.scene_object_index},
                             {"origin", std::move(origin)},
                             {"name", attachment.stable_name},
                             {"params", attachment.canonical_params},
                             {"owner", attachment.owner},
                             {"pending", attachment.pending},
                             {"active", attachment.active},
                             {"activation_failed",
                              attachment.activation_failed},
                             {"activation_error",
                              attachment.activation_error}});
                    }
                    result["arena"] = std::move(arena_json);
                    const auto isolation = arena.isolationState();
                    result["arena_isolation"] = {
                        {"next_handle", isolation.next_handle},
                        {"next_attachment_seq",
                         isolation.next_attachment_seq},
                        {"deferred", isolation.deferred_mutation_count},
                        {"callback_depth", isolation.callback_depth},
                        {"callback_owner", isolation.callback_owner},
                    };
                    return result;
                };

                const auto before = snapshot();
                fault.armed = true;
                OJson failed;
                if (action == Action::Set) {
                    auto request = cas();
                    request["operations"] = Json::array({
                        {{"op", "set_prefab_parameter"},
                         {"object", authoring_object_id.value},
                         {"parameter", "mode"},
                         {"value", "y_axis"}}
                    });
                    failed = commit(std::move(request));
                } else if (action == Action::Unset) {
                    auto request = cas();
                    request["operations"] = Json::array({
                        {{"op", "unset_prefab_parameter"},
                         {"object", authoring_object_id.value},
                         {"parameter", "mode"}}
                    });
                    failed = commit(std::move(request));
                } else if (action == Action::Undo) {
                    failed = commit(cas(), true, false);
                } else {
                    failed = commit(cas(), true, true);
                }
                REQUIRE(fault.fired);
                REQUIRE(failed.at("status") == "failed");
                REQUIRE(snapshot() == before);

                arena.deactivateAll();
                REQUIRE(GameObjects::remove(entity));
            }
        }
    }
}

} // namespace Pelican
