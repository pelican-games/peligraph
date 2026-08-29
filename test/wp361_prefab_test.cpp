#include "../src/core/build_features.hpp"
#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/communication/editorjournal.hpp"
#include "../src/core/communication/editorrpchandlers.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/imgui/inspector.hpp"
#include "../src/core/loader/authoringsceneauthority.hpp"
#include "../src/core/loader/editorprojectiontransaction.hpp"
#include "../src/core/loader/prefabprovider.hpp"
#include "../src/core/loader/resolvedscene.hpp"
#include "../src/core/userpublic/behavior.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#include "../src/project/prefab.hpp"
#include "../src/project/projectformat.hpp"
#include "../src/project/sceneformat.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <picosha2.h>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

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

class Wp361Behavior final : public Behavior {
  public:
    using Params = Wp361Params;
    void onInit(BehaviorContext &context) override {
        const auto &params = context.params<Params>();
        wp361_callback_trace.push_back(std::to_string(params.hp) + ":" +
                                       params.target);
    }
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

Json tagged(std::string name) { return Json{{"$param", std::move(name)}}; }

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
    REQUIRE(object.prefab_instance->closure_generation == 3);
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
    auto mutated = reordered;
    const auto found = std::find_if(mutated.begin(), mutated.end(),
                                    [](const auto &descriptor) {
                                        return descriptor.provider_name == "collider" &&
                                               descriptor.json_pointer == "/radius";
                                    });
    found->canonical_default = 0.75;
    REQUIRE(std::string{BindableProviderSnapshot{4, std::move(mutated)}
                            .fingerprint()} !=
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
TEST_CASE("WP361 real RpcServer exposes generated wire and rejects mutation and persistence",
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
    const auto before_bytes = AuthoringSceneAuthority::encodeSemantic(fixture.document);
    const auto before_revision = fixture.document.revision();
    const auto &object = instanceObject(fixture.resolved);
    const auto gid = generatedByKey(object, "sprite")
                         .origin.generated()->stable_generated_id;

    EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&]() -> const AuthoringSceneDocument & { return fixture.document; },
        .resolved_scene = [&]() -> const ResolvedScene & { return fixture.resolved; },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
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
    REQUIRE(parsed[1].at("error").at("data").at("code") ==
            "prefab_generated_read_only");
    for (std::size_t index = 2; index <= 6; ++index) {
        REQUIRE(parsed[index].at("error").at("data").at("code") ==
                "prefab_persistence_unsupported");
    }
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
    REQUIRE(parsed[11].at("error").at("code") ==
            JsonRpcErrorCodes::methodNotFound);
    REQUIRE(parsed[12].at("error").at("code") ==
            JsonRpcErrorCodes::methodNotFound);
    REQUIRE(AuthoringSceneAuthority::encodeSemantic(fixture.document) == before_bytes);
    REQUIRE(fixture.document.revision() == before_revision);
    REQUIRE(generatedByKey(instanceObject(fixture.resolved), "sprite")
                .origin.generated()->stable_generated_id == gid);
    const auto after_arena_snapshot = arena.snapshot();
    REQUIRE(arenaIdentity(after_arena_snapshot) == before_arena_identity);
    REQUIRE(after_arena_snapshot.size() == before_arena_snapshot.size());
    for (const auto &attachment : after_arena_snapshot) {
        REQUIRE(GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(
            attachment.entity));
    }
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

TEST_CASE("WP361 v4 JSON examples remain parse gates while WP362 routing stays unsupported",
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

} // namespace Pelican
