#include "editorjournal.hpp"
#include "editorgate.hpp"
#include "editorjournalintegrity.hpp"
#include "editorjournalinternal.hpp"
#include "editorjournalops.hpp"

#include "../loader/authoringsceneauthority.hpp"

#include "../appflow/framephase.hpp"
#include "../ecs/componentinfo.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../loader/componentcodec.hpp"
#include "../schemanodeadapter.hpp"
#include "../userpublic/details/behavior/registerer.hpp"
#include "../../project/prefab.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <exception>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace EditorJournalInternal {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::uint64_t maxExactJsonInteger = UINT64_C(9007199254740991);

EditFailure::EditFailure(EditorEditErrorCode error_code,
                         OrderedJson error_payload, std::string message,
                         std::string wire_code_name)
    : std::runtime_error(std::move(message)), code{error_code},
      payload(std::move(error_payload)),
      code_name(std::move(wire_code_name)) {}

[[noreturn]] void editFailure(EditorEditErrorCode code, OrderedJson payload,
                              std::string message) {
    throw EditFailure{code, std::move(payload), std::move(message)};
}

const Json &requireObject(const Json &value, std::string_view context) {
    if (!value.is_object()) {
        throw std::invalid_argument(std::string{context} + " must be an object");
    }
    return value;
}

void requireOnly(const Json &value,
                 std::initializer_list<std::string_view> fields,
                 std::string_view context) {
    requireObject(value, context);
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            throw std::invalid_argument(std::string{context} +
                                        " unknown field: " + it.key());
        }
    }
}

std::uint64_t exactUnsigned(const Json &value, std::string_view context,
                            bool nonzero) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            throw std::invalid_argument(std::string{context} +
                                        " must be non-negative");
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        throw std::invalid_argument(std::string{context} +
                                    " must be an integer token");
    }
    if (result > maxExactJsonInteger) {
        throw std::invalid_argument(std::string{context} +
                                    " exceeds 2^53-1");
    }
    if (nonzero && result == 0) {
        throw std::invalid_argument(std::string{context} +
                                    " must be non-zero");
    }
    return result;
}

std::string requireString(const Json &value, std::string_view field,
                          std::string_view context) {
    const auto found = value.find(std::string{field});
    if (found == value.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        throw std::invalid_argument(std::string{context} + " requires non-empty " +
                                    std::string{field});
    }
    return found->get<std::string>();
}

std::optional<std::string> optionalString(const Json &value,
                                          std::string_view field,
                                          std::string_view context) {
    const auto found = value.find(std::string{field});
    if (found == value.end()) return std::nullopt;
    if (!found->is_string() || found->get_ref<const std::string &>().empty()) {
        throw std::invalid_argument(std::string{context} + " " +
                                    std::string{field} +
                                    " must be a non-empty string");
    }
    return found->get<std::string>();
}

std::vector<ObjectLocation> objectLocations(const AuthoringSceneDocument &document) {
    std::vector<ObjectLocation> result;
    const auto scenes = AuthoringSceneAuthority::query(document);
    for (std::size_t scene_index = 0; scene_index < scenes.size(); ++scene_index) {
        const auto &scene = scenes[scene_index];
        for (const auto &object : scene.objects) {
            result.push_back(ObjectLocation{
                .scene_id = scene.scene_id,
                .scene_index = scene_index,
                .object_index = object.declaration_index,
                .object_id = object.authoring_object_id,
                .name = object.name,
                .parent = object.parent,
                .authored = object.authoredJson(),
            });
        }
    }
    return result;
}

ObjectLocation requireObjectLocation(const AuthoringSceneDocument &document,
                                     AuthoringObjectId object_id,
                                     EditorEditErrorCode error) {
    const auto objects = objectLocations(document);
    const auto found = std::find_if(objects.begin(), objects.end(),
                                    [&](const auto &object) {
                                        return object.object_id == object_id;
                                    });
    if (found == objects.end()) {
        editFailure(error,
                    {{"object", object_id.value}, {"object_path", "/authoring_objects/" +
                                                                 std::to_string(object_id.value)}},
                    "authoring object does not exist");
    }
    return *found;
}

std::optional<AuthoringObjectId>
objectIdByName(const AuthoringSceneDocument &document, std::string_view scene_id,
               std::string_view name) {
    for (const auto &object : objectLocations(document)) {
        if (object.scene_id == scene_id && object.name && *object.name == name) {
            return object.object_id;
        }
    }
    return std::nullopt;
}

std::pair<std::size_t, Json> requireComponent(const ObjectLocation &object,
                                              std::string_view component_name) {
    const auto &components = object.authored.at("components");
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (components.at(index).value("name", std::string{}) == component_name) {
            return {index, components.at(index)};
        }
    }
    editFailure(EditorEditErrorCode::missing_component,
                {{"object", object.object_id.value},
                 {"slot", component_name}},
                "component does not exist");
}

std::pair<std::size_t, Json> requireComponentAt(
    const ObjectLocation &object, std::size_t component_index,
    std::string_view component_name) {
    const auto &components = object.authored.at("components");
    if (component_index >= components.size() ||
        components.at(component_index).value("name", std::string{}) !=
            component_name) {
        editFailure(EditorEditErrorCode::missing_component,
                    {{"object", object.object_id.value},
                     {"slot", component_name},
                     {"attachment_index", component_index}},
                    "component does not exist at the requested attachment index");
    }
    return {component_index, components.at(component_index)};
}

bool hasComponent(const ObjectLocation &object, std::string_view component_name) {
    const auto &components = object.authored.at("components");
    return std::any_of(components.begin(), components.end(),
                       [&](const auto &component) {
                           return component.value("name", std::string{}) ==
                                  component_name;
                       });
}

Json canonicalComponent(const Json &component, AuthoringObjectId object_id,
                        std::string_view component_name,
                        const std::function<bool(std::string_view)> &
                            component_exists) {
    const auto *codec = findComponentCodec(component_name);
    if (codec == nullptr) {
        if (component_exists && component_exists(component_name)) {
            editFailure(EditorEditErrorCode::not_editable,
                        {{"object", object_id.value},
                         {"slot", component_name}},
                        "component has no editable codec");
        }
        const ComponentUnknownError cause{std::string{component_name}};
        editFailure(EditorEditErrorCode::component_unknown, cause.payload(),
                    cause.what());
    }
    try {
        auto decoded = codec->decodeAuthored(component);
        if (!decoded) {
            const auto &violation = decoded.error();
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", resolutionJsonPointer(violation.tokens)},
                         {"detail", violation.leaf_message}},
                        violation.leaf_message);
        }
        return Json::parse(
            codec->encodeCanonical(std::move(decoded).value()).dump());
    } catch (const EditFailure &) {
        throw;
    } catch (const LegacyRejectException &error) {
        const auto failure = StoredResolutionFailure{LegacyResolutionText{
            .legacy_branch_id = error.legacyBranchId(),
            .message = error.what(),
        }};
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", component_name},
                     {"field_path", "/"},
                     {"detail", error.what()},
                     {"resolution_error", resolutionFailureJson(failure)}},
                    error.what());
    } catch (const std::exception &error) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", component_name},
                     {"field_path", "/"},
                     {"detail", error.what()}},
                    error.what());
    }
}

Json canonicalBehaviorComponent(const Json &component,
                                AuthoringObjectId object_id) {
    requireObject(component, "behavior component");
    for (auto it = component.begin(); it != component.end(); ++it) {
        if (it.key() != "name" && it.key() != "type" && it.key() != "params") {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", "behavior"},
                         {"field_path", "/" + it.key()},
                         {"detail", "unknown behavior attachment field"}},
                        "unknown behavior attachment field");
        }
    }
    if (component.value("name", std::string{}) != "behavior") {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", "behavior"},
                     {"field_path", "/name"},
                     {"detail", "behavior attachment name must be behavior"}},
                    "behavior attachment name must be behavior");
    }
    const auto type = requireString(component, "type", "behavior component");
    const auto *registration = internal::getBehaviorRegisterer().findByName(type);
    const auto params = component.contains("params")
                            ? component.at("params")
                            : Json::object();
    // Registration availability is a runtime projection input.  Keep an
    // unregistered attachment lossless here so the production context can
    // distinguish an active registry (named rejection) from an unavailable
    // DLL (pending attachment) at the commit boundary.
    if (registration == nullptr) {
        return Json{{"name", "behavior"},
                    {"type", type},
                    {"params", params}};
    }
    try {
        return Json{{"name", "behavior"},
                    {"type", type},
                    {"params", Json::parse(
                                   registration->canonicalize_params(params))}};
    } catch (const StructFieldValidationError &error) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", "behavior"},
                     {"field_path", std::string{error.path()}},
                     {"detail", error.what()}},
                    error.what());
    } catch (const std::exception &error) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", "behavior"},
                     {"field_path", "/params"},
                     {"detail", error.what()}},
                    error.what());
    }
}

EditorProjectionCommand makeReplaceComponentCommand(
    const ObjectLocation &object, std::size_t component_index,
    std::string component_name, Json component) {
    return EditorProjectionCommand{
        .object_path = "/authoring_objects/" +
                       std::to_string(object.object_id.value) + "/components/" +
                       component_name,
        .apply = [scene_id = object.scene_id, object_index = object.object_index,
                  component_index, component = std::move(component)](Json &document) {
            auto &objects = document.at("scenes").at(scene_id).at("objects");
            if (object_index >= objects.size() ||
                component_index >= objects.at(object_index).at("components").size()) {
                throw std::runtime_error("stable component location disappeared");
            }
            objects.at(object_index).at("components").at(component_index) = component;
        },
    };
}

EditorProjectionCommand makeInsertComponentCommand(
    const ObjectLocation &object, std::size_t component_index, Json component) {
    return EditorProjectionCommand{
        .object_path = "/authoring_objects/" +
                       std::to_string(object.object_id.value) + "/components",
        .apply = [scene_id = object.scene_id, object_index = object.object_index,
                  component_index, component = std::move(component)](Json &document) {
            auto &components = document.at("scenes").at(scene_id).at("objects")
                                   .at(object_index).at("components");
            if (component_index > components.size()) {
                throw std::runtime_error("component declaration interval disappeared");
            }
            components.insert(components.begin() +
                                  static_cast<Json::difference_type>(component_index),
                              component);
        },
    };
}

EditorProjectionCommand makeEraseComponentCommand(
    const ObjectLocation &object, std::size_t component_index,
    std::string component_name) {
    return EditorProjectionCommand{
        .object_path = "/authoring_objects/" +
                       std::to_string(object.object_id.value) + "/components/" +
                       component_name,
        .apply = [scene_id = object.scene_id, object_index = object.object_index,
                  component_index, component_name = std::move(component_name)](
                     Json &document) {
            auto &components = document.at("scenes").at(scene_id).at("objects")
                                   .at(object_index).at("components");
            if (component_index >= components.size() ||
                components.at(component_index).value("name", std::string{}) !=
                    component_name) {
                throw std::runtime_error("stable component slot disappeared");
            }
            components.erase(components.begin() +
                             static_cast<Json::difference_type>(component_index));
        },
    };
}

OrderedJson closureJson(const AuthoringObjectClosure &closure) {
    OrderedJson result{
        {"authoring_object_id", closure.authoring_object_id.value},
        {"scene_id", closure.scene_id},
        {"declaration_index", closure.declaration_index},
        {"authored_object", closure.authored_json},
    };
    result["previous_object_id"] = closure.previous_object_id
                                        ? OrderedJson(closure.previous_object_id->value)
                                        : OrderedJson(nullptr);
    result["next_object_id"] = closure.next_object_id
                                    ? OrderedJson(closure.next_object_id->value)
                                    : OrderedJson(nullptr);
    return result;
}

AuthoringObjectClosure closureFromJson(const Json &value) {
    requireObject(value, "object closure");
    AuthoringObjectClosure result;
    result.authoring_object_id = AuthoringObjectId{
        exactUnsigned(value.at("authoring_object_id"),
                      "closure authoring_object_id", true)};
    result.scene_id = requireString(value, "scene_id", "object closure");
    result.declaration_index = static_cast<std::size_t>(exactUnsigned(
        value.at("declaration_index"), "closure declaration_index"));
    result.authored_json = value.at("authored_object");
    if (const auto previous = value.find("previous_object_id");
        previous != value.end() && !previous->is_null()) {
        result.previous_object_id = AuthoringObjectId{
            exactUnsigned(*previous, "closure previous_object_id", true)};
    }
    if (const auto next = value.find("next_object_id");
        next != value.end() && !next->is_null()) {
        result.next_object_id = AuthoringObjectId{
            exactUnsigned(*next, "closure next_object_id", true)};
    }
    return result;
}

AuthoringObjectClosure closureForObject(const AuthoringSceneDocument &document,
                                        AuthoringObjectId object_id) {
    const auto location = requireObjectLocation(document, object_id);
    std::vector<ObjectLocation> scene_objects;
    for (const auto &object : objectLocations(document)) {
        if (object.scene_id == location.scene_id) scene_objects.push_back(object);
    }
    std::sort(scene_objects.begin(), scene_objects.end(), [](const auto &left,
                                                             const auto &right) {
        return left.object_index < right.object_index;
    });
    const auto found = std::find_if(scene_objects.begin(), scene_objects.end(),
                                    [&](const auto &object) {
                                        return object.object_id == object_id;
                                    });
    const auto index = static_cast<std::size_t>(found - scene_objects.begin());
    return AuthoringObjectClosure{
        .authoring_object_id = object_id,
        .scene_id = location.scene_id,
        .declaration_index = location.object_index,
        .previous_object_id = index > 0
                                  ? std::optional{scene_objects[index - 1].object_id}
                                  : std::nullopt,
        .next_object_id = index + 1 < scene_objects.size()
                              ? std::optional{scene_objects[index + 1].object_id}
                              : std::nullopt,
        .authored_json = location.authored,
    };
}

std::vector<std::string> gateReasonNames(std::uint32_t bits) {
    std::vector<std::string> result;
    const std::array reasons{
        std::pair{EditorGateReason::replay, "replay"},
        std::pair{EditorGateReason::golden, "golden"},
        std::pair{EditorGateReason::strict, "strict"},
        std::pair{EditorGateReason::reload_scene_transition,
                  "reload_scene_transition"},
        std::pair{EditorGateReason::preview_lease_conflict,
                  "preview_lease_conflict"},
    };
    for (const auto &[reason, name] : reasons) {
        if ((bits & editorGateReasonBit(reason)) != 0) result.emplace_back(name);
    }
    return result;
}

std::string primaryGateReason(const EditorGateSnapshot &snapshot) {
    return snapshot.reasons.empty() ? "gate_epoch_changed"
                                    : snapshot.reasons.front();
}

class LocalDocumentTarget final : public EditorProjectionDocumentTarget {
    SceneProjectionState state_;

  public:
    explicit LocalDocumentTarget(const SceneProjectionState &source)
        : state_{source} {}

    explicit LocalDocumentTarget(
        const AuthoringSceneDocument &source,
        PrefabRegistrySnapshot registry = {},
        BindableProviderSnapshot provider = {})
        : state_{ResolvedSceneResolver::prepare(
              AuthoringSceneAuthority::stage(
                  source,
                  AuthoringSceneAuthority::rawView(source).documentJson(),
                  SceneRevision{source.revision().value + 1}),
              SceneResolverGeneration{1}, {}, std::move(registry),
              std::move(provider))} {}

    const SceneProjectionState &projectionState() const override {
        return state_;
    }
    SceneRevision nextProjectionRevision() const override {
        if (state_.revision().value ==
            std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("SceneRevision space exhausted");
        }
        return SceneRevision{state_.revision().value + 1};
    }
    SceneResolverGeneration nextProjectionResolverGeneration() const override {
        return SceneResolverGeneration{
            state_.resolverGeneration().value + 1};
    }
    void publishProjectionState(
        SceneProjectionState &candidate) noexcept override {
        state_.swap(candidate);
    }
};

struct PreparedBatch {
    std::vector<EditorProjectionCommand> commands;
    std::vector<OrderedJson> forward;
    std::vector<OrderedJson> inverse;
    std::vector<EditorCanonicalOperation> canonical_forward;
    std::vector<PreparedOperation> operations;
    std::vector<AuthoringObjectId> affected;
};

std::string stableComponentTarget(AuthoringObjectId object_id,
                                  std::string_view component,
                                  std::string_view field_path) {
    auto result = "/authoring_objects/" + std::to_string(object_id.value) +
                  "/components/" + std::string{component};
    if (!field_path.empty() && field_path != "/") result += std::string{field_path};
    return result;
}

std::string stableBehaviorTarget(AuthoringObjectId object_id,
                                 std::size_t attachment_index,
                                 std::uint64_t attachment_handle,
                                 std::string_view field_path) {
    (void)attachment_handle;
    auto result = "/authoring_objects/" + std::to_string(object_id.value) +
                  "/behaviors/occurrences/" +
                  std::to_string(attachment_index);
    if (!field_path.empty() && field_path != "/") result += std::string{field_path};
    return result;
}

OrderedJson behaviorOriginIdentityJson(
    const EditorBehaviorAttachmentIdentity::Origin &origin) {
    if (!origin.generated) {
        return OrderedJson{
            {"kind", "authored"},
            {"authoring_component_index", origin.authoring_component_index},
        };
    }
    return OrderedJson{
        {"kind", "generated"},
        {"stable_generated_id", origin.stable_generated_id},
        {"prefab", origin.prefab},
        {"instance_id", origin.instance_id},
        {"component_key", origin.component_key},
        {"digest_sha256", origin.digest_sha256},
    };
}

EditorBehaviorAttachmentIdentity::Origin authoredBehaviorOrigin(
    std::size_t component_index) {
    return EditorBehaviorAttachmentIdentity::Origin{
        .generated = false,
        .authoring_component_index = component_index,
    };
}

OrderedJson behaviorAttachmentWitness(
    AuthoringObjectId object_id, std::size_t component_index,
    std::string_view stable_type,
    const EditorBehaviorAttachmentIdentity::Origin &origin) {
    return OrderedJson{
        {"authoring_object_id", object_id.value},
        {"authored_component_occurrence", component_index},
        {"type", stable_type},
        {"origin", behaviorOriginIdentityJson(origin)},
    };
}

OrderedJson authoredBehaviorAttachmentWitness(
    AuthoringObjectId object_id, std::size_t component_index,
    const Json &component) {
    return behaviorAttachmentWitness(
        object_id, component_index,
        requireString(component, "type", "behavior attachment identity"),
        authoredBehaviorOrigin(component_index));
}

OrderedJson runtimeBehaviorAttachmentWitness(
    const EditorBehaviorAttachmentIdentity &identity) {
    return behaviorAttachmentWitness(
        identity.authoring_object_id,
        identity.authored_component_occurrence,
        identity.stable_type, identity.origin);
}

bool sameBehaviorAttachmentWitness(const OrderedJson &left,
                                   const OrderedJson &right) {
    // Journal operations use the regular JSON object container while records
    // use ordered_json. Object member order is not part of the identity.
    return Json(left) == Json(right);
}

std::optional<OrderedJson> currentAuthoredBehaviorAttachmentWitness(
    const AuthoringSceneDocument &document, AuthoringObjectId object_id,
    std::size_t component_index) {
    for (const auto &scene : AuthoringSceneAuthority::query(document)) {
        const auto object = std::find_if(
            scene.objects.begin(), scene.objects.end(),
            [&](const AuthoringObjectView &candidate) {
                return candidate.authoring_object_id == object_id;
            });
        if (object == scene.objects.end() ||
            component_index >= object->components.size()) {
            continue;
        }
        const auto &component = object->components[component_index].authoredJson();
        const auto name = component.find("name");
        const auto type = component.find("type");
        if (name == component.end() || !name->is_string() ||
            name->get_ref<const std::string &>() != "behavior" ||
            type == component.end() || !type->is_string() ||
            type->get_ref<const std::string &>().empty()) {
            return std::nullopt;
        }
        return behaviorAttachmentWitness(
            object_id, component_index, type->get_ref<const std::string &>(),
            authoredBehaviorOrigin(component_index));
    }
    return std::nullopt;
}

[[noreturn]] void behaviorAttachmentIntegrityFailure(
    std::string_view reason, AuthoringObjectId object_id,
    std::size_t component_index, const OrderedJson &expected = nullptr,
    const OrderedJson &current = nullptr) {
    OrderedJson payload{{"reason", reason},
                        {"object", object_id.value},
                        {"component_index", component_index}};
    if (!expected.is_null()) payload["expected_identity"] = expected;
    if (!current.is_null()) payload["runtime_identity"] = current;
    editFailure(EditorEditErrorCode::journal_integrity_error,
                std::move(payload),
                "journal behavior attachment identity is unavailable or no longer matches");
}

void copyBehaviorAttachmentWitness(const Json &raw, OrderedJson &forward,
                                   OrderedJson &inverse) {
    if (const auto found = raw.find("attachment_identity");
        found != raw.end()) {
        forward["attachment_identity"] = *found;
        inverse["attachment_identity"] = *found;
    }
}

std::uint64_t internalIdentityValue(const Json &operation,
                                    std::string_view field,
                                    std::string_view context,
                                    bool nonzero) {
    const auto found = operation.find(std::string{field});
    return found == operation.end() ? 0 : exactUnsigned(*found, context, nonzero);
}

std::vector<std::string> pointerTokens(std::string_view pointer,
                                       std::string_view context,
                                       bool allow_empty) {
    if (pointer.empty()) {
        if (allow_empty) return {};
        throw std::invalid_argument(std::string{context} +
                                    " must be a non-empty JSON pointer");
    }
    if (pointer.front() != '/') {
        throw std::invalid_argument(std::string{context} +
                                    " must be an RFC6901 JSON pointer");
    }
    std::vector<std::string> result;
    std::size_t begin = 1;
    while (begin <= pointer.size()) {
        const auto end = pointer.find('/', begin);
        const auto encoded = pointer.substr(
            begin, end == std::string_view::npos ? pointer.size() - begin
                                                  : end - begin);
        std::string token;
        token.reserve(encoded.size());
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            if (encoded[index] != '~') {
                token.push_back(encoded[index]);
                continue;
            }
            if (index + 1 >= encoded.size() ||
                (encoded[index + 1] != '0' && encoded[index + 1] != '1')) {
                throw std::invalid_argument(std::string{context} +
                                            " has an invalid JSON pointer escape");
            }
            token.push_back(encoded[++index] == '0' ? '~' : '/');
        }
        if (token.empty()) {
            throw std::invalid_argument(std::string{context} +
                                        " contains an empty path segment");
        }
        result.push_back(std::move(token));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return result;
}

bool decimalIndex(std::string_view token) noexcept {
    return !token.empty() &&
           std::all_of(token.begin(), token.end(), [](unsigned char value) {
               return value >= '0' && value <= '9';
           });
}

std::uint32_t exactU32(const Json &value, std::string_view context,
                       bool nonzero) {
    const auto raw = exactUnsigned(value, context, nonzero);
    if (raw > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string{context} + " exceeds u32");
    }
    return static_cast<std::uint32_t>(raw);
}

AddressedComponent requireComponentAddress(
    const Json &raw, const AuthoringSceneDocument &document,
    std::string_view context, bool row_optional) {
    const auto found = raw.find("address");
    if (found == raw.end()) {
        throw std::invalid_argument(std::string{context} + " requires address");
    }
    requireOnly(*found,
                {"authoring_object_id", "component", "list_path", "row_id",
                 "leaf_path"},
                std::string{context} + " address");
    const auto object_id = AuthoringObjectId{exactUnsigned(
        found->at("authoring_object_id"),
        std::string{context} + " address authoring_object_id", true)};
    const auto object = requireObjectLocation(document, object_id);
    const auto &identity = requireObject(found->at("component"),
                                         std::string{context} + " component");
    requireOnly(identity, {"type", "occurrence", "origin"},
                std::string{context} + " component");
    const auto stable_type = requireString(identity, "type", context);
    const auto component_index = static_cast<std::size_t>(exactUnsigned(
        identity.at("occurrence"), std::string{context} + " occurrence"));
    const auto &components = object.authored.at("components");
    if (component_index >= components.size()) {
        editFailure(EditorEditErrorCode::journal_integrity_error,
                    {{"reason", "attachment_occurrence"},
                     {"object", object_id.value},
                     {"occurrence", component_index}},
                    "component attachment occurrence no longer exists");
    }
    const auto component = components.at(component_index);
    const auto component_name = component.value("name", std::string{});
    const auto expected_type = component_name == "behavior"
                                   ? component.value("type", std::string{})
                                   : component_name;
    if (stable_type != expected_type) {
        editFailure(EditorEditErrorCode::journal_integrity_error,
                    {{"reason", "attachment_type"},
                     {"object", object_id.value},
                     {"expected", expected_type},
                     {"actual", stable_type}},
                    "component attachment type witness does not match");
    }
    const OrderedJson expected_origin{
        {"kind", "authored"},
        {"authoring_component_index", component_index},
    };
    if (!identity.at("origin").is_object() ||
        Json(identity.at("origin")) != Json(expected_origin)) {
        editFailure(EditorEditErrorCode::journal_integrity_error,
                    {{"reason", "attachment_origin"},
                     {"object", object_id.value},
                     {"expected", expected_origin},
                     {"actual", identity.at("origin")}},
                    "component attachment origin witness does not match");
    }

    AddressedComponent result{
        .object = object,
        .component_index = component_index,
        .component_name = component_name,
        .component = component,
        .attachment_identity = behaviorAttachmentWitness(
            object_id, component_index, expected_type,
            authoredBehaviorOrigin(component_index)),
        .list_path = requireString(*found, "list_path", context),
    };
    result.list_tokens = pointerTokens(result.list_path,
                                       std::string{context} + " list_path");
    if (component_name == "behavior" &&
        (result.list_tokens.empty() || result.list_tokens.front() != "params")) {
        result.list_tokens.insert(result.list_tokens.begin(), "params");
        result.list_path = "/params" + result.list_path;
    }
    if (const auto row = found->find("row_id"); row != found->end()) {
        result.row_id = exactU32(*row, std::string{context} + " row_id", true);
    } else if (!row_optional) {
        throw std::invalid_argument(std::string{context} +
                                    " address requires row_id");
    }
    if (const auto leaf = found->find("leaf_path"); leaf != found->end()) {
        if (!leaf->is_string()) {
            throw std::invalid_argument(std::string{context} +
                                        " leaf_path must be a string");
        }
        result.leaf_path = leaf->get<std::string>();
        result.leaf_tokens = pointerTokens(
            result.leaf_path, std::string{context} + " leaf_path", true);
    }
    return result;
}

// WP382a-fix moved the recursive descriptor to a fixed-capacity constexpr form
// and made `Schema::Node` the single owning host model.  Row addressing walks
// the owning model, so the descriptor is converted once per lookup here.
std::optional<Schema::Node> componentSchema(const AddressedComponent &address) {
    if (address.component_name == "behavior") {
        const auto *registration = internal::getBehaviorRegisterer().findByName(
            address.component.value("type", std::string{}));
        if (registration == nullptr) return std::nullopt;
        const auto *node =
            internal::getBehaviorRegisterer().schemaFor(*registration);
        if (node == nullptr) return std::nullopt;
        return *node;
    }
    const auto *codec = findComponentCodec(address.component_name);
    if (codec == nullptr) return std::nullopt;
    return internal::hostSchemaNode(codec->schemaNode());
}

Schema::Node requireListSchema(const AddressedComponent &address) {
    const auto root = componentSchema(address);
    const Schema::Node *node = root ? &*root : nullptr;
    if (node == nullptr) {
        editFailure(EditorEditErrorCode::not_editable,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name}},
                    "component has no recursive editable schema");
    }
    std::size_t token_index = 0;
    if (address.component_name == "behavior" &&
        !address.list_tokens.empty() && address.list_tokens.front() == "params") {
        token_index = 1;
    }
    for (; token_index < address.list_tokens.size(); ++token_index) {
        if (node->tag != Schema::NodeTag::Struct) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", address.object.object_id.value},
                         {"slot", address.component_name},
                         {"field_path", address.list_path},
                         {"detail", "list_path does not identify a schema list"}},
                        "list_path does not identify a schema list");
        }
        const auto child = std::find_if(
            node->children.begin(), node->children.end(), [&](const auto &entry) {
                return entry.name == address.list_tokens[token_index];
            });
        if (child == node->children.end()) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", address.object.object_id.value},
                         {"slot", address.component_name},
                         {"field_path", address.list_path},
                         {"detail", "list_path is absent from the schema"}},
                        "list_path is absent from the schema");
        }
        node = &*child;
    }
    if (node->tag != Schema::NodeTag::List || node->children.size() != 1U ||
        node->children.front().tag != Schema::NodeTag::Struct) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.list_path},
                     {"detail", "list_path does not identify a row list"}},
                    "list_path does not identify a row list");
    }
    return *node;
}

// Returns a reference into `list_schema`, which the caller owns.
const Schema::Node &requireRowLeafSchema(const AddressedComponent &address,
                                         const Schema::Node &list_schema) {
    const Schema::Node *node = &list_schema.children.front();
    for (std::size_t index = 0; index < address.leaf_tokens.size(); ++index) {
        if (node->tag == Schema::NodeTag::List) {
            if (address.leaf_tokens[index] == "rows" &&
                index + 1U < address.leaf_tokens.size() &&
                decimalIndex(address.leaf_tokens[index + 1U])) {
                editFailure(
                    EditorEditErrorCode::list_index_addressing_rejected,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.leaf_path}},
                    "ListIndexAddressingRejected: nested list rows require row_id addressing");
            }
            break;
        }
        if (node->tag != Schema::NodeTag::Struct) break;
        const auto child = std::find_if(
            node->children.begin(), node->children.end(), [&](const auto &entry) {
                return entry.name == address.leaf_tokens[index];
            });
        if (child == node->children.end()) break;
        node = &*child;
        if (index + 1U == address.leaf_tokens.size()) return *node;
    }
    editFailure(EditorEditErrorCode::schema_violation,
                {{"object", address.object.object_id.value},
                 {"slot", address.component_name},
                 {"field_path", address.leaf_path},
                 {"detail", "leaf_path does not identify a row field"}},
                "leaf_path does not identify a row field");
}

Json *findListValue(Json &component, const AddressedComponent &address) {
    Json *value = &component;
    for (const auto &token : address.list_tokens) {
        if (!value->is_object()) return nullptr;
        const auto found = value->find(token);
        if (found == value->end()) return nullptr;
        value = &*found;
    }
    return value;
}

Json &createMissingListValue(Json &component,
                             const AddressedComponent &address) {
    Json *value = &component;
    for (const auto &token : address.list_tokens) {
        if (!value->is_object()) {
            throw std::invalid_argument(
                "list_path parent is not an object and cannot be inserted");
        }
        value = &(*value)[token];
        if (value->is_null()) *value = Json::object();
    }
    return *value;
}

void validateListContainerShape(const Json &list,
                                const AddressedComponent &address) {
    const auto payload = [&] {
        return OrderedJson{{"object", address.object.object_id.value},
                           {"slot", address.component_name},
                           {"field_path", address.list_path}};
    };
    if (!list.is_object() || !list.contains("rows") ||
        !list.at("rows").is_array()) {
        editFailure(EditorEditErrorCode::list_container_missing, payload(),
                    "ListContainerMissing: list must use an object container");
    }
    if (!list.contains("$list") || !list.at("$list").is_object() ||
        !list.at("$list").contains("v") ||
        !list.at("$list").contains("next_id")) {
        editFailure(EditorEditErrorCode::row_envelope_missing, payload(),
                    "RowEnvelopeMissing: invalid $list envelope");
    }
    const auto &meta = list.at("$list");
    if (exactU32(meta.at("v"), "$list.v") != 1U) {
        editFailure(EditorEditErrorCode::row_envelope_unsupported, payload(),
                    "RowEnvelopeUnsupported: unknown $list version");
    }
    (void)exactU32(meta.at("next_id"), "$list.next_id", true);
    for (const auto &row : list.at("rows")) {
        if (!row.is_object() || !row.contains("$row") ||
            !row.at("$row").is_object() ||
            !row.at("$row").contains("id") ||
            !row.at("$row").contains("v")) {
            editFailure(EditorEditErrorCode::row_envelope_missing, payload(),
                        "RowEnvelopeMissing: row has no $row envelope");
        }
        if (exactU32(row.at("$row").at("v"), "$row.v") != 1U) {
            editFailure(EditorEditErrorCode::row_envelope_unsupported,
                        payload(),
                        "RowEnvelopeUnsupported: unknown $row version");
        }
        (void)exactU32(row.at("$row").at("id"), "$row.id", true);
    }
}

std::pair<std::size_t, Json &> requireRow(Json &list, std::uint32_t row_id,
                                         const AddressedComponent &address) {
    validateListContainerShape(list, address);
    auto &rows = list.at("rows");
    for (std::size_t index = 0; index < rows.size(); ++index) {
        auto &row = rows.at(index);
        if (exactU32(row.at("$row").at("id"), "$row.id", true) == row_id) {
            return {index, row};
        }
    }
    editFailure(EditorEditErrorCode::schema_violation,
                {{"object", address.object.object_id.value},
                 {"slot", address.component_name},
                 {"field_path", address.list_path},
                 {"row_id", row_id},
                 {"detail", "row_id is absent from the current list"}},
                "row_id is absent from the current list");
}

void validateRawComponent(const Json &component, AuthoringObjectId object_id,
                          std::string_view component_name) {
    if (component_name == "behavior") {
        (void)canonicalBehaviorComponent(component, object_id);
    } else {
        (void)canonicalComponent(component, object_id, component_name);
    }
}

void validateListHighWaterMonotonic(const Json &previous, Json &next,
                                    const Schema::Node &schema,
                                    AuthoringObjectId object_id,
                                    std::string_view component_name,
                                    const std::string &path,
                                    bool preserve_on_replay = false) {
    if (schema.tag == Schema::NodeTag::List) {
        if (!previous.is_object() || !next.is_object() ||
            !previous.contains("$list") || !next.contains("$list")) {
            return;
        }
        const auto old_next_id = exactU32(
            previous.at("$list").at("next_id"), "$list.next_id", true);
        const auto new_next_id =
            exactU32(next.at("$list").at("next_id"), "$list.next_id", true);
        if (new_next_id < old_next_id) {
            if (preserve_on_replay) {
                next.at("$list")["next_id"] = old_next_id;
                return;
            }
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", path + "/$list/next_id"},
                         {"detail", "$list.next_id must not decrease"},
                         {"previous", old_next_id},
                         {"next", new_next_id}},
                        "$list.next_id must not decrease");
        }
        return;
    }
    if (schema.tag != Schema::NodeTag::Struct || !previous.is_object() ||
        !next.is_object()) {
        return;
    }
    for (const auto &child : schema.children) {
        const auto old_member = previous.find(child.name);
        const auto new_member = next.find(child.name);
        if (old_member == previous.end() || new_member == next.end()) continue;
        validateListHighWaterMonotonic(
            *old_member, *new_member, child, object_id, component_name,
            path + "/" + child.name, preserve_on_replay);
    }
}

void validateRawComponentTransition(const Json &previous, Json &next,
                                    AuthoringObjectId object_id,
                                    std::string_view component_name,
                                    bool preserve_on_replay) {
    validateRawComponent(next, object_id, component_name);
    std::optional<Schema::Node> owned;
    const Schema::Node *schema = nullptr;
    const Json *old_root = &previous;
    Json *new_root = &next;
    if (component_name == "behavior") {
        const auto *registration = internal::getBehaviorRegisterer().findByName(
            next.value("type", std::string{}));
        if (registration == nullptr) return;
        schema = internal::getBehaviorRegisterer().schemaFor(*registration);
        if (schema == nullptr || !previous.contains("params") ||
            !next.contains("params")) {
            return;
        }
        old_root = &previous.at("params");
        new_root = &next.at("params");
    } else if (const auto *codec = findComponentCodec(component_name)) {
        owned = internal::hostSchemaNode(codec->schemaNode());
        schema = &*owned;
    }
    if (schema != nullptr) {
        validateListHighWaterMonotonic(*old_root, *new_root, *schema, object_id,
                                       component_name, {}, preserve_on_replay);
    }
}

std::optional<std::string_view> classifyLegacyListPointer(
    const Json &component, std::string_view component_name,
    std::span<const std::string> tokens) {
    std::optional<Schema::Node> owned;
    const Schema::Node *node = nullptr;
    std::size_t index = 0;
    if (component_name == "behavior") {
        if (tokens.empty() || tokens.front() != "params") return std::nullopt;
        const auto *registration = internal::getBehaviorRegisterer().findByName(
            component.value("type", std::string{}));
        if (registration == nullptr) return std::nullopt;
        node = internal::getBehaviorRegisterer().schemaFor(*registration);
        index = 1;
    } else if (const auto *codec = findComponentCodec(component_name)) {
        owned = internal::hostSchemaNode(codec->schemaNode());
        node = &*owned;
    }
    for (; node != nullptr && index < tokens.size(); ++index) {
        if (node->tag != Schema::NodeTag::Struct) return std::nullopt;
        const auto child = std::find_if(
            node->children.begin(), node->children.end(), [&](const auto &entry) {
                return entry.name == tokens[index];
            });
        if (child == node->children.end()) return std::nullopt;
        node = &*child;
        if (node->tag != Schema::NodeTag::List) continue;

        if (index + 1U == tokens.size()) return "list_root";
        const auto &container_member = tokens[index + 1U];
        if (container_member == "$list") return "list_envelope";
        if (container_member == "$row") return "row_envelope";
        if (container_member != "rows") return "list_root";
        if (index + 2U == tokens.size()) return "rows_container";

        const auto &row_selector = tokens[index + 2U];
        if (row_selector == "-") return "append_index";
        if (!decimalIndex(row_selector)) return "rows_container";
        if (index + 3U < tokens.size() && tokens[index + 3U] == "$row") {
            return "row_envelope";
        }
        return "numeric_index";
    }
    return std::nullopt;
}

void rejectLegacyListPointer(const Json &component,
                             std::string_view component_name,
                             std::span<const std::string> tokens,
                             AuthoringObjectId object_id,
                             std::string_view field_path) {
    const auto classification =
        classifyLegacyListPointer(component, component_name, tokens);
    if (!classification) return;
    editFailure(EditorEditErrorCode::list_index_addressing_rejected,
                {{"object", object_id.value},
                 {"slot", component_name},
                 {"field_path", field_path},
                 {"pointer_classification", *classification}},
                "ListIndexAddressingRejected: list structure requires addressed row operations");
}

[[noreturn]] void throwPrefabEditFailure(const PrefabError &error,
                                         AuthoringObjectId object_id,
                                         std::string_view parameter) {
    auto code = EditorEditErrorCode::schema_violation;
    switch (error.code()) {
    case PrefabErrorCode::PrefabDependencyMismatch:
    case PrefabErrorCode::PrefabNotFound:
        code = EditorEditErrorCode::prefab_dependency_mismatch;
        break;
    case PrefabErrorCode::PrefabProviderStale:
        code = EditorEditErrorCode::prefab_provider_stale;
        break;
    case PrefabErrorCode::PrefabParameterRequired:
        code = EditorEditErrorCode::prefab_parameter_required;
        break;
    case PrefabErrorCode::PrefabParameterUnknown:
        code = EditorEditErrorCode::prefab_parameter_unknown;
        break;
    case PrefabErrorCode::PrefabParameterType:
        code = EditorEditErrorCode::prefab_parameter_type;
        break;
    default: break;
    }
    editFailure(code,
                {{"object", object_id.value},
                 {"parameter", parameter},
                 {"prefab_error", prefabErrorCodeName(error.code())},
                 {"field_path",
                  error.context().json_pointer
                      ? OrderedJson(*error.context().json_pointer)
                      : OrderedJson(nullptr)}},
                error.what());
}

} // namespace EditorJournalInternal

// The helpers above keep external linkage so editorjournalops.cpp and
// editorjournalintegrity.cpp can call them; everything below stays
// internal to this translation unit.
using namespace EditorJournalInternal;

namespace {

EditorProjectionCommand makePrefabParameterCommand(
    const ObjectLocation &object, std::string parameter,
    std::optional<Json> value) {
    return EditorProjectionCommand{
        .object_path = "/authoring_objects/" +
                       std::to_string(object.object_id.value) +
                       "/prefab/parameters/" + parameter,
        .apply = [scene_id = object.scene_id,
                  object_index = object.object_index,
                  parameter = std::move(parameter),
                  value = std::move(value)](Json &document) {
            auto &object_json = document.at("scenes").at(scene_id).at("objects")
                                    .at(object_index);
            auto &prefab = object_json.at("prefab");
            if (value) {
                if (!prefab.contains("parameters")) {
                    prefab["parameters"] = Json::object();
                }
                prefab.at("parameters")[parameter] = *value;
            } else if (const auto parameters = prefab.find("parameters");
                       parameters != prefab.end()) {
                parameters->erase(parameter);
                if (parameters->empty()) prefab.erase("parameters");
            }
        },
    };
}

PreparedOperation preparePrefabParameter(
    const Json &raw, const AuthoringSceneDocument &document,
    const PrefabRegistrySnapshot &registry, bool unset) {
    const auto op = unset ? std::string{"unset_prefab_parameter"}
                          : std::string{"set_prefab_parameter"};
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object"), op + " object", true)};
    const auto parameter = requireString(raw, "parameter", op);
    const auto object = requireObjectLocation(document, object_id);
    const auto prefab = object.authored.find("prefab");
    if (prefab == object.authored.end() || !prefab->is_object()) {
        editFailure(EditorEditErrorCode::closure_unresolvable,
                    {{"object", object_id.value},
                     {"parameter", parameter},
                     {"object_path", "/authoring_objects/" +
                                         std::to_string(object_id.value) +
                                         "/prefab"}},
                    "authoring object is not a prefab instance");
    }

    PrefabInstanceDeclaration previous;
    try {
        previous = resolvePrefabInstance(
            *prefab, registry,
            {.scene_id = object.scene_id,
             .parameter = parameter,
             .json_pointer = "/prefab/parameters/" + parameter});
    } catch (const PrefabError &error) {
        throwPrefabEditFailure(error, object_id, parameter);
    }
    const auto *record = registry.find(previous.ref);
    if (record == nullptr) {
        editFailure(EditorEditErrorCode::prefab_dependency_mismatch,
                    {{"object", object_id.value}, {"parameter", parameter}},
                    "prefab dependency disappeared from the registry snapshot");
    }
    const auto declaration = std::find_if(
        record->document->parameters.begin(),
        record->document->parameters.end(), [&](const auto &candidate) {
            return candidate.name == parameter;
        });
    if (declaration == record->document->parameters.end()) {
        editFailure(EditorEditErrorCode::prefab_parameter_unknown,
                    {{"object", object_id.value}, {"parameter", parameter}},
                    "unknown prefab parameter: " + parameter);
    }
    const auto old = std::find_if(
        previous.parameters.begin(), previous.parameters.end(),
        [&](const auto &candidate) { return candidate.name == parameter; });
    if (old == previous.parameters.end()) {
        throw std::logic_error("resolved prefab lost its parameter declaration");
    }
    if (unset && !declaration->default_value) {
        editFailure(EditorEditErrorCode::prefab_parameter_required,
                    {{"object", object_id.value}, {"parameter", parameter}},
                    "required prefab parameter cannot be unset");
    }

    auto next_prefab = *prefab;
    if (unset) {
        if (const auto parameters = next_prefab.find("parameters");
            parameters != next_prefab.end()) {
            parameters->erase(parameter);
            if (parameters->empty()) next_prefab.erase("parameters");
        }
    } else {
        if (!next_prefab.contains("parameters")) {
            next_prefab["parameters"] = Json::object();
        }
        next_prefab.at("parameters")[parameter] = raw.at("value");
    }
    try {
        (void)resolvePrefabInstance(
            next_prefab, registry,
            {.scene_id = object.scene_id,
             .instance_id = previous.instance_id,
             .prefab = previous.ref,
             .parameter = parameter,
             .json_pointer = "/prefab/parameters/" + parameter});
    } catch (const PrefabError &error) {
        throwPrefabEditFailure(error, object_id, parameter);
    }

    OrderedJson forward{{"op", op},
                        {"object", object_id.value},
                        {"parameter", parameter}};
    if (!unset) forward["value"] = raw.at("value");
    OrderedJson inverse;
    if (old->is_override) {
        inverse = {{"op", "set_prefab_parameter"},
                   {"object", object_id.value},
                   {"parameter", parameter},
                   {"value", *old->value_authored}};
    } else {
        inverse = {{"op", "unset_prefab_parameter"},
                   {"object", object_id.value},
                   {"parameter", parameter}};
    }
    const auto target = "/authoring_objects/" +
                        std::to_string(object_id.value) +
                        "/prefab/parameters/" + parameter;
    return PreparedOperation{
        .commands = {makePrefabParameterCommand(
            object, parameter,
            unset ? std::optional<Json>{}
                  : std::optional<Json>{raw.at("value")})},
        .forward = std::move(forward),
        .inverse = std::move(inverse),
        .affected = {object_id},
        .stable_target = target,
        .read_set = {target},
        .write_set = {target},
        .structural_domain = {{"kind", "prefab_parameter"},
                              {"object", object_id.value},
                              {"parameter", parameter}},
    };
}

EditorEditErrorCode transitionEditErrorCode(
    OriginTransitionErrorCode code) noexcept {
    switch (code) {
    case OriginTransitionErrorCode::TargetMismatch:
        return EditorEditErrorCode::origin_transition_target_mismatch;
    case OriginTransitionErrorCode::GeneratedIdSetMismatch:
        return EditorEditErrorCode::origin_transition_gid_set_mismatch;
    case OriginTransitionErrorCode::AuthoredIndexSetMismatch:
        return EditorEditErrorCode::origin_transition_index_set_mismatch;
    case OriginTransitionErrorCode::AuthoredIndexInvalid:
        return EditorEditErrorCode::origin_transition_index_invalid;
    case OriginTransitionErrorCode::ComponentMismatch:
        return EditorEditErrorCode::origin_transition_component_mismatch;
    case OriginTransitionErrorCode::ComponentKeyMismatch:
        return EditorEditErrorCode::origin_transition_component_key_mismatch;
    case OriginTransitionErrorCode::GeneratedIdMismatch:
        return EditorEditErrorCode::origin_transition_gid_derivation_mismatch;
    case OriginTransitionErrorCode::AuthorityBodyMismatch:
        return EditorEditErrorCode::origin_transition_body_mismatch;
    }
    return EditorEditErrorCode::origin_transition_component_mismatch;
}

[[noreturn]] void throwOriginTransitionFailure(
    const OriginTransitionError &error, AuthoringObjectId object_id) {
    editFailure(transitionEditErrorCode(error.code()),
                {{"object", object_id.value},
                 {"transition_error",
                  originTransitionErrorCodeName(error.code())}},
                error.what());
}

OrderedJson unpackExpandedComponentsJson(
    std::span<const OriginTransitionAuthority> authority) {
    OrderedJson result = OrderedJson::array();
    for (const auto &component : authority) {
        result.push_back({{"component_key", component.component_key},
                          {"body", component.body}});
    }
    return result;
}

OrderedJson unpackDependencyWitness(
    const PrefabInstanceDeclaration &instance,
    const PrefabRegistryRecord &record, const ResolvedScene &resolved,
    std::span<const OriginTransitionAuthority> authority) {
    std::vector<std::string> behavior_names;
    for (const auto &component : authority) {
        if (component.body.value("name", std::string{}) == "behavior") {
            behavior_names.push_back(
                component.body.value("type", std::string{}));
        }
    }
    std::sort(behavior_names.begin(), behavior_names.end());
    behavior_names.erase(
        std::unique(behavior_names.begin(), behavior_names.end()),
        behavior_names.end());
    OrderedJson behaviors = OrderedJson::array();
    const auto dependencies =
        resolved.bindableProvider().behaviorDependencies();
    for (const auto &name : behavior_names) {
        const auto found = std::lower_bound(
            dependencies.begin(), dependencies.end(), name,
            [](const auto &dependency, std::string_view stable_name) {
                return dependency.stable_name < stable_name;
            });
        if (found == dependencies.end() || found->stable_name != name) {
            editFailure(EditorEditErrorCode::prefab_provider_stale,
                        {{"behavior", name}},
                        "generated behavior dependency is absent from the provider");
        }
        behaviors.push_back(
            {{"stable_name", found->stable_name},
             {"schema_version", found->schema_version},
             {"params_schema_fingerprint",
              found->params_schema_fingerprint}});
    }
    const auto &closure = resolved.prefabClosure();
    return {{"ref", instance.ref},
            {"instance_id", instance.instance_id},
            {"prefab_digest_sha256", record.digest_sha256},
            {"provider_fingerprint_sha256",
             closure.provider_fingerprint_sha256},
            {"behaviors", std::move(behaviors)},
            {"resolver_inputs_fingerprint_sha256",
             closure.resolver_inputs_fingerprint_sha256}};
}

EditorProjectionCommand makeUnpackPrefabCommand(
    const ObjectLocation &object, Json prefab,
    std::vector<OriginTransitionAuthority> authority, bool restore) {
    const auto path = "/authoring_objects/" +
                      std::to_string(object.object_id.value);
    return EditorProjectionCommand{
        .object_path = path,
        .apply = [scene_id = object.scene_id,
                  object_index = object.object_index,
                  prefab = std::move(prefab),
                  authority = std::move(authority), restore](Json &document) {
            auto &object_json = document.at("scenes").at(scene_id)
                                    .at("objects").at(object_index);
            auto &components = object_json.at("components");
            if (!restore) {
                const auto current = object_json.find("prefab");
                if (current == object_json.end() || *current != prefab) {
                    throw std::runtime_error(
                        "unpack prefab block changed before application");
                }
                for (const auto &component : authority) {
                    components.push_back(component.body);
                }
                object_json.erase("prefab");
                return;
            }

            if (object_json.contains("prefab") ||
                components.size() < authority.size()) {
                throw std::runtime_error(
                    "unpack inverse target does not contain the expanded interval");
            }
            const auto first = components.size() - authority.size();
            for (std::size_t index = 0; index < authority.size(); ++index) {
                if (OrderedJson(components.at(first + index)) !=
                    authority[index].body) {
                    throw std::runtime_error(
                        "unpack inverse expanded component changed");
                }
            }
            components.erase(
                components.begin() +
                    static_cast<Json::difference_type>(first),
                components.end());
            object_json["prefab"] = prefab;
        },
    };
}

PreparedOperation prepareUnpackPrefab(
    const Json &raw, const AuthoringSceneDocument &document,
    const ResolvedScene &resolved) {
    constexpr auto op = "unpack_prefab";
    requireOnly(raw, {"op", "object"}, op);
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object"), "unpack_prefab object", true)};
    const auto object = requireObjectLocation(document, object_id);
    const auto prefab = object.authored.find("prefab");
    if (prefab == object.authored.end() || !prefab->is_object()) {
        editFailure(EditorEditErrorCode::closure_unresolvable,
                    {{"object", object_id.value},
                     {"object_path", "/authoring_objects/" +
                                         std::to_string(object_id.value) +
                                         "/prefab"}},
                    "authoring object is not a prefab instance");
    }

    PrefabInstanceDeclaration instance;
    try {
        instance = resolvePrefabInstance(
            *prefab, resolved.prefabRegistry(),
            {.scene_id = object.scene_id, .json_pointer = "/prefab"});
    } catch (const PrefabError &error) {
        throwPrefabEditFailure(error, object_id, {});
    }
    const auto *record = resolved.prefabRegistry().find(instance.ref);
    if (record == nullptr) {
        editFailure(EditorEditErrorCode::prefab_dependency_mismatch,
                    {{"object", object_id.value}, {"ref", instance.ref}},
                    "prefab dependency disappeared from the registry snapshot");
    }

    std::vector<OriginTransitionAuthority> authority;
    authority.reserve(record->document->components.size());
    try {
        for (const auto &component : record->document->components) {
            auto body = resolvePrefabComponentBody(
                *record->document, component, instance,
                resolved.bindableProvider().descriptors(),
                {.scene_id = object.scene_id,
                 .instance_id = instance.instance_id,
                 .prefab = instance.ref});
            std::vector<std::string> marker_fields;
            for (auto field = body.begin(); field != body.end(); ++field) {
                if (prefabGeneratedMarkerRule(field.key(), field.value())) {
                    marker_fields.push_back(field.key());
                }
            }
            for (const auto &field : marker_fields) body.erase(field);
            authority.push_back({.component_key = component.key,
                                 .body = std::move(body)});
        }
    } catch (const PrefabError &error) {
        throwPrefabEditFailure(error, object_id, {});
    }

    OrderedJson forward{
        {"op", op},
        {"object", object_id.value},
        {"scene_id", object.scene_id},
        {"prefab", OrderedJson(*prefab)},
        {"expanded_components", unpackExpandedComponentsJson(authority)},
        {"dependency_witness",
         unpackDependencyWitness(instance, *record, resolved, authority)},
    };
    const auto target = "/authoring_objects/" +
                        std::to_string(object_id.value);
    return PreparedOperation{
        .commands = {makeUnpackPrefabCommand(object, *prefab, authority,
                                              false)},
        .forward = std::move(forward),
        .inverse = OrderedJson::object(),
        .affected = {object_id},
        .stable_target = target,
        .read_set = {target},
        .write_set = {target},
        .structural_domain = {{"kind", "object_subtree"},
                              {"scene_id", object.scene_id},
                              {"object", object_id.value}},
        .unpack = PreparedUnpack{
            .generated_scene = resolved,
            .scene_id = object.scene_id,
            .object_id = object_id,
            .instance_id = instance.instance_id,
            .authority = std::move(authority),
        },
    };
}

OrderedJson invertUnpackOperation(const OrderedJson &forward) {
    auto result = forward;
    const auto op = forward.value("op", std::string{});
    if (op == "unpack_prefab") result["op"] = "restore_prefab";
    else if (op == "restore_prefab") result["op"] = "unpack_prefab";
    else throw std::invalid_argument("operation is not an unpack inverse");
    const auto map = originTransitionMapFromJson(
        forward.at("origin_transition"));
    result["origin_transition"] =
        originTransitionMapJson(invertOriginTransitionMap(map));
    return result;
}

void finalizePreparedUnpack(PreparedOperation &prepared,
                            const AuthoringSceneDocument &authored_document,
                            const ResolvedScene &authored_scene) {
    if (!prepared.unpack) return;
    auto &unpack = *prepared.unpack;
    try {
        const auto transition = buildOriginTransitionMap(
            unpack.generated_scene, authored_document, authored_scene,
            unpack.scene_id, unpack.object_id, unpack.instance_id,
            unpack.authority);
        prepared.forward["origin_transition"] =
            originTransitionMapJson(transition);
        prepared.inverse = invertUnpackOperation(prepared.forward);
        if (const auto failure = unpackJournalIntegrityFailure(
                prepared.forward, prepared.inverse)) {
            editFailure(EditorEditErrorCode::journal_integrity_error,
                        {{"reason", *failure},
                         {"object", unpack.object_id.value}},
                        "generated unpack journal pair is inconsistent");
        }
    } catch (const OriginTransitionError &error) {
        throwOriginTransitionFailure(error, unpack.object_id);
    }
}

PreparedOperation prepareRpcOperation(const Json &raw,
                                      const AuthoringSceneDocument &document,
                                      const ResolvedScene &resolved,
                                      std::string_view current_scene,
                                      const PrefabRegistrySnapshot &registry,
                                      const std::function<bool(std::string_view)> &
                                          component_exists) {
    requireObject(raw, "edit operation");
    const auto op = requireString(raw, "op", "edit operation");
    if (op == "set_component_value") return prepareSet(raw, document);
    if (op == "insert_component_field")
        return prepareInsertField(raw, document);
    if (op == "remove_component_field")
        return prepareRemoveField(raw, document);
    if (op == "move_component_field")
        return prepareMoveField(raw, document);
    if (op == "add_component")
        return prepareAdd(raw, document, component_exists);
    if (op == "remove_component") return prepareRemove(raw, document);
    if (op == "spawn")
        return prepareSpawn(raw, document, current_scene, component_exists);
    if (op == "destroy") return prepareDestroy(raw, document);
    if (op == "reparent") return prepareReparent(raw, document);
    if (op == "set_prefab_parameter")
        return preparePrefabParameter(raw, document, registry, false);
    if (op == "unset_prefab_parameter")
        return preparePrefabParameter(raw, document, registry, true);
    if (op == "unpack_prefab")
        return prepareUnpackPrefab(raw, document, resolved);
    editFailure(EditorEditErrorCode::method_unavailable,
                {{"method", op}, {"adapter", "editor_operation"}},
                "unknown editor operation");
}

EditFailure projectionFailure(const EditorProjectionResult &result,
                              std::string_view method) {
    const auto code = result.error ? result.error->code
                                   : EditorProjectionErrorCode::CommandInvalid;
    const auto path = result.error ? result.error->object_path : std::string{"/"};
    const auto message = result.error ? result.error->message
                                      : std::string{"projection transaction failed"};
    if (result.error && result.error->prefab_error_code) {
        const auto prefab_code = *result.error->prefab_error_code;
        auto edit_code = EditorEditErrorCode::schema_violation;
        switch (prefab_code) {
        case PrefabErrorCode::PrefabInstanceIdCollision:
            edit_code = EditorEditErrorCode::prefab_instance_id_collision;
            break;
        case PrefabErrorCode::PrefabDependencyMismatch:
        case PrefabErrorCode::PrefabNotFound:
            edit_code = EditorEditErrorCode::prefab_dependency_mismatch;
            break;
        case PrefabErrorCode::PrefabProviderStale:
            edit_code = EditorEditErrorCode::prefab_provider_stale;
            break;
        case PrefabErrorCode::PrefabParameterRequired:
            edit_code = EditorEditErrorCode::prefab_parameter_required;
            break;
        case PrefabErrorCode::PrefabParameterUnknown:
            edit_code = EditorEditErrorCode::prefab_parameter_unknown;
            break;
        case PrefabErrorCode::PrefabParameterType:
            edit_code = EditorEditErrorCode::prefab_parameter_type;
            break;
        default: break;
        }
        return EditFailure{
            edit_code,
            OrderedJson{{"method", method},
                        {"object_path", path},
                        {"prefab_error", prefabErrorCodeName(prefab_code)}},
            message};
    }
    switch (code) {
    case EditorProjectionErrorCode::StaleRevision:
        return EditFailure{EditorEditErrorCode::stale_revision,
                           OrderedJson{{"current_revision", result.base_revision.value},
                                       {"target", nullptr}}, message};
    case EditorProjectionErrorCode::TransformCycle:
        return EditFailure{EditorEditErrorCode::cycle_detected,
                           OrderedJson{{"object_path", path}}, message};
    case EditorProjectionErrorCode::TransformZeroParentScale:
        return EditFailure{EditorEditErrorCode::zero_scale,
                           OrderedJson{{"object_path", path}}, message};
    case EditorProjectionErrorCode::TransformNonFinite:
        return EditFailure{EditorEditErrorCode::non_finite_transform,
                           OrderedJson{{"object_path", path}}, message};
    case EditorProjectionErrorCode::TransformUnrepresentable:
        return EditFailure{EditorEditErrorCode::trs_unrepresentable,
                           OrderedJson{{"object_path", path}}, message};
    case EditorProjectionErrorCode::CodecMissing:
        return EditFailure{EditorEditErrorCode::not_editable,
                           OrderedJson{{"object", nullptr}, {"slot", path}}, message};
    case EditorProjectionErrorCode::ComponentNotFound:
        return EditFailure{EditorEditErrorCode::missing_component,
                           OrderedJson{{"object", nullptr}, {"slot", path}}, message};
    case EditorProjectionErrorCode::ObjectNotFound:
        return EditFailure{method == "reparent"
                               ? EditorEditErrorCode::parent_not_found
                               : EditorEditErrorCode::closure_unresolvable,
                            method == "reparent" ? OrderedJson{{"name", path}}
                                                  : OrderedJson{{"object_path", path}},
                            message};
    case EditorProjectionErrorCode::BehaviorTypeUnknown:
        return EditFailure{
            EditorEditErrorCode::behavior_type_unknown,
            result.error ? result.error->data : OrderedJson::object(),
            message};
    case EditorProjectionErrorCode::RuntimeBindingMissing:
        return EditFailure{
            EditorEditErrorCode::runtime_binding_missing,
            result.error ? result.error->data : OrderedJson::object(),
            message};
    case EditorProjectionErrorCode::AdapterPrepareFailed:
    case EditorProjectionErrorCode::AdapterPublishFailed:
        return EditFailure{EditorEditErrorCode::method_unavailable,
                           OrderedJson{{"method", method}, {"adapter", path}}, message};
    case EditorProjectionErrorCode::CommandInvalid:
        if (message.starts_with("editor_preview_unsupported@1")) {
            return EditFailure{
                EditorEditErrorCode::preview_unsupported,
                OrderedJson{{"method", method}, {"detail", message}},
                message};
        }
        return EditFailure{EditorEditErrorCode::schema_violation,
                           OrderedJson{{"object", nullptr}, {"slot", nullptr},
                                       {"field_path", path}, {"detail", message}}, message};
    }
    return EditFailure{EditorEditErrorCode::schema_violation,
                       OrderedJson{{"object", nullptr}, {"slot", nullptr},
                                   {"field_path", path}, {"detail", message}}, message};
}

Json normalizeBehaviorAttachmentIdentities(
    const Json &raw_operations, const AuthoringSceneDocument &document,
    SceneRevision base_revision,
    const EditorEditRuntimeDependencies &dependencies) {
    if (!raw_operations.is_array()) return raw_operations;
    Json result = raw_operations;
    for (std::size_t command_index = 0; command_index < result.size();
         ++command_index) {
        auto &operation = result.at(command_index);
        if (!operation.is_object()) continue;
        const auto op = operation.value("op", std::string{});
        const bool addressed = operation.contains("address") &&
                               (op == "set_component_value" ||
                                op == "insert_component_field" ||
                                op == "remove_component_field" ||
                                op == "move_component_field");
        if (addressed) {
            operation.erase("attachment_handle");
            operation.erase("attachment_seq");
            const auto address = requireComponentAddress(
                operation, document, op,
                op == "insert_component_field");
            if (address.component_name != "behavior") continue;
            const auto object_id = address.object.object_id;
            const auto attachment_index = address.component_index;
            const auto expected_identity = address.attachment_identity;
            if (!dependencies.resolve_behavior_attachment) {
                behaviorAttachmentIntegrityFailure(
                    "behavior_attachment_resolver_unavailable", object_id,
                    attachment_index, expected_identity);
            }
            const auto identity = dependencies.resolve_behavior_attachment(
                object_id, attachment_index);
            if (!identity) {
                behaviorAttachmentIntegrityFailure(
                    "behavior_attachment_resolver_missing", object_id,
                    attachment_index, expected_identity);
            }
            if (!sameBehaviorAttachmentWitness(
                    runtimeBehaviorAttachmentWitness(*identity),
                    expected_identity)) {
                behaviorAttachmentIntegrityFailure(
                    "behavior_attachment_witness_mismatch", object_id,
                    attachment_index, expected_identity,
                    runtimeBehaviorAttachmentWitness(*identity));
            }
            operation["attachment_handle"] = identity->handle;
            operation["attachment_seq"] = identity->attachment_seq;
            continue;
        }
        const bool add_behavior =
            op == "add_component" && operation.contains("component") &&
            operation.at("component").is_object() &&
            operation.at("component").value("name", std::string{}) == "behavior";
        const bool target_behavior =
            (op == "remove_component" || op == "set_component_value" ||
             op == "insert_component_field" ||
             op == "remove_component_field" ||
             op == "move_component_field") &&
            operation.value("component_slot", std::string{}) == "behavior";
        if (!add_behavior && !target_behavior) continue;

        operation.erase("attachment_handle");
        operation.erase("attachment_seq");
        operation.erase("attachment_identity");
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object_id"), "behavior edit object_id", true)};
        const auto object = requireObjectLocation(document, object_id);
        std::size_t attachment_index = 0;
        if (add_behavior) {
            attachment_index = object.authored.at("components").size();
            if (const auto found = operation.find("component_index");
                found != operation.end()) {
                attachment_index = static_cast<std::size_t>(exactUnsigned(
                    *found, "behavior add component_index"));
            }
            if (!dependencies.allocate_behavior_attachment) {
                editFailure(EditorEditErrorCode::method_unavailable,
                            {{"method", "add_component"},
                             {"adapter", "behavior_identity"}},
                            "behavior attach identity allocator is unavailable");
            }
            if (base_revision.value == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("behavior commit sequence space exhausted");
            }
            const auto identity = dependencies.allocate_behavior_attachment(
                base_revision.value + 1U, command_index, attachment_index);
            const auto attachment_seq = sceneBehaviorAttachmentSeq(
                object.object_index, attachment_index);
            if (identity.handle == 0 || identity.handle > maxExactJsonInteger ||
                attachment_seq > maxExactJsonInteger) {
                throw std::overflow_error(
                    "behavior attachment identity exceeds the exact JSON range");
            }
            operation["attachment_handle"] = identity.handle;
            operation["attachment_seq"] = attachment_seq;
            operation["attachment_identity"] =
                authoredBehaviorAttachmentWitness(
                    object_id, attachment_index,
                    operation.at("component"));
            continue;
        }

        const auto found_index = operation.find("attachment_index");
        if (found_index == operation.end()) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", "behavior"},
                         {"field_path", "/attachment_index"},
                         {"detail", "behavior target requires attachment_index"}},
                        "behavior target requires attachment_index");
        }
        attachment_index = static_cast<std::size_t>(exactUnsigned(
            *found_index, "behavior attachment_index"));
        const auto &[component_index, component] =
            requireComponentAt(object, attachment_index, "behavior");
        const auto expected_identity = authoredBehaviorAttachmentWitness(
            object_id, component_index, component);
        operation["attachment_identity"] = expected_identity;
        if (!dependencies.resolve_behavior_attachment) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_resolver_unavailable", object_id,
                attachment_index, expected_identity);
        }
        const auto identity = dependencies.resolve_behavior_attachment(
            object_id, attachment_index);
        if (!identity) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_resolver_missing", object_id,
                attachment_index, expected_identity);
        }
        if (identity->handle == 0 ||
            identity->handle > maxExactJsonInteger ||
            identity->attachment_seq > maxExactJsonInteger) {
            throw std::overflow_error(
                "resolved behavior attachment identity exceeds the exact JSON range");
        }
        const auto runtime_identity =
            runtimeBehaviorAttachmentWitness(*identity);
        if (!sameBehaviorAttachmentWitness(runtime_identity,
                                           expected_identity)) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_witness_mismatch", object_id,
                attachment_index, expected_identity, runtime_identity);
        }
        operation["attachment_handle"] = identity->handle;
        operation["attachment_seq"] = identity->attachment_seq;
    }
    return result;
}

std::vector<OrderedJson> rebindJournalBehaviorTargets(
    std::span<const OrderedJson> operations,
    const EditorEditRuntimeDependencies &dependencies) {
    std::vector<OrderedJson> result{operations.begin(), operations.end()};

    for (auto &operation : result) {
        const auto op = operation.value("op", std::string{});
        if ((op != "remove_component" &&
             op != "set_component_value" &&
             op != "insert_component_field" &&
             op != "remove_component_field" &&
             op != "move_component_field") ||
            operation.value("component_slot", std::string{}) !=
                "behavior") {
            continue;
        }
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object_id"),
            "journal behavior object_id", true)};
        const auto attachment_index = static_cast<std::size_t>(
            exactUnsigned(operation.at("attachment_index"),
                          "journal behavior attachment_index"));
        const auto witness = operation.find("attachment_identity");
        if (witness == operation.end() || !witness->is_object()) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_identity_missing", object_id,
                attachment_index);
        }
        const OrderedJson expected_identity = *witness;
        const auto authored_identity =
            currentAuthoredBehaviorAttachmentWitness(
                dependencies.document(), object_id, attachment_index);
        if (!authored_identity ||
            !sameBehaviorAttachmentWitness(*authored_identity,
                                           expected_identity)) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_authored_identity", object_id,
                attachment_index, expected_identity,
                authored_identity.value_or(OrderedJson{}));
        }
        if (!dependencies.resolve_behavior_attachment) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_resolver_unavailable", object_id,
                attachment_index, expected_identity);
        }
        const auto current = dependencies.resolve_behavior_attachment(
            object_id, attachment_index);
        if (!current) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_resolver_missing", object_id,
                attachment_index, expected_identity);
        }
        if (current->handle == 0 ||
            current->handle > maxExactJsonInteger ||
            current->attachment_seq > maxExactJsonInteger) {
            throw std::overflow_error(
                "resolved behavior attachment identity exceeds the exact JSON range");
        }
        const auto runtime_identity =
            runtimeBehaviorAttachmentWitness(*current);
        if (!sameBehaviorAttachmentWitness(runtime_identity,
                                           expected_identity)) {
            behaviorAttachmentIntegrityFailure(
                "behavior_attachment_witness_mismatch", object_id,
                attachment_index, expected_identity, runtime_identity);
        }
        // Handle and sequence both belong to the current arena projection.
        // The authored witness is the integrity identity; refresh both
        // runtime values at the last possible point before materialization.
        operation["attachment_handle"] = current->handle;
        operation["attachment_seq"] = current->attachment_seq;
    }
    return result;
}

EditorCanonicalOperation freezeJournalOperation(
    const OrderedJson &wire, const SceneProjectionState &projection_state) {
    auto result = freezeEditorCanonicalOperation(wire, projection_state);
    std::visit(
        [&](auto &operation) {
            using Operation = std::remove_cvref_t<decltype(operation)>;
            if constexpr (std::same_as<Operation,
                                       EditorSetComponentValueOperation> ||
                          std::same_as<Operation,
                                       EditorAddComponentOperation>) {
                const auto &component =
                    operation.wire.at(
                        std::same_as<Operation,
                                     EditorSetComponentValueOperation>
                            ? "authored_component"
                            : "component");
                const auto canonical = operation.component == "behavior"
                                           ? canonicalBehaviorComponent(
                                                 component,
                                                 operation.object_id)
                                           : canonicalComponent(
                                                 component,
                                                 operation.object_id,
                                                 operation.component);
                operation.canonical_component = OrderedJson(canonical).dump();
                if (operation.component == "behavior") {
                    operation.canonical_behavior_params = OrderedJson(
                        canonical.value("params", Json::object())).dump();
                }
            } else if constexpr (
                std::same_as<Operation, EditorRemoveComponentOperation> ||
                std::same_as<Operation, EditorReparentOperation> ||
                std::same_as<Operation, EditorSpawnOperation> ||
                std::same_as<Operation, EditorDestroyOperation> ||
                std::same_as<Operation, EditorRemoveObjectsOperation> ||
                std::same_as<Operation, EditorRestoreObjectsOperation> ||
                std::same_as<Operation, EditorSetPrefabParameterOperation> ||
                std::same_as<Operation, EditorUnsetPrefabParameterOperation> ||
                std::same_as<Operation, EditorUnpackPrefabOperation> ||
                std::same_as<Operation, EditorRestorePrefabOperation>) {
                // These alternatives carry no authored component body, so
                // there is nothing to canonicalize onto the frozen operation.
            } else {
                static_assert(editorCanonicalOperationUnhandled<Operation>,
                              "unhandled EditorCanonicalOperation alternative");
            }
        },
        result);
    return result;
}

// PG-1: an operation that brings components into the scene crosses the gate
// scene load does, asked of the object as it resolved after the operation --
// so a prefab spawn is judged by the components its expansion produced.
void requireObjectFeatures(const ResolvedScene &resolved,
                           AuthoringObjectId object_id) {
    for (const auto &scene : resolved.scenes()) {
        for (const auto &object : scene.objects) {
            if (object.authoring_object_id != object_id) continue;
            const auto refusal = resolvedObjectFeatureRefusal(object);
            if (!refusal) return;
            throw EditFailure{
                EditorEditErrorCode::module_unavailable,
                {{"object", object_id.value},
                 {"slot", refusal->component},
                 {"requirement", refusal->requirement},
                 {"option", refusal->option}},
                refusal->message, refusal->code};
        }
    }
}

PreparedBatch prepareBatch(const Json &raw_operations,
                           const AuthoringSceneDocument &source,
                           std::string_view current_scene,
                           PrefabRegistrySnapshot registry = {},
                           BindableProviderSnapshot provider = {},
                           const SceneProjectionState *projection_state =
                               nullptr,
                           const std::function<bool(std::string_view)> &
                               component_exists = {}) {
    if (!raw_operations.is_array() || raw_operations.empty()) {
        throw std::invalid_argument("edit operations must be a non-empty array");
    }
    const auto contains_unpack = std::any_of(
        raw_operations.begin(), raw_operations.end(), [](const Json &raw) {
            return raw.is_object() &&
                   raw.value("op", std::string{}) == "unpack_prefab";
        });
    if (contains_unpack && raw_operations.size() != 1U) {
        editFailure(EditorEditErrorCode::unpack_must_be_singleton,
                    {{"operation_count", raw_operations.size()}},
                    "unpack_prefab must be the only operation in its request");
    }
    std::optional<LocalDocumentTarget> local;
    if (projection_state != nullptr &&
        projection_state->revision() == source.revision()) {
        local.emplace(*projection_state);
    } else {
        local.emplace(source, std::move(registry), std::move(provider));
    }
    PreparedBatch batch;
    batch.operations.reserve(raw_operations.size());
    batch.canonical_forward.reserve(raw_operations.size());
    for (const auto &raw : raw_operations) {
        auto prepared = prepareRpcOperation(
            raw, local->projectionDocument(), local->projectionResolvedScene(),
            current_scene, local->projectionResolvedScene().prefabRegistry(),
            component_exists);
        EditorProjectionTransaction transaction{
            *local, local->projectionDocument().revision()};
        std::vector<EditorProjectionAdapter *> adapters;
        const auto result = transaction.commit(prepared.commands, adapters);
        if (!result.committed()) {
            throw projectionFailure(result, prepared.forward.value("op", "edit"));
        }
        finalizePreparedUnpack(prepared, local->projectionDocument(),
                              local->projectionResolvedScene());
        if (prepared.forward.at("op") == "spawn") {
            const auto inserted = std::find_if(
                result.structural_changes.begin(), result.structural_changes.end(),
                [](const auto &change) {
                    return change.kind == AuthoringStructuralChangeKind::Insert;
                });
            if (inserted == result.structural_changes.end()) {
                throw std::logic_error("spawn preflight produced no structural insert");
            }
            prepared.forward["object_id"] = inserted->authoring_object_id.value;
            prepared.inverse["object_ids"].push_back(inserted->authoring_object_id.value);
            prepared.affected.push_back(inserted->authoring_object_id);
            prepared.structural_domain["object"] = inserted->authoring_object_id.value;
        } else if (prepared.forward.at("op") == "destroy") {
            std::vector<AuthoringObjectClosure> closures = result.removed_objects;
            std::sort(closures.begin(), closures.end(), [](const auto &left,
                                                            const auto &right) {
                return left.declaration_index < right.declaration_index;
            });
            for (const auto &closure : closures) {
                prepared.inverse["closures"].push_back(closureJson(closure));
            }
        }
        const auto &op = prepared.forward.at("op");
        if (op == "add_component" || op == "spawn") {
            requireObjectFeatures(
                local->projectionResolvedScene(),
                AuthoringObjectId{prepared.forward.at("object_id")
                                      .get<std::uint64_t>()});
        }
        batch.canonical_forward.push_back(
            freezeJournalOperation(prepared.forward,
                                   local->projectionState()));
        batch.commands.insert(batch.commands.end(), prepared.commands.begin(),
                              prepared.commands.end());
        batch.forward.push_back(prepared.forward);
        batch.affected.insert(batch.affected.end(), prepared.affected.begin(),
                              prepared.affected.end());
        batch.operations.push_back(std::move(prepared));
    }
    for (auto it = batch.operations.rbegin(); it != batch.operations.rend(); ++it) {
        batch.inverse.push_back(it->inverse);
    }
    std::sort(batch.affected.begin(), batch.affected.end(),
              [](auto left, auto right) { return left.value < right.value; });
    batch.affected.erase(std::unique(batch.affected.begin(), batch.affected.end()),
                         batch.affected.end());
    return batch;
}

EditorProjectionCommand commandFromCanonical(const Json &operation,
                                              const AuthoringSceneDocument &document) {
    const auto op = requireString(operation, "op", "journal operation");
    if (op == "unpack_prefab" || op == "restore_prefab") {
        requireOnly(operation,
                    {"op", "object", "scene_id", "prefab",
                     "expanded_components", "dependency_witness",
                     "origin_transition"},
                    "canonical unpack operation");
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object"), "journal unpack object", true)};
        const auto object = requireObjectLocation(document, object_id);
        if (requireString(operation, "scene_id", op) != object.scene_id) {
            editFailure(EditorEditErrorCode::journal_integrity_error,
                        {{"reason", "scene_id"},
                         {"object", object_id.value}},
                        "unpack journal scene does not match its object");
        }
        auto authority = unpackAuthorityFromJson(
            operation.at("expanded_components"));
        (void)originTransitionMapFromJson(
            operation.at("origin_transition"));
        return makeUnpackPrefabCommand(
            object, operation.at("prefab"), std::move(authority),
            op == "restore_prefab");
    }
    if (op == "set_prefab_parameter" ||
        op == "unset_prefab_parameter") {
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object"), "journal prefab object", true)};
        const auto object = requireObjectLocation(document, object_id);
        const auto parameter = requireString(operation, "parameter", op);
        return makePrefabParameterCommand(
            object, parameter,
            op == "set_prefab_parameter"
                ? std::optional<Json>{operation.at("value")}
                : std::optional<Json>{});
    }
    if (op == "set_component_value" ||
        op == "insert_component_field" ||
        op == "remove_component_field" ||
        op == "move_component_field") {
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object_id"), "journal object_id", true)};
        const auto object = requireObjectLocation(document, object_id);
        const auto component_name = requireString(operation, "component_slot", op);
        const auto located = operation.contains("attachment_index")
                                 ? requireComponentAt(
                                       object,
                                       static_cast<std::size_t>(exactUnsigned(
                                           operation.at("attachment_index"),
                                           "journal attachment_index")),
                                       component_name)
                                 : requireComponent(object, component_name);
        const auto &[index, current] = located;
        (void)current;
        return makeReplaceComponentCommand(object, index, component_name,
                                           operation.at("authored_component"));
    }
    if (op == "add_component") {
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object_id"), "journal object_id", true)};
        const auto object = requireObjectLocation(document, object_id);
        const auto index = static_cast<std::size_t>(exactUnsigned(
            operation.at("component_index"), "journal component_index"));
        return makeInsertComponentCommand(object, index, operation.at("component"));
    }
    if (op == "remove_component") {
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object_id"), "journal object_id", true)};
        const auto object = requireObjectLocation(document, object_id);
        const auto component_name = requireString(operation, "component_slot", op);
        const auto located = component_name == "behavior"
                                 ? requireComponentAt(
                                       object,
                                       static_cast<std::size_t>(exactUnsigned(
                                           operation.at("attachment_index"),
                                           "journal attachment_index")),
                                       component_name)
                                 : requireComponent(object, component_name);
        const auto &[index, current] = located;
        (void)current;
        return makeEraseComponentCommand(object, index, component_name);
    }
    if (op == "spawn") {
        if (const auto closure = operation.find("closure"); closure != operation.end()) {
            return makeRestoreObjectCommand(closureFromJson(*closure));
        }
        return makeInsertObjectCommand(
            requireString(operation, "scene_id", op),
            static_cast<std::size_t>(exactUnsigned(operation.at("declaration_index"),
                                                   "journal declaration_index")),
            operation.at("object"));
    }
    if (op == "remove_objects" || op == "destroy") {
        throw std::logic_error("multi-command canonical operation requires expansion");
    }
    if (op == "restore_objects") {
        throw std::logic_error("restore canonical operation requires expansion");
    }
    if (op == "reparent") {
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object_id"), "journal object_id", true)};
        const auto child = requireObjectLocation(document, object_id);
        const auto new_parent_name = operation.at("new_parent_name").is_null()
                                         ? std::optional<std::string>{}
                                         : std::optional{operation.at("new_parent_name")
                                                             .get<std::string>()};
        if (const auto restore = operation.find("restore_transform");
            restore != operation.end()) {
            const auto [index, current] = requireComponent(child, "transform");
            (void)current;
            return EditorProjectionCommand{
                .object_path = "/authoring_objects/" +
                               std::to_string(object_id.value) + "/parent",
                .apply = [scene_id = child.scene_id,
                          object_index = child.object_index, index,
                          new_parent_name,
                          transform = Json(*restore)](Json &raw) {
                    auto &object = raw.at("scenes").at(scene_id).at("objects")
                                       .at(object_index);
                    if (new_parent_name) object["parent"] = *new_parent_name;
                    else object.erase("parent");
                    object.at("components").at(index) = transform;
                },
            };
        }
        const auto preserve = operation.at("preserve") == "world"
                                  ? ReparentPreserve::World
                                  : ReparentPreserve::Local;
        return makeReparentCommand(child.scene_id, *child.name, new_parent_name,
                                   preserve);
    }
    throw std::invalid_argument("unknown canonical journal operation: " + op);
}

std::vector<EditorProjectionCommand>
commandsFromCanonical(std::span<const OrderedJson> operations,
                      const AuthoringSceneDocument &document,
                      PrefabRegistrySnapshot registry = {},
                      BindableProviderSnapshot provider = {},
                      const SceneProjectionState *projection_state = nullptr,
                      std::vector<EditorCanonicalOperation> *canonical_operations =
                          nullptr) {
    std::optional<LocalDocumentTarget> local;
    if (projection_state != nullptr &&
        projection_state->revision() == document.revision()) {
        local.emplace(*projection_state);
    } else {
        local.emplace(document, std::move(registry), std::move(provider));
    }
    std::vector<EditorProjectionCommand> result;
    if (canonical_operations != nullptr) {
        canonical_operations->clear();
        canonical_operations->reserve(operations.size());
    }
    for (auto operation : operations) {
        // Journal snapshots restore authored values, not the allocator's past.
        // Reconcile before freezing so typed projection and raw commands see
        // the same high water, including each step of a multi-operation undo.
        if (operation.contains("authored_component")) {
            const auto object_id = AuthoringObjectId{exactUnsigned(
                operation.at("object_id"), "journal object_id", true)};
            const auto object = requireObjectLocation(
                local->projectionDocument(), object_id);
            const auto component_name = requireString(
                operation, "component_slot", "journal operation");
            const auto located = operation.contains("attachment_index")
                ? requireComponentAt(object, static_cast<std::size_t>(
                    exactUnsigned(operation.at("attachment_index"),
                                  "journal attachment_index")), component_name)
                : requireComponent(object, component_name);
            Json next = operation.at("authored_component");
            validateRawComponentTransition(located.second, next, object_id,
                                           component_name, true);
            operation["authored_component"] = std::move(next);
        }
        if (operation.at("op") == "restore_prefab") {
            const auto object_id = AuthoringObjectId{exactUnsigned(
                operation.at("object"), "journal unpack object", true)};
            const auto object = requireObjectLocation(local->projectionDocument(), object_id);
            const auto &components = object.authored.at("components");
            auto authority = unpackAuthorityFromJson(operation.at("expanded_components"));
            if (components.size() >= authority.size()) {
                const auto first = components.size() - authority.size();
                for (std::size_t index = 0; index < authority.size(); ++index) {
                    Json expected = authority[index].body;
                    validateRawComponentTransition(components.at(first + index), expected,
                        object_id, expected.at("name").get<std::string>(), true);
                    authority[index].body = std::move(expected);
                }
                // Keep the inverse command and origin authority identical.
                // Only schema-defined List allocators advance; body equality
                // still rejects changed row IDs, values, or attachments.
                operation["expanded_components"] = unpackExpandedComponentsJson(authority);
            }
        }
        if (canonical_operations != nullptr) {
            canonical_operations->push_back(
                freezeJournalOperation(operation,
                                       local->projectionState()));
        }
        const auto op = operation.at("op").get<std::string>();
        const bool unpack = op == "unpack_prefab";
        const bool restore = op == "restore_prefab";
        std::optional<AuthoringSceneDocument> base_document;
        std::optional<ResolvedScene> base_resolved;
        if (unpack || restore) {
            base_document = local->projectionDocument();
            base_resolved = local->projectionResolvedScene();
        }
        std::vector<EditorProjectionCommand> current;
        if (op == "destroy" || op == "remove_objects") {
            for (const auto &id : operation.at("object_ids")) {
                current.push_back(makeRemoveObjectCommand(
                    AuthoringObjectId{exactUnsigned(id, "journal object_id", true)}));
            }
        } else if (op == "restore_objects") {
            for (const auto &closure : operation.at("closures")) {
                current.push_back(makeRestoreObjectCommand(closureFromJson(closure)));
            }
        } else {
            current.push_back(commandFromCanonical(operation,
                                                   local->projectionDocument()));
        }
        EditorProjectionTransaction transaction{
            *local, local->projectionDocument().revision()};
        std::vector<EditorProjectionAdapter *> adapters;
        const auto preflight = transaction.commit(current, adapters);
        if (!preflight.committed()) throw projectionFailure(preflight, op);
        if (unpack || restore) {
            const auto object_id = AuthoringObjectId{exactUnsigned(
                operation.at("object"), "journal unpack object", true)};
            const auto scene_id = requireString(operation, "scene_id", op);
            const auto &witness = operation.at("dependency_witness");
            const auto instance_id = requireString(
                witness, "instance_id", "unpack dependency witness");
            const auto authority = unpackAuthorityFromJson(
                operation.at("expanded_components"));
            const auto transition = originTransitionMapFromJson(
                operation.at("origin_transition"));
            const auto expected_direction =
                unpack ? OriginTransitionDirection::GeneratedToAuthored
                       : OriginTransitionDirection::AuthoredToGenerated;
            if (transition.direction != expected_direction) {
                editFailure(EditorEditErrorCode::journal_integrity_error,
                            {{"reason", "origin_transition_direction"},
                             {"object", object_id.value}},
                            "unpack transition direction does not match operation");
            }
            try {
                if (unpack) {
                    validateOriginTransitionMap(
                        transition, *base_resolved,
                        local->projectionDocument(),
                        local->projectionResolvedScene(), scene_id,
                        object_id, instance_id, authority);
                } else {
                    validateOriginTransitionMap(
                        transition, local->projectionResolvedScene(),
                        *base_document, *base_resolved, scene_id, object_id,
                        instance_id, authority);
                }
            } catch (const OriginTransitionError &error) {
                throwOriginTransitionFailure(error, object_id);
            }
        }
        result.insert(result.end(), current.begin(), current.end());
    }
    return result;
}

OrderedJson writerJson(const EditorLastWriterStamp &stamp) {
    return {{"revision", stamp.revision.value},
            {"transaction_id", stamp.transaction_id},
            {"actor_id", stamp.actor_id.value}};
}

OrderedJson editErrorJson(const EditFailure &failure) {
    OrderedJson result{{"code", failure.code_name.empty()
                                    ? std::string{editorEditErrorCodeName(failure.code)}
                                    : failure.code_name},
                       {"message", failure.what()},
                       {"payload", failure.payload}};
    return result;
}

OrderedJson targetState(const Json &operations,
                        const AuthoringSceneDocument &document) {
    OrderedJson result = OrderedJson::object();
    if (!operations.is_array() || operations.empty() || !operations.front().is_object()) {
        return result;
    }
    const auto &operation = operations.front();
    auto id = operation.find("object_id");
    if (id == operation.end()) id = operation.find("object");
    if (id != operation.end()) {
        const auto object_id = AuthoringObjectId{exactUnsigned(*id, "edit object_id", true)};
        result["object_id"] = object_id.value;
        try {
            const auto object = requireObjectLocation(document, object_id);
            const auto op = operation.value("op", std::string{});
            const bool behavior_target =
                operation.value("component_slot", std::string{}) ==
                "behavior";
            result["exists"] = true;
            result["scene_id"] = object.scene_id;
            if (!behavior_target) {
                result["declaration_index"] = object.object_index;
            }
            result["name"] = object.name ? OrderedJson(*object.name) : OrderedJson(nullptr);
            result["parent"] = object.parent ? OrderedJson(*object.parent)
                                              : OrderedJson(nullptr);
            if (op == "set_prefab_parameter" ||
                op == "unset_prefab_parameter") {
                const auto parameter = operation.at("parameter").get<std::string>();
                result["parameter"] = parameter;
                bool overridden = false;
                if (const auto prefab = object.authored.find("prefab");
                    prefab != object.authored.end() && prefab->is_object()) {
                    if (const auto parameters = prefab->find("parameters");
                        parameters != prefab->end() &&
                        parameters->is_object()) {
                        if (const auto value = parameters->find(parameter);
                            value != parameters->end()) {
                            overridden = true;
                            result["value_authored"] = *value;
                        }
                    }
                }
                result["source"] = overridden ? "override" : "default";
            }
            if (op == "unpack_prefab" || op == "restore_prefab") {
                result["prefab"] = object.authored.contains("prefab")
                                       ? OrderedJson(object.authored.at("prefab"))
                                       : OrderedJson(nullptr);
                result["components"] = object.authored.at("components");
            }
            if (op == "spawn") result["authored_object"] = object.authored;
            if (op == "reparent" && hasComponent(object, "transform")) {
                result["authored_transform"] =
                    requireComponent(object, "transform").second;
            }
            if (const auto slot = operation.find("component_slot");
                slot != operation.end() && slot->is_string()) {
                const auto name = slot->get<std::string>();
                result["component_slot"] = name;
                if (operation.contains("attachment_index")) {
                    const auto index = static_cast<std::size_t>(exactUnsigned(
                        operation.at("attachment_index"),
                        "target component attachment_index"));
                    const auto &components = object.authored.at("components");
                    const bool exists = index < components.size() &&
                        components.at(index).value("name", std::string{}) == name;
                    result["attachment_index"] = index;
                    if (operation.contains("attachment_identity")) {
                        result["attachment_identity"] =
                            operation.at("attachment_identity");
                    }
                    result["component_exists"] = exists;
                    if (exists) result["authored_component"] = components.at(index);
                } else {
                    result["component_exists"] = hasComponent(object, name);
                    if (result["component_exists"].get<bool>()) {
                        result["authored_component"] = requireComponent(object, name).second;
                    }
                }
            }
        } catch (const EditFailure &) {
            result["exists"] = false;
        }
    } else if (operation.contains("closures") &&
               operation.at("closures").is_array()) {
        result["objects"] = OrderedJson::array();
        for (const auto &closure : operation.at("closures")) {
            const auto object_id = AuthoringObjectId{exactUnsigned(
                closure.at("authoring_object_id"), "edit object_id", true)};
            OrderedJson entry{{"object_id", object_id.value}};
            try {
                const auto object = requireObjectLocation(document, object_id);
                entry["exists"] = true;
                entry["scene_id"] = object.scene_id;
                entry["declaration_index"] = object.object_index;
                entry["authored_object"] = object.authored;
            } catch (const EditFailure &) {
                entry["exists"] = false;
            }
            result["objects"].push_back(std::move(entry));
        }
    } else if (operation.contains("object_ids") &&
               operation.at("object_ids").is_array()) {
        result["objects"] = OrderedJson::array();
        for (const auto &raw_id : operation.at("object_ids")) {
            const auto object_id = AuthoringObjectId{
                exactUnsigned(raw_id, "edit object_id", true)};
            OrderedJson entry{{"object_id", object_id.value}};
            try {
                const auto object = requireObjectLocation(document, object_id);
                entry["exists"] = true;
                entry["scene_id"] = object.scene_id;
                entry["declaration_index"] = object.object_index;
                entry["authored_object"] = object.authored;
            } catch (const EditFailure &) {
                entry["exists"] = false;
            }
            result["objects"].push_back(std::move(entry));
        }
    } else if (operation.value("op", std::string{}) == "spawn") {
        result["name"] = operation.at("object").value("name", std::string{});
        const auto object_id = objectIdByName(
            document, operation.value("scene_id", std::string{}),
            operation.at("object").value("name", std::string{}));
        result["exists"] = object_id.has_value();
        if (object_id) {
            const auto object = requireObjectLocation(document, *object_id);
            result["object_id"] = object_id->value;
            result["declaration_index"] = object.object_index;
            result["authored_object"] = object.authored;
        }
    }
    return result;
}

std::string randomSessionToken() {
    std::random_device source;
    std::mt19937_64 random{(static_cast<std::uint64_t>(source()) << 32U) ^
                           static_cast<std::uint64_t>(source())};
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "editor-reconnect-";
    for (int word = 0; word < 2; ++word) {
        auto value = random();
        for (int index = 0; index < 16; ++index) {
            result.push_back(hex[value & 0xFU]);
            value >>= 4U;
        }
    }
    return result;
}

std::vector<std::string> batchWriteSet(const PreparedBatch &batch) {
    std::vector<std::string> result;
    for (const auto &operation : batch.operations) {
        result.insert(result.end(), operation.write_set.begin(),
                      operation.write_set.end());
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<OrderedJson> batchStructuralDomains(const PreparedBatch &batch) {
    std::vector<OrderedJson> result;
    result.reserve(batch.operations.size());
    for (const auto &operation : batch.operations) {
        result.push_back(operation.structural_domain);
    }
    return result;
}

bool stablePathsOverlap(std::string_view left, std::string_view right) {
    if (left == right) return true;
    const auto is_prefix = [](std::string_view prefix, std::string_view value) {
        return value.size() > prefix.size() && value.starts_with(prefix) &&
               value[prefix.size()] == '/';
    };
    return is_prefix(left, right) || is_prefix(right, left);
}

std::vector<std::uint64_t> domainObjects(const Json &domain) {
    std::vector<std::uint64_t> result;
    for (const auto *field : {"object", "root", "child", "old_parent",
                              "new_parent"}) {
        const auto found = domain.find(field);
        if (found != domain.end() && found->is_number_unsigned()) {
            result.push_back(found->get<std::uint64_t>());
        }
    }
    for (const auto *field : {"objects", "descendants"}) {
        const auto found = domain.find(field);
        if (found == domain.end() || !found->is_array()) continue;
        for (const auto &value : *found) {
            if (value.is_number_unsigned()) {
                result.push_back(value.get<std::uint64_t>());
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::string> domainNames(const Json &domain) {
    std::vector<std::string> result;
    if (const auto found = domain.find("name_reservation");
        found != domain.end() && found->is_string()) {
        result.push_back(found->get<std::string>());
    }
    if (const auto found = domain.find("name_reservations");
        found != domain.end() && found->is_array()) {
        for (const auto &value : *found) {
            if (value.is_string()) result.push_back(value.get<std::string>());
        }
    }
    return result;
}

std::vector<std::uint64_t> domainIndices(const Json &domain) {
    std::vector<std::uint64_t> result;
    if (const auto found = domain.find("declaration_index");
        found != domain.end() && found->is_number_unsigned()) {
        result.push_back(found->get<std::uint64_t>());
    }
    if (const auto found = domain.find("declaration_indices");
        found != domain.end() && found->is_array()) {
        for (const auto &value : *found) {
            if (value.is_number_unsigned()) {
                result.push_back(value.get<std::uint64_t>());
            }
        }
    }
    return result;
}

template <class Value>
bool intersects(const std::vector<Value> &left, const std::vector<Value> &right) {
    return std::any_of(left.begin(), left.end(), [&](const auto &value) {
        return std::find(right.begin(), right.end(), value) != right.end();
    });
}

bool structuralDomainsOverlap(const Json &left, const Json &right) {
    const auto left_kind = left.value("kind", std::string{});
    const auto right_kind = right.value("kind", std::string{});
    const auto left_objects = domainObjects(left);
    const auto right_objects = domainObjects(right);
    const auto same_object = [&] {
        return left.value("object", std::uint64_t{}) != 0 &&
               left.value("object", std::uint64_t{}) ==
                   right.value("object", std::uint64_t{});
    };

    // Fine-grained domains decide overlap before any object-wide domain does.
    // In particular, two list attachments on one object are independent.
    if (left_kind == "component_list_row" &&
        right_kind == "component_list_row") {
        return same_object() &&
               left.value("list_path", std::string{}) ==
                   right.value("list_path", std::string{}) &&
               left.contains("attachment") && right.contains("attachment") &&
               left.at("attachment") == right.at("attachment");
    }

    const auto component_value_domain = [](std::string_view kind) {
        return kind == "component_slot" || kind == "value_field";
    };
    if (component_value_domain(left_kind) &&
        component_value_domain(right_kind)) {
        if (left_kind == "value_field" && right_kind == "value_field" &&
            left.contains("attachment_index") &&
            right.contains("attachment_index") &&
            !left.at("attachment_index").is_null() &&
            !right.at("attachment_index").is_null() &&
            left.at("attachment_index") != right.at("attachment_index")) {
            return false;
        }
        return same_object() &&
               left.value("slot", std::string{}) ==
                   right.value("slot", std::string{});
    }
    if ((left_kind == "component_list_row" &&
         component_value_domain(right_kind)) ||
        (right_kind == "component_list_row" &&
         component_value_domain(left_kind))) {
        const auto &list = left_kind == "component_list_row" ? left : right;
        const auto &component = left_kind == "component_list_row" ? right : left;
        return same_object() && list.contains("attachment") &&
               list.at("attachment").value("type", std::string{}) ==
                   component.value("slot", std::string{});
    }

    if (left_kind == "prefab_parameter" && right_kind == "prefab_parameter") {
        return same_object() &&
               left.value("parameter", std::string{}) ==
                   right.value("parameter", std::string{});
    }
    if (left_kind == "behavior_params" && right_kind == "behavior_params") {
        return same_object() &&
               left.value("attachment_index", std::uint64_t{}) ==
                   right.value("attachment_index", std::uint64_t{});
    }
    const auto behavior_value_domain = [](const Json &domain, std::string_view kind) {
        return kind == "behavior_params" ||
               (kind == "component_list_row" &&
                domain.value("slot", std::string{}) == "behavior");
    };
    if ((left_kind == "behavior_attachment" &&
         right_kind == "behavior_attachment") ||
        (left_kind == "behavior_attachment" &&
         behavior_value_domain(right, right_kind)) ||
        (right_kind == "behavior_attachment" &&
         behavior_value_domain(left, left_kind))) {
        // Adding or removing an attachment changes the authored occurrence
        // sequence, so every behavior attachment on that object conflicts.
        return same_object();
    }

    const auto object_wide = [](std::string_view kind) {
        return kind == "object_subtree" || kind == "parent_edge";
    };
    if ((object_wide(left_kind) || object_wide(right_kind)) &&
        intersects(left_objects, right_objects)) {
        return true;
    }

    const auto left_scene = left.value("scene_id", std::string{});
    const auto right_scene = right.value("scene_id", std::string{});
    if (!left_scene.empty() && left_scene == right_scene) {
        if (intersects(domainNames(left), domainNames(right))) return true;
        if (intersects(domainIndices(left), domainIndices(right))) return true;
    }

    return false;
}

bool recordOverlaps(const EditorJournalRecord &record,
                    std::span<const std::string> write_set,
                    std::span<const OrderedJson> domains) {
    for (const auto &left : record.write_set) {
        for (const auto &right : write_set) {
            if (stablePathsOverlap(left, right)) return true;
        }
    }
    for (const auto &left : record.structural_domain) {
        for (const auto &right : domains) {
            if (structuralDomainsOverlap(left, right)) return true;
        }
    }
    return false;
}

void reconcilePostconditionHighWater(Json &observed, Json &expected,
                                     AuthoringObjectId object_id) {
    if (observed.contains("object_id")) {
        object_id = AuthoringObjectId{observed.at("object_id").get<std::uint64_t>()};
    }
    const auto reconcile_component = [object_id](Json &left, Json &right) {
        const auto name = left.value("name", std::string{});
        if (name != right.value("name", std::string{}) ||
            left.value("type", Json{}) != right.value("type", Json{})) return;
        // Compare allocator state at a common high water. Replay may recreate
        // an attachment from an older snapshot; row IDs and values still match.
        validateRawComponentTransition(left, right, object_id, name, true);
        validateRawComponentTransition(right, left, object_id, name, true);
    };
    if (observed.contains("authored_component") && expected.contains("authored_component")) {
        reconcile_component(observed.at("authored_component"), expected.at("authored_component"));
    }
    if (observed.contains("components") && expected.contains("components")) {
        auto &left = observed.at("components");
        auto &right = expected.at("components");
        if (left.size() == right.size()) {
            for (std::size_t i = 0; i < left.size(); ++i) reconcile_component(left.at(i), right.at(i));
        }
    }
    if (observed.contains("authored_object") && expected.contains("authored_object")) {
        reconcilePostconditionHighWater(observed.at("authored_object"),
                                       expected.at("authored_object"), object_id);
    }
    if (observed.contains("objects") && expected.contains("objects")) {
        auto &left = observed.at("objects");
        auto &right = expected.at("objects");
        if (left.size() == right.size()) {
            for (std::size_t i = 0; i < left.size(); ++i) {
                reconcilePostconditionHighWater(left.at(i), right.at(i), object_id);
            }
        }
    }
}

bool postconditionsHold(const EditorJournalRecord &record,
                        const AuthoringSceneDocument &document) {
    if (record.commands.size() != record.forward_postconditions.size()) return false;
    for (const auto &command : record.commands) {
        Json observed = targetState(Json::array({command.forward}), document);
        Json expected = command.forward_postcondition;
        reconcilePostconditionHighWater(observed, expected, AuthoringObjectId{
            command.forward.value("object_id", std::uint64_t{})});
        if (observed != expected) {
            return false;
        }
    }
    return true;
}

void requireLivePreviewCapability(const PreparedBatch &batch,
                                  std::string_view method) {
    // Semantic eligibility belongs to planEditorProjection.  This boundary
    // only protects the prepared-batch structural invariant so preview and
    // committed execution share one route matrix.
    for (const auto &operation : batch.operations) {
        if (!operation.forward.is_object() ||
            !operation.forward.contains("op") ||
            !operation.forward.at("op").is_string()) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", method}, {"field", "op"}},
                        "live preview operation is not structurally canonical");
        }
    }
}

bool sameStableSet(std::span<const std::string> left,
                   std::span<const std::string> right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

void deduplicate(std::vector<std::string> &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

EditorPreviewAbortError::EditorPreviewAbortError(std::string detail)
    : std::runtime_error{std::string{codeName()} + ": " +
                         std::move(detail)} {}

std::optional<std::string> unpackJournalIntegrityFailure(
    const OrderedJson &forward, const OrderedJson &inverse) {
    return unpackJournalIntegrityFailureImpl(forward, inverse);
}

EditorMutationRequestAst parseEditorMutationRequest(
    const Json &params, std::string_view method) {
    const auto context = std::string{method};
    EditorMutationRequestAst result;
    if (method == "edit") {
        requireOnly(params,
                    {"actor_id", "base_revision", "scene_revision",
                     "closure_generation", "provider_generation",
                     "operations", "coalesce_key"},
                    context);
        result.actor_id = EditorActorId{
            exactUnsigned(params.at("actor_id"), "edit actor_id", true)};
        const bool has_base = params.contains("base_revision");
        const bool has_scene = params.contains("scene_revision");
        if (has_base == has_scene) {
            throw std::invalid_argument(
                "edit requires exactly one of base_revision and scene_revision");
        }
        const auto revision_field = has_scene ? "scene_revision" : "base_revision";
        result.base_revision = SceneRevision{exactUnsigned(
            params.at(revision_field), "edit " + std::string{revision_field})};
        if (const auto found = params.find("closure_generation");
            found != params.end()) {
            result.closure_generation = exactUnsigned(
                *found, "edit closure_generation", true);
        }
        if (const auto found = params.find("provider_generation");
            found != params.end()) {
            result.provider_generation = exactUnsigned(
                *found, "edit provider_generation", true);
        }
        result.coalesce_key = optionalString(params, "coalesce_key", context);
    } else if (method == "open_preview") {
        requireOnly(params, {"actor_id", "operations"}, context);
        result.actor_id = EditorActorId{exactUnsigned(
            params.at("actor_id"), "open_preview actor_id", true)};
    } else if (method == "update_preview") {
        requireOnly(params, {"actor_id", "ticket", "operations"},
                    context);
        result.actor_id = EditorActorId{exactUnsigned(
            params.at("actor_id"), "update_preview actor_id", true)};
        result.ticket = requireString(params, "ticket", context);
    } else {
        throw std::invalid_argument(
            "generated selector parser received an unsupported method");
    }

    const auto operations = params.find("operations");
    if (operations == params.end() || !operations->is_array() ||
        operations->empty()) {
        throw std::invalid_argument(context +
                                    " operations must be a non-empty array");
    }
    result.operations = *operations;
    for (const auto &operation : *operations) {
        requireObject(operation, "edit operation");
        const auto op = requireString(operation, "op", "edit operation");
        if (op == "set_prefab_parameter" ||
            op == "unset_prefab_parameter") {
            result.prefab_mutation = true;
            if (op == "set_prefab_parameter") {
                requireOnly(operation,
                            {"op", "object", "parameter", "value"}, op);
                if (!operation.contains("value")) {
                    throw std::invalid_argument(
                        "set_prefab_parameter value is required");
                }
            } else {
                requireOnly(operation, {"op", "object", "parameter"}, op);
            }
            (void)exactUnsigned(operation.at("object"),
                                op + " object", true);
            (void)requireString(operation, "parameter", op);
            continue;
        }
        if (op == "unpack_prefab") {
            result.prefab_mutation = true;
            requireOnly(operation, {"op", "object"}, op);
            (void)exactUnsigned(operation.at("object"),
                                "unpack_prefab object", true);
            continue;
        }
        if (op != "set_component_value") continue;
        if (operation.contains("address")) {
            requireOnly(operation, {"op", "address", "value"},
                        "set_component_value addressed selector");
            if (!operation.contains("value")) {
                throw std::invalid_argument(
                    "set_component_value value is required");
            }
            continue;
        }
        const bool has_slot = operation.contains("component_slot");
        const bool has_generated = operation.contains("generated_id");
        if (has_slot == has_generated) {
            throw std::invalid_argument(
                "set_component_value requires exactly one of component_slot and generated_id");
        }
        const auto object_id = AuthoringObjectId{exactUnsigned(
            operation.at("object_id"), "set_component_value object_id",
            true)};
        const auto field_path = requireString(
            operation, "field_path", "set_component_value");
        if (!field_path.starts_with('/')) {
            throw std::invalid_argument(
                "set_component_value field_path must be an RFC6901 JSON pointer");
        }
        try {
            (void)nlohmann::json::json_pointer{field_path};
        } catch (const nlohmann::json::exception &) {
            throw std::invalid_argument(
                "set_component_value field_path must be an RFC6901 JSON pointer");
        }
        if (!operation.contains("value")) {
            throw std::invalid_argument(
                "set_component_value value is required");
        }
        if (!has_generated) {
            (void)requireString(operation, "component_slot",
                                "set_component_value");
            continue;
        }
        requireOnly(operation,
                    {"op", "object_id", "generated_id", "field_path",
                     "value"},
                    "set_component_value generated selector");
        EditorGeneratedSelectorAst selector;
        selector.object_id = object_id;
        selector.generated_id = requireString(
            operation, "generated_id", "set_component_value");
        selector.field_path = field_path;
        result.generated_selectors.push_back(std::move(selector));
    }
    if (method == "edit" && result.prefab_mutation) {
        if (!params.contains("scene_revision")) {
            throw std::invalid_argument(
                "prefab mutations require scene_revision");
        }
        if (!result.closure_generation || !result.provider_generation) {
            throw std::invalid_argument(
                "prefab mutations require closure_generation and provider_generation");
        }
    }
    return result;
}

bool operationsUsePrefabParameters(
    std::span<const OrderedJson> operations) {
    return std::any_of(operations.begin(), operations.end(), [](const auto &op) {
        const auto name = op.value("op", std::string{});
        return name == "set_prefab_parameter" ||
               name == "unset_prefab_parameter" ||
               name == "unpack_prefab" || name == "restore_prefab";
    });
}

std::string_view editorEditErrorCodeName(EditorEditErrorCode code) noexcept {
    switch (code) {
    case EditorEditErrorCode::stale_revision: return "stale_revision";
    case EditorEditErrorCode::gate_closed: return "gate_closed";
    case EditorEditErrorCode::preview_lease_conflict: return "preview_lease_conflict";
    case EditorEditErrorCode::preview_lease_busy: return "preview_lease_busy";
    case EditorEditErrorCode::not_lease_owner: return "not_lease_owner";
    case EditorEditErrorCode::ticket_not_found: return "ticket_not_found";
    case EditorEditErrorCode::preview_unsupported:
        return "editor_preview_unsupported@1";
    case EditorEditErrorCode::undo_conflict: return "undo_conflict";
    case EditorEditErrorCode::not_editable: return "not_editable";
    case EditorEditErrorCode::schema_violation: return "schema_violation";
    case EditorEditErrorCode::row_envelope_missing:
        return "RowEnvelopeMissing";
    case EditorEditErrorCode::row_envelope_unsupported:
        return "RowEnvelopeUnsupported";
    case EditorEditErrorCode::row_id_exhausted: return "RowIdExhausted";
    case EditorEditErrorCode::list_container_missing:
        return "ListContainerMissing";
    case EditorEditErrorCode::list_index_addressing_rejected:
        return "ListIndexAddressingRejected";
    case EditorEditErrorCode::prefab_instance_id_collision:
        return "prefab_instance_id_collision";
    case EditorEditErrorCode::prefab_dependency_mismatch:
        return "prefab_dependency_mismatch";
    case EditorEditErrorCode::prefab_provider_stale:
        return "prefab_provider_stale";
    case EditorEditErrorCode::prefab_parameter_required:
        return "prefab_parameter_required";
    case EditorEditErrorCode::prefab_parameter_unknown:
        return "prefab_parameter_unknown";
    case EditorEditErrorCode::prefab_parameter_type:
        return "prefab_parameter_type";
    case EditorEditErrorCode::unpack_must_be_singleton:
        return "unpack_must_be_singleton@1";
    case EditorEditErrorCode::journal_integrity_error:
        return "journal_integrity_error@1";
    case EditorEditErrorCode::prefab_digest_changed:
        return "prefab_digest_changed@1";
    case EditorEditErrorCode::prefab_provider_changed:
        return "prefab_provider_changed@1";
    case EditorEditErrorCode::prefab_behavior_dependency_changed:
        return "prefab_behavior_dependency_changed@1";
    case EditorEditErrorCode::origin_transition_target_mismatch:
        return "origin_transition_target_mismatch@1";
    case EditorEditErrorCode::origin_transition_gid_set_mismatch:
        return "origin_transition_gid_set_mismatch@1";
    case EditorEditErrorCode::origin_transition_index_set_mismatch:
        return "origin_transition_index_set_mismatch@1";
    case EditorEditErrorCode::origin_transition_index_invalid:
        return "origin_transition_index_invalid@1";
    case EditorEditErrorCode::origin_transition_component_mismatch:
        return "origin_transition_component_mismatch@1";
    case EditorEditErrorCode::origin_transition_component_key_mismatch:
        return "origin_transition_component_key_mismatch@1";
    case EditorEditErrorCode::origin_transition_gid_derivation_mismatch:
        return "origin_transition_gid_derivation_mismatch@1";
    case EditorEditErrorCode::origin_transition_body_mismatch:
        return "origin_transition_body_mismatch@1";
    case EditorEditErrorCode::behavior_type_unknown:
        return "behavior_type_unknown@1";
    case EditorEditErrorCode::runtime_binding_missing:
        return "runtime_binding_missing@1";
    case EditorEditErrorCode::component_unknown: return componentUnknownErrorCode;
    case EditorEditErrorCode::duplicate_component: return "duplicate_component";
    case EditorEditErrorCode::missing_component: return "missing_component";
    case EditorEditErrorCode::name_conflict: return "name_conflict";
    case EditorEditErrorCode::parent_not_found: return "parent_not_found";
    case EditorEditErrorCode::closure_unresolvable: return "closure_unresolvable";
    case EditorEditErrorCode::cycle_detected: return "cycle_detected";
    case EditorEditErrorCode::zero_scale: return "zero_scale";
    case EditorEditErrorCode::non_finite_transform: return "non_finite_transform";
    case EditorEditErrorCode::trs_unrepresentable: return "trs_unrepresentable";
    case EditorEditErrorCode::preserve_missing: return "preserve_missing";
    case EditorEditErrorCode::method_unavailable: return "method_unavailable";
    case EditorEditErrorCode::module_unavailable: return "module_unavailable@1";
    }
    return "method_unavailable";
}

OrderedJson editorJournalJson(const EditorJournalRecord &record) {
    OrderedJson commands = OrderedJson::array();
    for (const auto &command : record.commands) {
        commands.push_back({{"forward", command.forward},
                            {"inverse", command.inverse},
                            {"stable_target", command.stable_target},
                            {"read_set", command.read_set},
                            {"write_set", command.write_set},
                            {"structural_domain", command.structural_domain},
                            {"forward_postcondition", command.forward_postcondition},
                            {"last_writer", writerJson(command.last_writer)}});
    }
    OrderedJson affected = OrderedJson::array();
    for (const auto id : record.affected_authoring_ids) affected.push_back(id.value);
    OrderedJson result{
        {"transaction_id", record.transaction_id},
        {"actor_id", record.actor_id.value},
        {"actor_display_name", record.actor_display_name},
        {"base_revision", record.base_revision.value},
        {"committed_revision", record.committed_revision.value},
        {"ordered_forward", record.ordered_forward},
        {"ordered_inverse", record.ordered_inverse},
        {"affected_authoring_ids", affected},
        {"status", record.status},
        {"operation_kind", record.operation_kind},
        {"stable_targets", record.stable_targets},
        {"read_set", record.read_set},
        {"write_set", record.write_set},
        {"structural_domain", record.structural_domain},
        {"forward_postconditions", record.forward_postconditions},
        {"commands", std::move(commands)},
    };
    result["coalesce_key"] = record.coalesce_key
                                  ? OrderedJson(*record.coalesce_key)
                                  : OrderedJson(nullptr);
    result["source_transaction_id"] = record.source_transaction_id
                                                ? OrderedJson(*record.source_transaction_id)
                                                : OrderedJson(nullptr);
    return result;
}

struct EditorEditCoordinator::Impl {
    struct Session {
        EditorActorId actor_id{};
        std::string display_name;
        std::string reconnect_token;
    };
    struct Ticket {
        enum class Kind : std::uint8_t { edit, undo, redo };

        std::string id;
        Kind kind = Kind::edit;
        EditorActorId actor_id{};
        SceneRevision base_revision{};
        Json raw_operations;
        std::optional<std::string> source_transaction_id;
        std::optional<std::string> coalesce_key;
        std::optional<std::uint64_t> expected_closure_generation;
        std::optional<std::uint64_t> expected_provider_generation;
        bool prefab_cas = false;
        std::optional<OrderedJson> accepted_unpack_forward;
        std::optional<OrderedJson> accepted_unpack_inverse;
        std::uint64_t accepted_gate_epoch = 0;
        std::string accepted_scene_id;
        OrderedJson result;
    };
    struct RedoEntry {
        std::string source_transaction_id;
        std::string undo_transaction_id;
    };
    struct PreviewLease {
        std::string ticket;
        EditorActorId actor_id{};
        SceneRevision base_revision{};
        Json raw_operations;
        std::vector<EditorCanonicalOperation> canonical_operations;
        std::vector<std::string> write_set;
        std::vector<OrderedJson> structural_domain;
        std::vector<AuthoringObjectId> affected;
        std::string scene_id;
    };
    struct PreviewTombstone {
        EditorActorId actor_id{};
        std::string final_status;
    };
    struct PreviewRequest {
        enum class Kind : std::uint8_t { open, update, commit, abort };

        std::string request_id;
        std::string ticket;
        Kind kind = Kind::open;
        EditorActorId actor_id{};
        Json raw_operations;
        std::uint64_t accepted_gate_epoch = 0;
        std::string accepted_scene_id;
        OrderedJson result;
    };
    // The live-preview lease and everything that hangs off it: the open lease,
    // the reservation held between acceptance and the frame boundary, the
    // request queue, the published request results, the tombstones that answer
    // a request naming a ticket that is already gone, and the epoch counter.
    // Kept together so a change to the preview state machine cannot silently
    // reach the ticket queue or the undo/redo stacks beside it.
    struct PreviewLeaseState {
        std::optional<PreviewLease> lease;
        std::optional<PreviewLease> reservation;
        std::vector<PreviewRequest> pending;
        std::unordered_map<std::string, OrderedJson> results;
        std::unordered_map<std::string, PreviewTombstone> tombstones;
        std::uint64_t next_ticket = 1;
        std::uint64_t next_request = 1;
        std::uint64_t epoch = 0;
    };

    EditorEditRuntimeDependencies dependencies;
    std::unordered_map<std::uint64_t, Session> sessions;
    std::unordered_map<std::string, std::uint64_t> reconnect_tokens;
    std::optional<EditorActorId> bound_actor;
    std::uint64_t next_actor = 1;
    std::uint64_t next_ticket = 1;
    std::uint64_t next_transaction = 1;
    std::vector<Ticket> pending;
    std::unordered_map<std::string, OrderedJson> results;
    std::vector<OrderedJson> completed;
    std::vector<EditorJournalRecord> journal;
    // Active writer history, not journal-action history. Undo removes the
    // reverted writer; redo installs its new transaction as the active top.
    std::unordered_map<std::string, std::vector<EditorLastWriterStamp>>
        last_writers;
    std::unordered_map<std::uint64_t, std::vector<std::string>> undo_stacks;
    std::unordered_map<std::uint64_t, std::vector<RedoEntry>> redo_stacks;
    PreviewLeaseState preview;
    EditorGateEpoch<EditorGateObservation> gate_tracker;
    bool hook_installed = false;

    explicit Impl(EditorEditRuntimeDependencies runtime)
        : dependencies{std::move(runtime)} {}

    const AuthoringSceneDocument &document() const { return dependencies.document(); }
    const SceneProjectionState *projectionState() const {
        if (!dependencies.projection_state) return nullptr;
        const auto &state = dependencies.projection_state();
        return state.revision() == document().revision() ? &state : nullptr;
    }
    const SceneProjectionState &requireProjectionState(
        std::string_view method) const {
        const auto *state = projectionState();
        if (state == nullptr) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", method},
                         {"adapter", "projection_state"}},
                        "unpack requires the live projection state callback");
        }
        return *state;
    }
    PrefabRegistrySnapshot prefabRegistry() const {
        return dependencies.prefab_registry
                   ? dependencies.prefab_registry()
                   : PrefabRegistrySnapshot{};
    }
    BindableProviderSnapshot bindableProvider() const {
        return dependencies.bindable_provider
                   ? dependencies.bindable_provider()
                   : BindableProviderSnapshot{};
    }
    const std::function<bool(std::string_view)> &componentExists() const {
        return dependencies.component_exists;
    }
    std::uint64_t closureGeneration() const {
        return dependencies.closure_generation
                   ? dependencies.closure_generation()
                   : 0;
    }
    std::uint64_t providerGeneration() const {
        return bindableProvider().generation();
    }

    std::optional<EditFailure> prefabCasFailure(
        const Ticket &ticket) const {
        if (!ticket.prefab_cas) return std::nullopt;
        const auto closure = closureGeneration();
        if (!ticket.expected_closure_generation ||
            *ticket.expected_closure_generation != closure) {
            return EditFailure{
                EditorEditErrorCode::prefab_dependency_mismatch,
                OrderedJson{{"expected_closure_generation",
                             ticket.expected_closure_generation
                                 ? OrderedJson(*ticket.expected_closure_generation)
                                 : OrderedJson(nullptr)},
                            {"current_closure_generation", closure}},
                "prefab resolution closure is stale"};
        }
        const auto provider = providerGeneration();
        if (!ticket.expected_provider_generation ||
            *ticket.expected_provider_generation != provider) {
            return EditFailure{
                EditorEditErrorCode::prefab_provider_stale,
                OrderedJson{{"expected_provider_generation",
                             ticket.expected_provider_generation
                                 ? OrderedJson(*ticket.expected_provider_generation)
                                 : OrderedJson(nullptr)},
                            {"current_provider_generation", provider}},
                "prefab provider generation is stale"};
        }
        return std::nullopt;
    }

    EditorGateSnapshot gateSnapshot() {
        const auto observation = dependencies.gate ? dependencies.gate()
                                                   : EditorGateObservation{};
        const auto epoch = gate_tracker.observe(observation);
        return EditorGateSnapshot{.can_edit = observation.reasons == 0,
                                  .can_preview = observation.reasons == 0,
                                  .epoch = epoch,
                                  .reasons = gateReasonNames(observation.reasons)};
    }

    const Session &requireBound(EditorActorId actor) const {
        if (!bound_actor || *bound_actor != actor) {
            throw std::invalid_argument(
                "actor_id was not issued and bound to this editor connection");
        }
        return sessions.at(actor.value);
    }

    std::string allocateTicket() { return "edit-" + std::to_string(next_ticket++); }
    std::string allocatePreviewTicket() {
        return "preview-" + std::to_string(preview.next_ticket++);
    }
    std::string allocatePreviewRequest() {
        return "preview-request-" + std::to_string(preview.next_request++);
    }
    std::string allocateTransaction() {
        return "txn-" + std::to_string(next_transaction++);
    }

    OrderedJson rejected(std::string ticket, EditorActorId actor,
                         SceneRevision base, const EditFailure &failure) {
        return {{"ticket", std::move(ticket)},
                {"actor_id", actor.value},
                {"base_revision", base.value},
                {"status", "rejected"},
                {"error", editErrorJson(failure)}};
    }

    OrderedJson previewRejected(const PreviewRequest &request,
                                const EditFailure &failure,
                                std::string status = "rejected") const {
        return {{"request_id", request.request_id},
                {"ticket", request.ticket},
                {"actor_id", request.actor_id.value},
                {"status", std::move(status)},
                {"error", editErrorJson(failure)}};
    }

    // The wire name of a ticket kind.  One function instead of the same three
    // way ternary spelled out at each use.
    static constexpr const char *ticketKindName(Ticket::Kind kind) noexcept {
        return kind == Ticket::Kind::edit
                   ? "edit"
                   : kind == Ticket::Kind::undo ? "undo" : "redo";
    }

    // The only place an edit ticket outcome becomes visible: the ticket keeps
    // its own copy, `results` answers get_result, and `completed` is drained by
    // takeCompletedResults.  Every exit of commitPending goes through here.
    void publishResult(Ticket &ticket, OrderedJson result) {
        ticket.result = std::move(result);
        results[ticket.id] = ticket.result;
        completed.push_back(ticket.result);
    }

    void publishFailure(Ticket &ticket, const EditFailure &failure,
                        const char *status) {
        auto result =
            rejected(ticket.id, ticket.actor_id, ticket.base_revision, failure);
        result["status"] = status;
        publishResult(ticket, std::move(result));
    }

    // The three catch bodies at the end of commitPending differ only in how
    // they name the failure; the execution ones share one payload.
    void publishExecutionFailure(Ticket &ticket, std::string message) {
        const EditFailure failure{
            EditorEditErrorCode::method_unavailable,
            OrderedJson{{"method", "edit"}, {"adapter", "execution"}},
            std::move(message)};
        publishFailure(ticket, failure, "failed");
    }

    const EditorJournalRecord &requireJournal(std::string_view transaction_id) const {
        const auto found = std::find_if(journal.begin(), journal.end(),
                                        [&](const auto &record) {
                                            return record.transaction_id == transaction_id;
                                        });
        if (found == journal.end()) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", "undo"}, {"adapter", "journal"}},
                        "journal transaction does not exist");
        }
        return *found;
    }

    bool leaseOverlaps(const PreviewLease &lease,
                       std::span<const std::string> writes,
                       std::span<const OrderedJson> domains) const {
        for (const auto &left : lease.write_set) {
            for (const auto &right : writes) {
                if (stablePathsOverlap(left, right)) return true;
            }
        }
        for (const auto &left : lease.structural_domain) {
            for (const auto &right : domains) {
                if (structuralDomainsOverlap(left, right)) return true;
            }
        }
        return false;
    }

    const PreviewLease *conflictingLease(
        std::span<const std::string> writes,
        std::span<const OrderedJson> domains) const {
        if (preview.lease && leaseOverlaps(*preview.lease, writes, domains)) {
            return &*preview.lease;
        }
        if (preview.reservation &&
            leaseOverlaps(*preview.reservation, writes, domains)) {
            return &*preview.reservation;
        }
        return nullptr;
    }

    void requireNoLeaseOverlap(std::span<const std::string> writes,
                               std::span<const OrderedJson> domains) const {
        if (const auto *lease = conflictingLease(writes, domains)) {
            editFailure(EditorEditErrorCode::preview_lease_conflict,
                        {{"ticket", lease->ticket},
                         {"owner", lease->actor_id.value}},
                        "edit overlaps the open preview lease");
        }
    }

    [[noreturn]] void throwUndoConflict(const EditorJournalRecord &source,
                                        const OrderedJson &domain,
                                        std::string owner_transaction,
                                        SceneRevision revision) const {
        editFailure(EditorEditErrorCode::undo_conflict,
                    {{"domain", domain},
                     {"owner_txn", std::move(owner_transaction)},
                     {"revision", revision.value}},
                    "revert precondition was changed by a later transaction");
    }

    void requireRevertPreconditions(const EditorJournalRecord &source) const {
        for (const auto &candidate : journal) {
            if (candidate.committed_revision.value <=
                    source.committed_revision.value ||
                candidate.actor_id == source.actor_id) {
                continue;
            }
            if (recordOverlaps(candidate, source.write_set,
                               source.structural_domain)) {
                const auto domain = source.structural_domain.empty()
                                        ? OrderedJson(source.write_set)
                                        : source.structural_domain.front();
                throwUndoConflict(source, domain, candidate.transaction_id,
                                  candidate.committed_revision);
            }
        }
        // An undo journal record represents the absence of its source writer,
        // so redo is guarded by its postcondition and the actor redo stack.
        // Normal edits and redo records remain active writers and must be the
        // current top before they can be undone.
        if (source.operation_kind != "undo") {
            for (const auto &command : source.commands) {
                for (const auto &write : command.write_set) {
                    const auto writer = last_writers.find(write);
                    if (writer == last_writers.end() ||
                        writer->second.empty() ||
                        writer->second.back().transaction_id !=
                            source.transaction_id) {
                        const auto owner =
                            writer != last_writers.end() &&
                                    !writer->second.empty()
                                ? writer->second.back()
                                : EditorLastWriterStamp{document().revision(),
                                                        "<none>", {}};
                        throwUndoConflict(source, command.structural_domain,
                                          owner.transaction_id,
                                          owner.revision);
                    }
                }
            }
        }
        if (!postconditionsHold(source, document())) {
            const auto domain = source.structural_domain.empty()
                                    ? OrderedJson(source.write_set)
                                    : source.structural_domain.front();
            throwUndoConflict(source, domain, source.transaction_id,
                              document().revision());
        }
    }

    // JOURNAL0 record assembly, shared by the edit and the revert paths: a
    // command record also contributes to the record-level stable target,
    // read/write and structural domain sets.  Adding a field to
    // EditorJournalCommandRecord is a change here, not in two callers.
    static void appendCommandRecord(EditorJournalRecord &record,
                                    EditorJournalCommandRecord command) {
        record.stable_targets.push_back(command.stable_target);
        record.read_set.insert(record.read_set.end(), command.read_set.begin(),
                               command.read_set.end());
        record.write_set.insert(record.write_set.end(), command.write_set.begin(),
                                command.write_set.end());
        record.structural_domain.push_back(command.structural_domain);
        record.forward_postconditions.push_back(command.forward_postcondition);
        record.commands.push_back(std::move(command));
    }

    static void finalizeRecordSets(EditorJournalRecord &record) {
        deduplicate(record.stable_targets);
        deduplicate(record.read_set);
        deduplicate(record.write_set);
    }

    void finalizeJournal(Ticket &ticket, PreparedBatch batch,
                         const EditorProjectionResult &projection,
                         const std::string &transaction_id) {
        EditorJournalRecord record;
        record.transaction_id = transaction_id;
        record.actor_id = ticket.actor_id;
        record.actor_display_name = sessions.at(ticket.actor_id.value).display_name;
        record.base_revision = ticket.base_revision;
        record.committed_revision = projection.committed_revision;
        record.ordered_forward = batch.forward;
        record.ordered_inverse = batch.inverse;
        record.affected_authoring_ids = batch.affected;
        record.coalesce_key = ticket.coalesce_key;

        // A replay of spawn must restore the original AuthoringObjectId rather
        // than allocating a new one. The committed closure supplies that
        // identity and declaration interval without changing the public RPC op.
        for (auto &forward : record.ordered_forward) {
            if (forward.at("op") != "spawn") continue;
            const auto id = AuthoringObjectId{forward.at("object_id").get<std::uint64_t>()};
            forward["closure"] = closureJson(closureForObject(document(), id));
        }

        const EditorLastWriterStamp stamp{projection.committed_revision,
                                           transaction_id, ticket.actor_id};
        for (std::size_t index = 0; index < batch.operations.size(); ++index) {
            auto &prepared = batch.operations[index];
            EditorJournalCommandRecord command{
                .forward = record.ordered_forward[index],
                .inverse = prepared.inverse,
                .stable_target = prepared.stable_target,
                .read_set = prepared.read_set,
                .write_set = prepared.write_set,
                .structural_domain = prepared.structural_domain,
                .forward_postcondition = targetState(
                    Json::array({record.ordered_forward[index]}), document()),
                .last_writer = stamp,
            };
            appendCommandRecord(record, std::move(command));
        }
        finalizeRecordSets(record);
        for (const auto &write : record.write_set) {
            auto &history = last_writers[write];
            if (history.empty() ||
                history.back().transaction_id != stamp.transaction_id) {
                history.push_back(stamp);
            }
        }
        const auto committed_id = record.transaction_id;
        journal.push_back(std::move(record));
        undo_stacks[ticket.actor_id.value].push_back(committed_id);
        redo_stacks[ticket.actor_id.value].clear();
    }

    std::string finalizeRevertJournal(
        const Ticket &ticket, const EditorJournalRecord &source,
        const EditorProjectionResult &projection, std::string operation_kind) {
        EditorJournalRecord record;
        record.transaction_id = allocateTransaction();
        record.actor_id = ticket.actor_id;
        record.actor_display_name = sessions.at(ticket.actor_id.value).display_name;
        record.base_revision = ticket.base_revision;
        record.committed_revision = projection.committed_revision;
        record.ordered_forward = source.ordered_inverse;
        record.ordered_inverse = source.ordered_forward;
        record.affected_authoring_ids = source.affected_authoring_ids;
        record.operation_kind = std::move(operation_kind);
        record.source_transaction_id = source.transaction_id;

        const EditorLastWriterStamp stamp{projection.committed_revision,
                                           record.transaction_id,
                                           ticket.actor_id};
        for (std::size_t index = 0; index < record.ordered_forward.size(); ++index) {
            const auto source_index = source.commands.size() - 1U - index;
            const auto &source_command = source.commands.at(source_index);
            EditorJournalCommandRecord command{
                .forward = record.ordered_forward[index],
                .inverse = record.ordered_inverse[source_index],
                .stable_target = source_command.stable_target,
                .read_set = source_command.read_set,
                .write_set = source_command.write_set,
                .structural_domain = source_command.structural_domain,
                .forward_postcondition = targetState(
                    Json::array({record.ordered_forward[index]}), document()),
                .last_writer = stamp,
            };
            appendCommandRecord(record, std::move(command));
        }
        finalizeRecordSets(record);
        if (record.operation_kind == "undo") {
            for (const auto &write : source.write_set) {
                const auto found = last_writers.find(write);
                if (found == last_writers.end() || found->second.empty() ||
                    found->second.back().transaction_id !=
                        source.transaction_id) {
                    continue;
                }
                found->second.pop_back();
                if (found->second.empty()) last_writers.erase(found);
            }
        } else {
            for (const auto &write : record.write_set) {
                last_writers[write].push_back(stamp);
            }
        }
        const auto transaction_id = record.transaction_id;
        journal.push_back(std::move(record));
        return transaction_id;
    }

    void advancePreviewEpoch() {
        if (preview.epoch >= maxExactJsonInteger) {
            throw std::overflow_error("preview_epoch space exhausted");
        }
        ++preview.epoch;
    }

    EditFailure missingPreviewTicket(std::string_view ticket) const {
        OrderedJson payload{{"ticket", ticket}};
        if (const auto found = preview.tombstones.find(std::string{ticket});
            found != preview.tombstones.end()) {
            payload["final_status"] = found->second.final_status;
        }
        return EditFailure{EditorEditErrorCode::ticket_not_found,
                           std::move(payload),
                           "preview ticket no longer exists"};
    }

    EditorProjectionResult executeLivePreview(
        const PreviewLease &lease, std::span<const EditorProjectionCommand> commands,
        std::span<const EditorCanonicalOperation> operations, bool restore) {
        if (!dependencies.execute_preview) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", restore ? "abort_preview" : "open_preview"},
                         {"adapter", "live_preview"}},
                        "live preview execution is unavailable");
        }
        const EditorPreviewExecutionRequest request{
            .base_revision = document().revision(),
            .commands = restore ? std::span<const EditorProjectionCommand>{}
                                : commands,
            // Retain the lease operations on restore so the planner can keep
            // an inactive scene document-only instead of touching the active
            // scene's transform/light participants.
            .operations = operations,
            .restore_committed = restore,
        };
        return dependencies.execute_preview(request);
    }

    bool forceAbort(std::string reason) {
        if (!preview.lease) return false;
        try {
            const auto lease = *preview.lease;
            const auto projection = executeLivePreview(
                lease, {}, lease.canonical_operations, true);
            if (!projection.committed()) {
                throw projectionFailure(projection, "forced_abort_preview");
            }
            if (dependencies.preview_boundary) dependencies.preview_boundary();
            advancePreviewEpoch();
            preview.tombstones[lease.ticket] =
                PreviewTombstone{lease.actor_id, "forced_aborted"};
            completed.push_back({{"event", "ticket_forced_aborted"},
                                 {"ticket", lease.ticket},
                                 {"actor_id", lease.actor_id.value},
                                 {"reason", std::move(reason)},
                                 {"final_status", "forced_aborted"},
                                 {"preview_epoch", preview.epoch}});
            preview.lease.reset();
            return true;
        } catch (const EditorPreviewAbortError &) {
            throw;
        } catch (const std::exception &error) {
            throw EditorPreviewAbortError{error.what()};
        } catch (...) {
            throw EditorPreviewAbortError{"unknown preview restore failure"};
        }
    }

    void finishPreviewRequest(PreviewRequest &request, OrderedJson result) {
        request.result = std::move(result);
        preview.results[request.request_id] = request.result;
        completed.push_back(request.result);
    }

    // The preview twin of publishFailure: every rejected preview request goes
    // out through one place.
    void publishPreviewFailure(PreviewRequest &request,
                               const EditFailure &failure,
                               std::string status = "rejected") {
        finishPreviewRequest(
            request, previewRejected(request, failure, std::move(status)));
    }

    void commitPendingPreview() {
        auto queue = std::move(preview.pending);
        preview.pending.clear();
        for (auto &request : queue) {
            try {
                const auto gate = gateSnapshot();
                const bool gate_changed =
                    gate.epoch != request.accepted_gate_epoch ||
                    dependencies.current_scene_id() != request.accepted_scene_id;

                if (request.kind != PreviewRequest::Kind::abort &&
                    (!gate.can_preview || gate_changed)) {
                    const EditFailure failure{
                        EditorEditErrorCode::gate_closed,
                        {{"method", request.kind == PreviewRequest::Kind::open
                                        ? "open_preview"
                                        : request.kind == PreviewRequest::Kind::update
                                              ? "update_preview"
                                              : "commit_preview"},
                         {"reason", gate_changed ? "reload_scene_transition"
                                                  : primaryGateReason(gate)},
                         {"gate_epoch", gate.epoch}},
                        "editor preview gate changed after request acceptance"};
                    publishPreviewFailure(request, failure, "failed");
                    if (request.kind == PreviewRequest::Kind::open &&
                        preview.reservation &&
                        preview.reservation->ticket == request.ticket) {
                        preview.reservation.reset();
                    }
                    continue;
                }

                if (request.kind == PreviewRequest::Kind::open) {
                    if (preview.lease) {
                        const EditFailure failure{
                            EditorEditErrorCode::preview_lease_busy,
                            {{"owner", preview.lease->actor_id.value}},
                            "live preview lease is already occupied"};
                        publishPreviewFailure(request, failure);
                        preview.reservation.reset();
                        continue;
                    }
                    auto batch = prepareBatch(request.raw_operations, document(),
                                              request.accepted_scene_id,
                                              prefabRegistry(),
                                              bindableProvider(),
                                              projectionState(),
                                              componentExists());
                    requireLivePreviewCapability(batch, "open_preview");
                    PreviewLease lease{
                        .ticket = request.ticket,
                        .actor_id = request.actor_id,
                        .base_revision = document().revision(),
                        .raw_operations = request.raw_operations,
                        .canonical_operations = batch.canonical_forward,
                        .write_set = batchWriteSet(batch),
                        .structural_domain = batchStructuralDomains(batch),
                        .affected = batch.affected,
                        .scene_id = request.accepted_scene_id,
                    };
                    const auto projection = executeLivePreview(
                        lease, batch.commands, batch.canonical_forward, false);
                    if (!projection.committed()) {
                        throw projectionFailure(projection, "open_preview");
                    }
                    if (dependencies.preview_boundary) dependencies.preview_boundary();
                    advancePreviewEpoch();
                    preview.lease = std::move(lease);
                    preview.reservation.reset();
                    finishPreviewRequest(
                        request,
                        {{"request_id", request.request_id},
                         {"ticket", request.ticket},
                         {"actor_id", request.actor_id.value},
                         {"base_revision", preview.lease->base_revision.value},
                         {"affected_authoring_ids", [&] {
                              OrderedJson values = OrderedJson::array();
                              for (const auto id : preview.lease->affected) {
                                  values.push_back(id.value);
                              }
                              return values;
                          }()},
                         {"status", "open"},
                         {"preview_epoch", preview.epoch}});
                    continue;
                }

                if (!preview.lease || preview.lease->ticket != request.ticket) {
                    if (request.kind == PreviewRequest::Kind::abort) {
                        const auto tombstone = preview.tombstones.find(request.ticket);
                        const auto final_status =
                            tombstone == preview.tombstones.end()
                                ? std::string{"not_found"}
                                : tombstone->second.final_status;
                        finishPreviewRequest(
                            request,
                            {{"request_id", request.request_id},
                             {"ticket", request.ticket},
                             {"actor_id", request.actor_id.value},
                             {"status", "succeeded"},
                             {"final_status", final_status},
                             {"preview_epoch", preview.epoch}});
                    } else {
                        const auto failure = missingPreviewTicket(request.ticket);
                        publishPreviewFailure(request, failure);
                    }
                    continue;
                }
                if (preview.lease->actor_id != request.actor_id) {
                    const EditFailure failure{
                        EditorEditErrorCode::not_lease_owner,
                        {{"ticket", request.ticket},
                         {"owner", preview.lease->actor_id.value}},
                        "preview ticket belongs to another actor"};
                    publishPreviewFailure(request, failure);
                    continue;
                }

                if (request.kind == PreviewRequest::Kind::update) {
                    auto batch = prepareBatch(request.raw_operations, document(),
                                              request.accepted_scene_id,
                                              prefabRegistry(),
                                              bindableProvider(),
                                              projectionState(),
                                              componentExists());
                    requireLivePreviewCapability(batch, "update_preview");
                    auto writes = batchWriteSet(batch);
                    if (!sameStableSet(writes, preview.lease->write_set)) {
                        editFailure(EditorEditErrorCode::method_unavailable,
                                    {{"method", "update_preview"},
                                     {"field", "lease_write_set"}},
                                    "preview update cannot expand or replace its lease");
                    }
                    const auto projection = executeLivePreview(
                        *preview.lease, batch.commands,
                        batch.canonical_forward, false);
                    if (!projection.committed()) {
                        throw projectionFailure(projection, "update_preview");
                    }
                    preview.lease->raw_operations = request.raw_operations;
                    preview.lease->canonical_operations =
                        batch.canonical_forward;
                    preview.lease->affected = batch.affected;
                    advancePreviewEpoch();
                    finishPreviewRequest(request,
                                         {{"request_id", request.request_id},
                                          {"ticket", request.ticket},
                                          {"actor_id", request.actor_id.value},
                                          {"status", "updated"},
                                          {"preview_epoch", preview.epoch}});
                    continue;
                }

                if (request.kind == PreviewRequest::Kind::commit) {
                    if (document().revision() != preview.lease->base_revision) {
                        const EditFailure failure{
                            EditorEditErrorCode::stale_revision,
                            {{"current_revision", document().revision().value},
                             {"target", targetState(preview.lease->raw_operations,
                                                    document())}},
                            "preview ticket base SceneRevision is stale"};
                        auto result = previewRejected(request, failure);
                        // Abort before publishing: a restore failure thrown
                        // past this point would be caught below and replace
                        // this stale_revision rejection with a second result.
                        try {
                            forceAbort("stale_revision");
                        } catch (const EditorPreviewAbortError &error) {
                            result["preview_abort_error"] = error.what();
                        }
                        finishPreviewRequest(request, std::move(result));
                        continue;
                    }
                    try {
                        auto batch = prepareBatch(
                            preview.lease->raw_operations, document(),
                            preview.lease->scene_id, prefabRegistry(),
                            bindableProvider(), projectionState(),
                            componentExists());
                        const EditorEditExecutionRequest execution{
                            .base_revision = preview.lease->base_revision,
                            .commands = batch.commands,
                            .operations = batch.canonical_forward,
                        };
                        const auto projection = dependencies.execute(execution);
                        if (!projection.committed()) {
                            throw projectionFailure(projection,
                                                    "commit_preview");
                        }
                        Ticket journal_ticket{
                            .id = request.request_id,
                            .kind = Ticket::Kind::edit,
                            .actor_id = request.actor_id,
                            .base_revision = preview.lease->base_revision,
                            .raw_operations = preview.lease->raw_operations,
                        };
                        const auto transaction_id = allocateTransaction();
                        finalizeJournal(journal_ticket, std::move(batch),
                                        projection, transaction_id);
                        advancePreviewEpoch();
                        preview.tombstones[request.ticket] =
                            PreviewTombstone{request.actor_id, "committed"};
                        preview.lease.reset();
                        finishPreviewRequest(
                            request,
                            {{"request_id", request.request_id},
                             {"ticket", request.ticket},
                             {"actor_id", request.actor_id.value},
                             {"status", "committed"},
                             {"committed_revision",
                              projection.committed_revision.value},
                             {"transaction_id", transaction_id},
                             {"preview_epoch", preview.epoch}});
                    } catch (...) {
                        const auto commit_failure = std::current_exception();
                        if (preview.lease &&
                            preview.lease->ticket == request.ticket) {
                            forceAbort("commit_failed");
                        }
                        std::rethrow_exception(commit_failure);
                    }
                    continue;
                }

                const auto projection = executeLivePreview(
                    *preview.lease, {}, preview.lease->canonical_operations, true);
                if (!projection.committed()) {
                    throw projectionFailure(projection, "abort_preview");
                }
                if (dependencies.preview_boundary) dependencies.preview_boundary();
                advancePreviewEpoch();
                preview.tombstones[request.ticket] =
                    PreviewTombstone{request.actor_id, "aborted"};
                preview.lease.reset();
                finishPreviewRequest(request,
                                     {{"request_id", request.request_id},
                                      {"ticket", request.ticket},
                                      {"actor_id", request.actor_id.value},
                                      {"status", "succeeded"},
                                      {"final_status", "aborted"},
                                      {"preview_epoch", preview.epoch}});
            } catch (const EditFailure &failure) {
                publishPreviewFailure(request, failure, "failed");
                if (request.kind == PreviewRequest::Kind::open &&
                    preview.reservation &&
                    preview.reservation->ticket == request.ticket) {
                    preview.reservation.reset();
                }
            } catch (const std::exception &error) {
                const EditFailure failure{
                    EditorEditErrorCode::method_unavailable,
                    {{"method", "preview"}, {"adapter", "execution"}},
                    error.what()};
                publishPreviewFailure(request, failure, "failed");
                if (request.kind == PreviewRequest::Kind::open) {
                    preview.reservation.reset();
                }
            }
        }
    }

    static void hook(void *context) noexcept {
        static_cast<Impl *>(context)->commitPending();
    }

    void commitPending() noexcept {
        try {
            commitPendingPreview();
            const auto preview_gate = gateSnapshot();
            if (preview.lease &&
                (!preview_gate.can_preview ||
                 dependencies.current_scene_id() != preview.lease->scene_id)) {
                (void)forceAbort(preview_gate.can_preview
                                     ? "scene_transition"
                                     : primaryGateReason(preview_gate));
            }
        } catch (...) {
            // Individual preview requests are converted to stable failures by
            // commitPendingPreview. The frame boundary itself never throws.
        }
        auto queue = std::move(pending);
        pending.clear();
        for (auto &ticket : queue) {
            try {
                auto snapshot = gateSnapshot();
                if (!snapshot.can_edit || snapshot.epoch != ticket.accepted_gate_epoch) {
                    const EditFailure failure{
                        EditorEditErrorCode::gate_closed,
                        OrderedJson{{"method", ticketKindName(ticket.kind)},
                                    {"reason", primaryGateReason(snapshot)},
                                    {"gate_epoch", snapshot.epoch}},
                        "editor gate changed after request acceptance"};
                    publishFailure(ticket, failure, "failed");
                    continue;
                }
                if (dependencies.current_scene_id() != ticket.accepted_scene_id) {
                    const auto epoch = gate_tracker.advance();
                    const EditFailure failure{
                        EditorEditErrorCode::gate_closed,
                        OrderedJson{{"method", "edit"},
                                    {"reason", "reload_scene_transition"},
                                    {"gate_epoch", epoch}},
                        "scene changed after request acceptance"};
                    publishFailure(ticket, failure, "failed");
                    continue;
                }
                std::optional<EditorJournalRecord> revert_source;
                if (ticket.kind != Ticket::Kind::edit) {
                    revert_source =
                        requireJournal(*ticket.source_transaction_id);
                    if (journalUsesUnpack(*revert_source)) {
                        requireUnpackJournalRecord(
                            *revert_source,
                            requireProjectionState(
                                ticket.kind == Ticket::Kind::undo
                                    ? "undo"
                                    : "redo"));
                    }
                }
                if (ticket.accepted_unpack_forward) {
                    if (!ticket.accepted_unpack_inverse) {
                        editFailure(EditorEditErrorCode::journal_integrity_error,
                                    {{"reason", "accepted_inverse"}},
                                    "accepted unpack witness lost its inverse");
                    }
                    if (const auto failure = unpackJournalIntegrityFailureImpl(
                            *ticket.accepted_unpack_forward,
                            *ticket.accepted_unpack_inverse)) {
                        editFailure(EditorEditErrorCode::journal_integrity_error,
                                    {{"reason", *failure}},
                                    "accepted unpack witness is inconsistent");
                    }
                    requireUnpackDependencies(
                        *ticket.accepted_unpack_forward,
                        requireProjectionState("unpack_prefab"));
                }
                if (document().revision() != ticket.base_revision) {
                    const EditFailure failure{
                        EditorEditErrorCode::stale_revision,
                        OrderedJson{{"current_revision", document().revision().value},
                                    {"target", targetState(ticket.raw_operations, document())}},
                        "base SceneRevision is stale at execution"};
                    publishFailure(ticket, failure, "rejected");
                    continue;
                }
                if (ticket.kind == Ticket::Kind::edit) {
                    if (const auto failure = prefabCasFailure(ticket)) {
                        publishFailure(ticket, *failure, "failed");
                        continue;
                    }
                }

                PreparedBatch batch;
                std::vector<EditorProjectionCommand> revert_commands;
                std::vector<EditorCanonicalOperation> revert_operations;
                std::span<const EditorCanonicalOperation> execution_operations;
                std::span<const EditorProjectionCommand> execution_commands;
                if (ticket.kind == Ticket::Kind::edit) {
                    batch = prepareBatch(ticket.raw_operations, document(),
                                         ticket.accepted_scene_id,
                                         prefabRegistry(),
                                         bindableProvider(),
                                         projectionState(),
                                         componentExists());
                    const auto writes = batchWriteSet(batch);
                    const auto domains = batchStructuralDomains(batch);
                    requireNoLeaseOverlap(writes, domains);
                    if (ticket.accepted_unpack_forward &&
                        (batch.forward.size() != 1U ||
                         batch.inverse.size() != 1U ||
                         batch.forward.front() !=
                             *ticket.accepted_unpack_forward ||
                         batch.inverse.front() !=
                             *ticket.accepted_unpack_inverse)) {
                        editFailure(EditorEditErrorCode::journal_integrity_error,
                                    {{"reason", "acceptance_witness"}},
                                    "unpack witness changed between acceptance and execution");
                    }
                    execution_operations = batch.canonical_forward;
                    execution_commands = batch.commands;
                } else {
                    if (const auto failure = prefabCasFailure(ticket)) {
                        throw *failure;
                    }
                    auto &undo_stack = undo_stacks[ticket.actor_id.value];
                    auto &redo_stack = redo_stacks[ticket.actor_id.value];
                    const bool stack_matches =
                        ticket.kind == Ticket::Kind::undo
                            ? !undo_stack.empty() &&
                                  undo_stack.back() == revert_source->transaction_id
                            : !redo_stack.empty() &&
                                  redo_stack.back().undo_transaction_id ==
                                      revert_source->transaction_id;
                    if (!stack_matches) {
                        throwUndoConflict(*revert_source,
                                          revert_source->structural_domain.empty()
                                              ? OrderedJson(revert_source->write_set)
                                              : revert_source->structural_domain.front(),
                                          revert_source->transaction_id,
                                          document().revision());
                    }
                    requireRevertPreconditions(*revert_source);
                    requireNoLeaseOverlap(revert_source->write_set,
                                          revert_source->structural_domain);
                    const auto rebound_inverse =
                        rebindJournalBehaviorTargets(
                            revert_source->ordered_inverse,
                            dependencies);
                    revert_commands = commandsFromCanonical(
                        rebound_inverse, document(),
                        prefabRegistry(), bindableProvider(),
                        projectionState(), &revert_operations);
                    execution_operations = revert_operations;
                    execution_commands = revert_commands;
                }
                const EditorEditExecutionRequest request{
                    .base_revision = ticket.base_revision,
                    .commands = execution_commands,
                    .operations = execution_operations,
                };
                const auto projection = dependencies.execute(request);
                if (!projection.committed()) {
                    const auto method = ticket.kind == Ticket::Kind::edit
                                            ? (batch.forward.empty()
                                                   ? std::string{"edit"}
                                                   : batch.forward.front().at("op").get<std::string>())
                                            : std::string{ticketKindName(
                                                  ticket.kind)};
                    const auto failure = projectionFailure(projection, method);
                    publishFailure(ticket, failure,
                                   projection.status ==
                                           EditorProjectionStatus::Failed
                                       ? "failed"
                                       : "rejected");
                    continue;
                }
                std::string transaction_id;
                if (ticket.kind == Ticket::Kind::edit) {
                    transaction_id = allocateTransaction();
                    finalizeJournal(ticket, std::move(batch), projection,
                                    transaction_id);
                } else {
                    transaction_id = finalizeRevertJournal(
                        ticket, *revert_source, projection,
                        ticket.kind == Ticket::Kind::undo ? "undo" : "redo");
                    auto &undo_stack = undo_stacks[ticket.actor_id.value];
                    auto &redo_stack = redo_stacks[ticket.actor_id.value];
                    if (ticket.kind == Ticket::Kind::undo) {
                        undo_stack.pop_back();
                        redo_stack.push_back(
                            {revert_source->transaction_id, transaction_id});
                    } else {
                        redo_stack.pop_back();
                        undo_stack.push_back(transaction_id);
                    }
                }
                OrderedJson result{{"ticket", ticket.id},
                                   {"actor_id", ticket.actor_id.value},
                                   {"base_revision", ticket.base_revision.value},
                                   {"committed_revision",
                                    projection.committed_revision.value},
                                   {"transaction_id", transaction_id},
                                   {"operation", ticketKindName(ticket.kind)},
                                   {"status", "committed"}};
                if (ticket.source_transaction_id) {
                    result["source_transaction_id"] =
                        *ticket.source_transaction_id;
                }
                // The document and the journal already carry this edit, so the
                // ticket outcome is decided.  Force the stale lease down before
                // the result is published and keep a restore failure as a
                // diagnostic: letting it reach the catch below would republish
                // the very same ticket as "failed".
                if (preview.lease) {
                    try {
                        (void)forceAbort("base_revision_stale");
                    } catch (const EditorPreviewAbortError &error) {
                        result["preview_abort_error"] = error.what();
                    }
                }
                publishResult(ticket, std::move(result));
            } catch (const EditFailure &failure) {
                publishFailure(ticket, failure, "failed");
            } catch (const std::exception &error) {
                publishExecutionFailure(ticket, error.what());
            } catch (...) {
                publishExecutionFailure(ticket,
                                        "unknown editor execution failure");
            }
        }
        if (dependencies.frame_boundary_extension) {
            try {
                dependencies.frame_boundary_extension();
            } catch (...) {
                // The frame-boundary hook is noexcept.  Extension services
                // own conversion of individual requests to stable failures.
            }
        }
    }
};

EditorEditCoordinator::EditorEditCoordinator(
    EditorEditRuntimeDependencies dependencies)
    : impl_{std::make_unique<Impl>(std::move(dependencies))} {
    if (!impl_->dependencies.document || !impl_->dependencies.current_scene_id ||
        !impl_->dependencies.execute) {
        throw std::invalid_argument(
            "EditorEditCoordinator requires document, scene and execution providers");
    }
    if (impl_->dependencies.install_commit_hook) {
        if (!installEditorCommitQueueHook(impl_.get(), &Impl::hook)) {
            throw std::runtime_error("editor commit queue hook is already installed");
        }
        impl_->hook_installed = true;
    }
}

EditorEditCoordinator::~EditorEditCoordinator() {
    if (impl_ && impl_->hook_installed) removeEditorCommitQueueHook(impl_.get());
}

OrderedJson EditorEditCoordinator::openSession(const Json &params) {
    requireOnly(params, {"display_name"}, "open_editor_session");
    const auto display_name = optionalString(params, "display_name",
                                             "open_editor_session")
                                  .value_or("editor");
    if (impl_->next_actor > maxExactJsonInteger) {
        throw std::overflow_error("EditorActorId space exhausted");
    }
    const auto actor = EditorActorId{impl_->next_actor++};
    auto token = randomSessionToken();
    while (impl_->reconnect_tokens.contains(token)) token = randomSessionToken();
    impl_->sessions.emplace(actor.value,
                            Impl::Session{actor, display_name, token});
    impl_->reconnect_tokens.emplace(token, actor.value);
    impl_->bound_actor = actor;
    return {{"actor_id", actor.value},
            {"display_name", display_name},
            {"reconnect_token", token}};
}

OrderedJson EditorEditCoordinator::resumeSession(const Json &params) {
    requireOnly(params, {"reconnect_token"}, "resume_editor_session");
    const auto token = requireString(params, "reconnect_token",
                                     "resume_editor_session");
    const auto found = impl_->reconnect_tokens.find(token);
    if (found == impl_->reconnect_tokens.end()) {
        throw std::invalid_argument("resume_editor_session reconnect token is unknown");
    }
    const auto &session = impl_->sessions.at(found->second);
    impl_->bound_actor = session.actor_id;
    return {{"actor_id", session.actor_id.value},
            {"display_name", session.display_name},
            {"reconnected", true}};
}

OrderedJson EditorEditCoordinator::canEdit(const Json &params) {
    requireOnly(params, {}, "can_edit");
    const auto snapshot = impl_->gateSnapshot();
    return {{"can_edit", snapshot.can_edit},
            {"gate_epoch", snapshot.epoch},
            {"reasons", snapshot.reasons}};
}

OrderedJson EditorEditCoordinator::canPreview(const Json &params) {
    requireOnly(params, {}, "can_preview");
    const auto snapshot = impl_->gateSnapshot();
    auto reasons = snapshot.reasons;
    if (impl_->preview.lease || impl_->preview.reservation) {
        reasons.push_back("preview_lease_conflict");
    }
    return {{"can_preview", snapshot.can_preview &&
                                !impl_->preview.lease &&
                                !impl_->preview.reservation},
            {"gate_epoch", snapshot.epoch},
            {"preview_epoch", impl_->preview.epoch},
            {"reasons", reasons}};
}

OrderedJson EditorEditCoordinator::enqueue(const Json &params) {
    const auto request = parseEditorMutationRequest(params, "edit");
    const auto actor = request.actor_id;
    (void)impl_->requireBound(actor);
    const auto base = *request.base_revision;
    const auto &operations = request.operations;
    const auto &coalesce = request.coalesce_key;
    const auto ticket = impl_->allocateTicket();
    const auto gate = impl_->gateSnapshot();
    if (!gate.can_edit) {
        const EditFailure failure{
            EditorEditErrorCode::gate_closed,
            OrderedJson{{"method", "edit"},
                        {"reason", primaryGateReason(gate)},
                        {"gate_epoch", gate.epoch}},
            "editor gate is closed"};
        auto result = impl_->rejected(ticket, actor, base, failure);
        impl_->results[ticket] = result;
        return result;
    }
    if (impl_->document().revision() != base) {
        const EditFailure failure{
            EditorEditErrorCode::stale_revision,
            OrderedJson{{"current_revision", impl_->document().revision().value},
                        {"target", targetState(operations, impl_->document())}},
            "base SceneRevision is stale"};
        auto result = impl_->rejected(ticket, actor, base, failure);
        impl_->results[ticket] = result;
        return result;
    }
    if (request.prefab_mutation) {
        const auto current_closure = impl_->closureGeneration();
        if (*request.closure_generation != current_closure) {
            const EditFailure failure{
                EditorEditErrorCode::prefab_dependency_mismatch,
                OrderedJson{{"expected_closure_generation",
                             *request.closure_generation},
                            {"current_closure_generation", current_closure}},
                "prefab resolution closure is stale"};
            auto result = impl_->rejected(ticket, actor, base, failure);
            impl_->results[ticket] = result;
            return result;
        }
        const auto current_provider = impl_->providerGeneration();
        if (*request.provider_generation != current_provider) {
            const EditFailure failure{
                EditorEditErrorCode::prefab_provider_stale,
                OrderedJson{{"expected_provider_generation",
                             *request.provider_generation},
                            {"current_provider_generation", current_provider}},
                "prefab provider generation is stale"};
            auto result = impl_->rejected(ticket, actor, base, failure);
            impl_->results[ticket] = result;
            return result;
        }
    }
    Json normalized_operations;
    std::optional<OrderedJson> accepted_unpack_forward;
    std::optional<OrderedJson> accepted_unpack_inverse;
    OrderedJson inserted_rows = OrderedJson::array();
    try {
        normalized_operations = normalizeBehaviorAttachmentIdentities(
            operations, impl_->document(), base, impl_->dependencies);
        const auto batch = prepareBatch(normalized_operations, impl_->document(),
                                        impl_->dependencies.current_scene_id(),
                                        impl_->prefabRegistry(),
                                        impl_->bindableProvider(),
                                        impl_->projectionState(),
                                        impl_->componentExists());
        const auto writes = batchWriteSet(batch);
        const auto domains = batchStructuralDomains(batch);
        impl_->requireNoLeaseOverlap(writes, domains);
        if (batch.forward.size() == 1U &&
            isCanonicalUnpackOperation(batch.forward.front())) {
            (void)impl_->requireProjectionState("unpack_prefab");
            accepted_unpack_forward = batch.forward.front();
            accepted_unpack_inverse = batch.inverse.front();
        }
        for (const auto &forward : batch.forward) {
            if (forward.value("op", std::string{}) !=
                    "insert_component_field" ||
                !forward.contains("address") ||
                !forward.at("address").contains("row_id")) {
                continue;
            }
            inserted_rows.push_back(
                {{"address", forward.at("address")},
                 {"row_id", forward.at("address").at("row_id")}});
        }
    } catch (const EditFailure &failure) {
        auto result = impl_->rejected(ticket, actor, base, failure);
        impl_->results[ticket] = result;
        return result;
    }
    Impl::Ticket pending{
        .id = ticket,
        .actor_id = actor,
        .base_revision = base,
        .raw_operations = std::move(normalized_operations),
        .coalesce_key = coalesce,
        .expected_closure_generation = request.closure_generation,
        .expected_provider_generation = request.provider_generation,
        .prefab_cas = request.prefab_mutation,
        .accepted_unpack_forward = std::move(accepted_unpack_forward),
        .accepted_unpack_inverse = std::move(accepted_unpack_inverse),
        .accepted_gate_epoch = gate.epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"ticket", ticket},
                   {"actor_id", actor.value},
                   {"base_revision", base.value},
                   {"status", "accepted"}},
    };
    if (!inserted_rows.empty()) {
        pending.result["inserted_rows"] = inserted_rows;
        if (inserted_rows.size() == 1U) {
            pending.result["row_id"] = inserted_rows.front().at("row_id");
        }
    }
    impl_->results[ticket] = pending.result;
    impl_->pending.push_back(std::move(pending));
    return impl_->results.at(ticket);
}

OrderedJson EditorEditCoordinator::enqueueRevert(const Json &params,
                                                  bool redo) {
    auto &impl = *impl_;
    const auto method = redo ? "redo" : "undo";
    requireOnly(params,
                {"actor_id", "base_revision", "scene_revision",
                 "closure_generation", "provider_generation",
                 "transaction_id"},
                method);
    const auto actor = EditorActorId{exactUnsigned(
        params.at("actor_id"), std::string{method} + " actor_id", true)};
    (void)impl.requireBound(actor);
    const bool has_base = params.contains("base_revision");
    const bool has_scene = params.contains("scene_revision");
    if (has_base == has_scene) {
        throw std::invalid_argument(
            std::string{method} +
            " requires exactly one of base_revision and scene_revision");
    }
    const auto revision_field = has_scene ? "scene_revision" : "base_revision";
    const auto base = SceneRevision{exactUnsigned(
        params.at(revision_field),
        std::string{method} + " " + revision_field)};
    std::optional<std::uint64_t> expected_closure;
    std::optional<std::uint64_t> expected_provider;
    if (const auto found = params.find("closure_generation");
        found != params.end()) {
        expected_closure = exactUnsigned(
            *found, std::string{method} + " closure_generation", true);
    }
    if (const auto found = params.find("provider_generation");
        found != params.end()) {
        expected_provider = exactUnsigned(
            *found, std::string{method} + " provider_generation", true);
    }
    const auto requested = optionalString(params, "transaction_id", method);
    const auto ticket_id = impl.allocateTicket();
    const auto gate = impl.gateSnapshot();
    const auto reject = [&](const EditFailure &failure) {
        auto result = impl.rejected(ticket_id, actor, base, failure);
        result["operation"] = method;
        impl.results[ticket_id] = result;
        return result;
    };
    if (!gate.can_edit) {
        return reject(EditFailure{
            EditorEditErrorCode::gate_closed,
            {{"method", method},
             {"reason", primaryGateReason(gate)},
             {"gate_epoch", gate.epoch}},
            "editor gate is closed"});
    }
    try {
        std::string source_id;
        if (redo) {
            const auto &stack = impl.redo_stacks[actor.value];
            if (stack.empty()) {
                editFailure(EditorEditErrorCode::method_unavailable,
                            {{"method", method}, {"adapter", "journal"}},
                            "actor has no successful revert to redo");
            }
            source_id = stack.back().undo_transaction_id;
        } else {
            const auto &stack = impl.undo_stacks[actor.value];
            if (stack.empty()) {
                editFailure(EditorEditErrorCode::method_unavailable,
                            {{"method", method}, {"adapter", "journal"}},
                            "actor has no committed transaction to undo");
            }
            source_id = stack.back();
        }
        const auto &source = impl.requireJournal(source_id);
        if (journalUsesUnpack(source)) {
            requireUnpackJournalRecord(
                source, impl.requireProjectionState(method));
        }
        if (impl.document().revision() != base) {
            return reject(EditFailure{
                EditorEditErrorCode::stale_revision,
                {{"current_revision", impl.document().revision().value},
                 {"target", nullptr}},
                "base SceneRevision is stale"});
        }
        if (requested && *requested != source_id) {
            impl.throwUndoConflict(
                source,
                source.structural_domain.empty()
                    ? OrderedJson(source.write_set)
                    : source.structural_domain.front(),
                source.transaction_id, impl.document().revision());
        }
        const bool prefab_cas =
            operationsUsePrefabParameters(source.ordered_forward) ||
            operationsUsePrefabParameters(source.ordered_inverse);
        if (prefab_cas &&
            (!has_scene || !expected_closure || !expected_provider)) {
            throw std::invalid_argument(
                std::string{method} +
                " of a prefab mutation requires scene_revision, closure_generation, and provider_generation");
        }
        if (prefab_cas && *expected_closure != impl.closureGeneration()) {
            return reject(EditFailure{
                EditorEditErrorCode::prefab_dependency_mismatch,
                {{"expected_closure_generation", *expected_closure},
                 {"current_closure_generation", impl.closureGeneration()}},
                "prefab resolution closure is stale"});
        }
        if (prefab_cas && *expected_provider != impl.providerGeneration()) {
            return reject(EditFailure{
                EditorEditErrorCode::prefab_provider_stale,
                {{"expected_provider_generation", *expected_provider},
                 {"current_provider_generation", impl.providerGeneration()}},
                "prefab provider generation is stale"});
        }
        impl.requireRevertPreconditions(source);
        impl.requireNoLeaseOverlap(source.write_set, source.structural_domain);
        (void)commandsFromCanonical(source.ordered_inverse, impl.document(),
                                    impl.prefabRegistry(),
                                    impl.bindableProvider(),
                                    impl.projectionState());
        Impl::Ticket pending{
            .id = ticket_id,
            .kind = redo ? Impl::Ticket::Kind::redo : Impl::Ticket::Kind::undo,
            .actor_id = actor,
            .base_revision = base,
            .source_transaction_id = source_id,
            .expected_closure_generation = expected_closure,
            .expected_provider_generation = expected_provider,
            .prefab_cas = prefab_cas,
            .accepted_gate_epoch = gate.epoch,
            .accepted_scene_id = impl.dependencies.current_scene_id(),
            .result = {{"ticket", ticket_id},
                       {"actor_id", actor.value},
                       {"base_revision", base.value},
                       {"operation", method},
                       {"source_transaction_id", source_id},
                       {"status", "accepted"}},
        };
        impl.results[ticket_id] = pending.result;
        impl.pending.push_back(std::move(pending));
        return impl.results.at(ticket_id);
    } catch (const EditFailure &failure) {
        return reject(failure);
    }
}

OrderedJson EditorEditCoordinator::enqueueUndo(const Json &params) {
    return enqueueRevert(params, false);
}

OrderedJson EditorEditCoordinator::enqueueRedo(const Json &params) {
    return enqueueRevert(params, true);
}

OrderedJson EditorEditCoordinator::openPreview(const Json &params) {
    constexpr auto method = "open_preview";
    const auto parsed = parseEditorMutationRequest(params, method);
    const auto actor = parsed.actor_id;
    (void)impl_->requireBound(actor);
    const auto request_id = impl_->allocatePreviewRequest();
    const auto ticket = impl_->allocatePreviewTicket();
    const auto gate = impl_->gateSnapshot();
    Impl::PreviewRequest request{
        .request_id = request_id,
        .ticket = ticket,
        .kind = Impl::PreviewRequest::Kind::open,
        .actor_id = actor,
        .raw_operations = parsed.operations,
        .accepted_gate_epoch = gate.epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id},
                   {"ticket", ticket},
                   {"actor_id", actor.value},
                   {"status", "accepted"}},
    };
    const auto reject = [&](const EditFailure &failure) {
        auto result = impl_->previewRejected(request, failure);
        impl_->preview.results[request_id] = result;
        return result;
    };
    if (!gate.can_preview) {
        return reject(EditFailure{
            EditorEditErrorCode::gate_closed,
            {{"method", method},
             {"reason", primaryGateReason(gate)},
             {"gate_epoch", gate.epoch}},
            "editor preview gate is closed"});
    }
    if (impl_->preview.lease || impl_->preview.reservation) {
        const auto owner = impl_->preview.lease
                               ? impl_->preview.lease->actor_id.value
                               : impl_->preview.reservation->actor_id.value;
        return reject(EditFailure{EditorEditErrorCode::preview_lease_busy,
                                  {{"owner", owner}},
                                  "live preview lease is already occupied"});
    }
    try {
        auto batch = prepareBatch(request.raw_operations, impl_->document(),
                                  request.accepted_scene_id,
                                  impl_->prefabRegistry(),
                                  impl_->bindableProvider(),
                                  impl_->projectionState(),
                                  impl_->componentExists());
        requireLivePreviewCapability(batch, method);
        if (!impl_->dependencies.execute_preview) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", method}, {"adapter", "live_preview"}},
                        "live preview execution is unavailable");
        }
        impl_->preview.reservation = Impl::PreviewLease{
            .ticket = ticket,
            .actor_id = actor,
            .base_revision = impl_->document().revision(),
            .raw_operations = request.raw_operations,
            .canonical_operations = batch.canonical_forward,
            .write_set = batchWriteSet(batch),
            .structural_domain = batchStructuralDomains(batch),
            .affected = batch.affected,
            .scene_id = request.accepted_scene_id,
        };
    } catch (const EditFailure &failure) {
        return reject(failure);
    }
    impl_->preview.results[request_id] = request.result;
    impl_->preview.pending.push_back(std::move(request));
    return impl_->preview.results.at(request_id);
}

OrderedJson EditorEditCoordinator::updatePreview(const Json &params) {
    constexpr auto method = "update_preview";
    const auto parsed = parseEditorMutationRequest(params, method);
    const auto actor = parsed.actor_id;
    (void)impl_->requireBound(actor);
    const auto &ticket = *parsed.ticket;
    const auto request_id = impl_->allocatePreviewRequest();
    const auto gate = impl_->gateSnapshot();
    Impl::PreviewRequest request{
        .request_id = request_id,
        .ticket = ticket,
        .kind = Impl::PreviewRequest::Kind::update,
        .actor_id = actor,
        .raw_operations = parsed.operations,
        .accepted_gate_epoch = gate.epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id}, {"ticket", ticket},
                   {"actor_id", actor.value}, {"status", "accepted"}},
    };
    const auto reject = [&](const EditFailure &failure) {
        auto result = impl_->previewRejected(request, failure);
        impl_->preview.results[request_id] = result;
        return result;
    };
    if (!gate.can_preview) {
        return reject(EditFailure{EditorEditErrorCode::gate_closed,
                                  {{"method", method},
                                   {"reason", primaryGateReason(gate)},
                                   {"gate_epoch", gate.epoch}},
                                  "editor preview gate is closed"});
    }
    if (!impl_->preview.lease || impl_->preview.lease->ticket != ticket) {
        return reject(impl_->missingPreviewTicket(ticket));
    }
    if (impl_->preview.lease->actor_id != actor) {
        return reject(EditFailure{EditorEditErrorCode::not_lease_owner,
                                  {{"ticket", ticket},
                                   {"owner", impl_->preview.lease->actor_id.value}},
                                  "preview ticket belongs to another actor"});
    }
    try {
        auto batch = prepareBatch(request.raw_operations, impl_->document(),
                                  request.accepted_scene_id,
                                  impl_->prefabRegistry(),
                                  impl_->bindableProvider(),
                                  impl_->projectionState(),
                                  impl_->componentExists());
        requireLivePreviewCapability(batch, method);
        const auto writes = batchWriteSet(batch);
        if (!sameStableSet(writes, impl_->preview.lease->write_set)) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", method}, {"field", "lease_write_set"}},
                        "preview update cannot expand or replace its lease");
        }
    } catch (const EditFailure &failure) {
        return reject(failure);
    }
    impl_->preview.results[request_id] = request.result;
    impl_->preview.pending.push_back(std::move(request));
    return impl_->preview.results.at(request_id);
}

OrderedJson EditorEditCoordinator::commitPreview(const Json &params) {
    constexpr auto method = "commit_preview";
    requireOnly(params, {"actor_id", "ticket"}, method);
    const auto actor = EditorActorId{exactUnsigned(params.at("actor_id"),
                                                    "commit_preview actor_id", true)};
    (void)impl_->requireBound(actor);
    const auto ticket = requireString(params, "ticket", method);
    const auto request_id = impl_->allocatePreviewRequest();
    const auto gate = impl_->gateSnapshot();
    Impl::PreviewRequest request{
        .request_id = request_id,
        .ticket = ticket,
        .kind = Impl::PreviewRequest::Kind::commit,
        .actor_id = actor,
        .accepted_gate_epoch = gate.epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id}, {"ticket", ticket},
                   {"actor_id", actor.value}, {"status", "accepted"}},
    };
    const auto reject = [&](const EditFailure &failure) {
        auto result = impl_->previewRejected(request, failure);
        impl_->preview.results[request_id] = result;
        return result;
    };
    if (!gate.can_edit) {
        return reject(EditFailure{EditorEditErrorCode::gate_closed,
                                  {{"method", method},
                                   {"reason", primaryGateReason(gate)},
                                   {"gate_epoch", gate.epoch}},
                                  "editor edit gate is closed"});
    }
    if (!impl_->preview.lease || impl_->preview.lease->ticket != ticket) {
        return reject(impl_->missingPreviewTicket(ticket));
    }
    if (impl_->preview.lease->actor_id != actor) {
        return reject(EditFailure{EditorEditErrorCode::not_lease_owner,
                                  {{"ticket", ticket},
                                   {"owner", impl_->preview.lease->actor_id.value}},
                                  "preview ticket belongs to another actor"});
    }
    if (impl_->document().revision() != impl_->preview.lease->base_revision) {
        const EditFailure failure{
            EditorEditErrorCode::stale_revision,
            {{"current_revision", impl_->document().revision().value},
             {"target", targetState(impl_->preview.lease->raw_operations,
                                    impl_->document())}},
            "preview ticket base SceneRevision is stale"};
        auto result = reject(failure);
        (void)impl_->forceAbort("stale_revision");
        return result;
    }
    impl_->preview.results[request_id] = request.result;
    impl_->preview.pending.push_back(std::move(request));
    return impl_->preview.results.at(request_id);
}

OrderedJson EditorEditCoordinator::abortPreview(const Json &params) {
    constexpr auto method = "abort_preview";
    requireOnly(params, {"actor_id", "ticket"}, method);
    const auto actor = EditorActorId{exactUnsigned(params.at("actor_id"),
                                                    "abort_preview actor_id", true)};
    (void)impl_->requireBound(actor);
    const auto ticket = requireString(params, "ticket", method);
    const auto request_id = impl_->allocatePreviewRequest();
    const auto gate = impl_->gateSnapshot();
    Impl::PreviewRequest request{
        .request_id = request_id,
        .ticket = ticket,
        .kind = Impl::PreviewRequest::Kind::abort,
        .actor_id = actor,
        .accepted_gate_epoch = gate.epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id}, {"ticket", ticket},
                   {"actor_id", actor.value}, {"status", "accepted"}},
    };
    if (impl_->preview.lease && impl_->preview.lease->ticket == ticket &&
        impl_->preview.lease->actor_id != actor) {
        const EditFailure failure{EditorEditErrorCode::not_lease_owner,
                                  {{"ticket", ticket},
                                   {"owner", impl_->preview.lease->actor_id.value}},
                                  "preview ticket belongs to another actor"};
        auto result = impl_->previewRejected(request, failure);
        impl_->preview.results[request_id] = result;
        return result;
    }
    impl_->preview.results[request_id] = request.result;
    impl_->preview.pending.push_back(std::move(request));
    return impl_->preview.results.at(request_id);
}

OrderedJson EditorEditCoordinator::getResult(const Json &params) const {
    requireOnly(params, {"ticket"}, "get_edit_result");
    const auto ticket = requireString(params, "ticket", "get_edit_result");
    const auto found = impl_->results.find(ticket);
    if (found == impl_->results.end()) {
        throw std::invalid_argument("get_edit_result ticket was not issued by this session");
    }
    return found->second;
}

OrderedJson EditorEditCoordinator::getPreviewResult(const Json &params) const {
    requireOnly(params, {"request_id"}, "get_preview_result");
    const auto request_id = requireString(params, "request_id",
                                          "get_preview_result");
    const auto found = impl_->preview.results.find(request_id);
    if (found == impl_->preview.results.end()) {
        throw std::invalid_argument(
            "get_preview_result request_id was not issued by this session");
    }
    return found->second;
}

OrderedJson EditorEditCoordinator::queryJournal(const Json &params) const {
    requireOnly(params, {"after_transaction_id"}, "query_journal");
    const auto after = optionalString(params, "after_transaction_id",
                                      "query_journal");
    bool emit = !after.has_value();
    OrderedJson records = OrderedJson::array();
    for (const auto &record : impl_->journal) {
        if (!emit) {
            if (record.transaction_id == *after) emit = true;
            continue;
        }
        records.push_back(editorJournalJson(record));
    }
    return {{"records", std::move(records)},
            {"scene_revision", impl_->document().revision().value}};
}

void EditorEditCoordinator::commitPending() noexcept { impl_->commitPending(); }

std::vector<OrderedJson> EditorEditCoordinator::takeCompletedResults() {
    auto result = std::move(impl_->completed);
    impl_->completed.clear();
    return result;
}

std::vector<std::string> EditorEditCoordinator::pendingTicketIds() const {
    std::vector<std::string> result;
    result.reserve(impl_->pending.size());
    for (const auto &ticket : impl_->pending) result.push_back(ticket.id);
    for (const auto &request : impl_->preview.pending) {
        result.push_back(request.request_id);
    }
    return result;
}

bool EditorEditCoordinator::hasOpenPreviewLease() const noexcept {
    return impl_->preview.lease.has_value();
}

std::uint64_t EditorEditCoordinator::previewEpoch() const noexcept {
    return impl_->preview.epoch;
}

bool EditorEditCoordinator::forceAbortPreview(std::string reason) {
    return impl_->forceAbort(std::move(reason));
}

const std::vector<EditorJournalRecord> &
EditorEditCoordinator::journal() const noexcept {
    return impl_->journal;
}

EditorProjectionResult EditorEditCoordinator::executeJournalForVerification(
    std::string_view transaction_id, bool inverse) {
    const auto found = std::find_if(impl_->journal.begin(), impl_->journal.end(),
                                    [&](const auto &record) {
                                        return record.transaction_id == transaction_id;
                                    });
    if (found == impl_->journal.end()) {
        throw std::invalid_argument("journal transaction does not exist");
    }
    if (journalUsesUnpack(*found)) {
        requireUnpackJournalRecord(
            *found, impl_->requireProjectionState(
                        inverse ? "journal_inverse" : "journal_forward"));
    }
    const auto &journal_operations = inverse ? found->ordered_inverse
                                             : found->ordered_forward;
    const auto operations = rebindJournalBehaviorTargets(
        journal_operations, impl_->dependencies);
    std::vector<EditorCanonicalOperation> canonical_operations;
    auto commands = commandsFromCanonical(
        operations, impl_->document(), impl_->prefabRegistry(),
        impl_->bindableProvider(), impl_->projectionState(),
        &canonical_operations);
    const EditorEditExecutionRequest request{
        .base_revision = impl_->document().revision(),
        .commands = commands,
        .operations = canonical_operations,
    };
    return impl_->dependencies.execute(request);
}

} // namespace Pelican
