#include "editorjournal.hpp"

#include "../loader/authoringsceneauthority.hpp"

#include "../appflow/framephase.hpp"
#include "../loader/componentcodec.hpp"
#include "../userpublic/details/behavior/registerer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr std::uint64_t maxExactJsonInteger = UINT64_C(9007199254740991);

class EditFailure final : public std::runtime_error {
  public:
    EditorEditErrorCode code;
    OrderedJson payload;

    EditFailure(EditorEditErrorCode error_code, OrderedJson error_payload,
                std::string message)
        : std::runtime_error(std::move(message)), code{error_code},
          payload(std::move(error_payload)) {}
};

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
                            bool nonzero = false) {
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

struct ObjectLocation {
    std::string scene_id;
    std::size_t scene_index = 0;
    std::size_t object_index = 0;
    AuthoringObjectId object_id{};
    std::optional<std::string> name;
    std::optional<std::string> parent;
    Json authored;
};

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
                                     EditorEditErrorCode error =
                                         EditorEditErrorCode::closure_unresolvable) {
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
                        std::string_view component_name) {
    const auto *codec = findComponentCodec(component_name);
    if (codec == nullptr) {
        editFailure(EditorEditErrorCode::unknown_component_type,
                    {{"object", object_id.value}, {"name", component_name}},
                    "component type has no registered editor codec");
    }
    try {
        return Json::parse(codec->encodeCanonical(codec->decodeAuthored(component)).dump());
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
    if (registration == nullptr) {
        editFailure(EditorEditErrorCode::method_unavailable,
                    {{"method", "behavior"},
                     {"adapter", "behavior_registration"},
                     {"type", type}},
                    "behavior registration is unavailable");
    }
    const auto params = component.contains("params")
                            ? component.at("params")
                            : Json::object();
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
    explicit LocalDocumentTarget(const AuthoringSceneDocument &source)
        : state_{ResolvedSceneResolver::prepare(
              AuthoringSceneAuthority::stage(
                  source,
                  AuthoringSceneAuthority::rawView(source).documentJson(),
                  SceneRevision{source.revision().value + 1}),
              SceneResolverGeneration{1})} {}

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

struct PreparedOperation {
    std::vector<EditorProjectionCommand> commands;
    OrderedJson forward;
    OrderedJson inverse;
    std::vector<AuthoringObjectId> affected;
    std::string stable_target;
    std::vector<std::string> read_set;
    std::vector<std::string> write_set;
    OrderedJson structural_domain = OrderedJson::object();
};

struct PreparedBatch {
    std::vector<EditorProjectionCommand> commands;
    std::vector<OrderedJson> forward;
    std::vector<OrderedJson> inverse;
    std::vector<PreparedOperation> operations;
    std::vector<AuthoringObjectId> affected;
};

std::string stableComponentTarget(AuthoringObjectId object_id,
                                  std::string_view component,
                                  std::string_view field_path = {}) {
    auto result = "/authoring_objects/" + std::to_string(object_id.value) +
                  "/components/" + std::string{component};
    if (!field_path.empty() && field_path != "/") result += std::string{field_path};
    return result;
}

std::string stableBehaviorTarget(AuthoringObjectId object_id,
                                 std::size_t attachment_index,
                                 std::uint64_t attachment_handle,
                                 std::string_view field_path = {}) {
    auto result = "/authoring_objects/" + std::to_string(object_id.value) +
                  "/behaviors/";
    if (attachment_handle != 0) {
        result += "handles/" + std::to_string(attachment_handle);
    } else {
        result += "indices/" + std::to_string(attachment_index);
    }
    if (!field_path.empty() && field_path != "/") result += std::string{field_path};
    return result;
}

std::uint64_t internalIdentityValue(const Json &operation,
                                    std::string_view field,
                                    std::string_view context,
                                    bool nonzero = false) {
    const auto found = operation.find(std::string{field});
    return found == operation.end() ? 0 : exactUnsigned(*found, context, nonzero);
}

PreparedOperation prepareSet(const Json &raw,
                             const AuthoringSceneDocument &document) {
    constexpr auto context = "set_component_value";
    requireOnly(raw, {"op", "object_id", "component_slot", "field_path",
                      "value", "attachment_index", "attachment_handle",
                      "attachment_seq"}, context);
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object_id"), "set_component_value object_id", true)};
    const auto component_name = requireString(raw, "component_slot", context);
    const auto field_path = requireString(raw, "field_path", context);
    if (field_path.front() != '/' || field_path == "/name" ||
        field_path.starts_with("/name/")) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", component_name},
                     {"field_path", field_path},
                     {"detail", "field_path must be a JSON pointer and cannot change name"}},
                    "invalid component field path");
    }
    const auto object = requireObjectLocation(document, object_id);
    const bool behavior = component_name == "behavior";
    std::size_t attachment_index = 0;
    std::pair<std::size_t, Json> located;
    if (behavior) {
        const auto found = raw.find("attachment_index");
        if (found == raw.end()) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", "/attachment_index"},
                         {"detail", "behavior edits require attachment_index"}},
                        "behavior edits require attachment_index");
        }
        attachment_index = static_cast<std::size_t>(exactUnsigned(
            *found, "set_component_value attachment_index"));
        located = requireComponentAt(object, attachment_index, component_name);
        if (!field_path.starts_with("/params/") || field_path.size() <= 8U) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", field_path},
                         {"detail", "behavior field_path must target one params field"}},
                        "behavior field_path must target one params field");
        }
    } else {
        located = requireComponent(object, component_name);
    }
    const auto &[component_index, old_component] = located;
    const auto *codec = behavior ? nullptr : findComponentCodec(component_name);
    if (!behavior && codec == nullptr) {
        editFailure(EditorEditErrorCode::not_editable,
                    {{"object", object_id.value}, {"slot", component_name}},
                    "component has no editable codec");
    }
    // BehaviorParamsPolicy defaults are part of the editable authored view.
    // Expand them into a temporary before resolving the field pointer, while
    // retaining old_component verbatim for inverse/raw restoration.
    Json next_component = behavior
                              ? canonicalBehaviorComponent(old_component,
                                                           object_id)
                              : old_component;
    try {
        const Json::json_pointer pointer{field_path};
        (void)next_component.at(pointer);
        next_component[pointer] = raw.at("value");
    } catch (const std::exception &error) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", component_name},
                     {"field_path", field_path},
                     {"detail", error.what()}},
                    error.what());
    }
    next_component = behavior
                         ? canonicalBehaviorComponent(next_component, object_id)
                         : canonicalComponent(next_component, object_id,
                                              component_name);
    const auto attachment_handle = internalIdentityValue(
        raw, "attachment_handle", "set_component_value attachment_handle", true);
    const auto attachment_seq = internalIdentityValue(
        raw, "attachment_seq", "set_component_value attachment_seq");
    const auto target = behavior
                            ? stableBehaviorTarget(object_id, attachment_index,
                                                   attachment_handle, field_path)
                            : stableComponentTarget(object_id, component_name,
                                                    field_path);
    OrderedJson forward{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"field_path", field_path},
                        {"value", raw.at("value")},
                        {"authored_component", next_component}};
    OrderedJson inverse{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"field_path", field_path},
                        {"authored_component", old_component}};
    if (behavior) {
        forward["attachment_index"] = attachment_index;
        inverse["attachment_index"] = attachment_index;
        if (attachment_handle != 0) {
            forward["attachment_handle"] = attachment_handle;
            inverse["attachment_handle"] = attachment_handle;
        }
        forward["attachment_seq"] = attachment_seq;
        inverse["attachment_seq"] = attachment_seq;
    }
    return PreparedOperation{
        .commands = {makeReplaceComponentCommand(object, component_index,
                                                  component_name, next_component)},
        .forward = std::move(forward),
        .inverse = std::move(inverse),
        .affected = {object_id},
        .stable_target = target,
        .read_set = {target},
        .write_set = {target},
        .structural_domain = {{"kind", behavior ? "behavior_params" : "value_field"},
                              {"object", object_id.value},
                              {"slot", component_name},
                              {"field_path", field_path},
                              {"attachment_index", behavior ? OrderedJson(attachment_index)
                                                             : OrderedJson(nullptr)},
                              {"attachment_handle", behavior && attachment_handle != 0
                                                            ? OrderedJson(attachment_handle)
                                                            : OrderedJson(nullptr)}},
    };
}

PreparedOperation prepareAdd(const Json &raw,
                             const AuthoringSceneDocument &document) {
    constexpr auto context = "add_component";
    requireOnly(raw, {"op", "object_id", "component", "component_index",
                      "attachment_handle", "attachment_seq"}, context);
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object_id"), "add_component object_id", true)};
    const auto object = requireObjectLocation(document, object_id);
    const auto &component = requireObject(raw.at("component"),
                                          "add_component component");
    const auto component_name = requireString(component, "name", context);
    const bool behavior = component_name == "behavior";
    if (!behavior && hasComponent(object, component_name)) {
        editFailure(EditorEditErrorCode::duplicate_component,
                    {{"object", object_id.value}, {"slot", component_name}},
                    "component slot already exists");
    }
    auto canonical = behavior ? canonicalBehaviorComponent(component, object_id)
                              : canonicalComponent(component, object_id,
                                                   component_name);
    auto component_index = object.authored.at("components").size();
    if (const auto found = raw.find("component_index"); found != raw.end()) {
        component_index = static_cast<std::size_t>(exactUnsigned(
            *found, "add_component component_index"));
        if (component_index > object.authored.at("components").size()) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", "/component_index"},
                         {"detail", "component index is outside the declaration interval"}},
                        "component index is outside the declaration interval");
        }
    }
    const auto attachment_handle = internalIdentityValue(
        raw, "attachment_handle", "add_component attachment_handle", true);
    const auto attachment_seq = internalIdentityValue(
        raw, "attachment_seq", "add_component attachment_seq");
    if (behavior && (attachment_handle == 0 || attachment_seq == 0)) {
        editFailure(EditorEditErrorCode::method_unavailable,
                    {{"method", context}, {"adapter", "behavior_identity"}},
                    "behavior attach identity allocator is unavailable");
    }
    const auto target = behavior
                            ? stableBehaviorTarget(object_id, component_index,
                                                   attachment_handle)
                            : stableComponentTarget(object_id, component_name);
    OrderedJson forward{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"component_index", component_index},
                        {"component", canonical}};
    OrderedJson inverse{{"op", "remove_component"},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"component_index", component_index}};
    if (behavior) {
        forward["attachment_handle"] = attachment_handle;
        forward["attachment_seq"] = attachment_seq;
        inverse["attachment_index"] = component_index;
        inverse["attachment_handle"] = attachment_handle;
        inverse["attachment_seq"] = attachment_seq;
    }
    return PreparedOperation{
        .commands = {makeInsertComponentCommand(object, component_index, canonical)},
        .forward = std::move(forward),
        .inverse = std::move(inverse),
        .affected = {object_id},
        .stable_target = target,
        .read_set = {behavior ? stableBehaviorTarget(object_id, component_index, 0)
                              : target},
        .write_set = behavior
                         ? std::vector<std::string>{
                               "/authoring_objects/" + std::to_string(object_id.value) +
                                   "/behaviors",
                               target}
                         : std::vector<std::string>{target},
        .structural_domain = {{"kind", behavior ? "behavior_attachment"
                                                   : "component_slot"},
                              {"object", object_id.value},
                              {"slot", component_name},
                              {"component_index", component_index},
                              {"attachment_handle", behavior
                                                            ? OrderedJson(attachment_handle)
                                                            : OrderedJson(nullptr)},
                              {"attachment_seq", behavior
                                                         ? OrderedJson(attachment_seq)
                                                         : OrderedJson(nullptr)}},
    };
}

PreparedOperation prepareRemove(const Json &raw,
                                const AuthoringSceneDocument &document) {
    constexpr auto context = "remove_component";
    requireOnly(raw, {"op", "object_id", "component_slot", "attachment_index",
                      "attachment_handle", "attachment_seq"}, context);
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object_id"), "remove_component object_id", true)};
    const auto component_name = requireString(raw, "component_slot", context);
    const bool behavior = component_name == "behavior";
    const auto object = requireObjectLocation(document, object_id);
    std::pair<std::size_t, Json> located;
    if (behavior) {
        const auto found = raw.find("attachment_index");
        if (found == raw.end()) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", "/attachment_index"},
                         {"detail", "behavior remove requires attachment_index"}},
                        "behavior remove requires attachment_index");
        }
        located = requireComponentAt(
            object, static_cast<std::size_t>(exactUnsigned(
                        *found, "remove_component attachment_index")),
            component_name);
    } else {
        located = requireComponent(object, component_name);
    }
    const auto &[component_index, old_component] = located;
    const auto attachment_handle = internalIdentityValue(
        raw, "attachment_handle", "remove_component attachment_handle", true);
    const auto attachment_seq = internalIdentityValue(
        raw, "attachment_seq", "remove_component attachment_seq");
    const auto target = behavior
                            ? stableBehaviorTarget(object_id, component_index,
                                                   attachment_handle)
                            : stableComponentTarget(object_id, component_name);
    OrderedJson forward{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"component_index", component_index}};
    OrderedJson inverse{{"op", "add_component"},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"component_index", component_index},
                        {"component", old_component}};
    if (behavior) {
        forward["attachment_index"] = component_index;
        inverse["attachment_index"] = component_index;
        if (attachment_handle != 0) {
            forward["attachment_handle"] = attachment_handle;
            inverse["attachment_handle"] = attachment_handle;
        }
        forward["attachment_seq"] = attachment_seq;
        inverse["attachment_seq"] = attachment_seq;
    }
    return PreparedOperation{
        .commands = {makeEraseComponentCommand(object, component_index,
                                                component_name)},
        .forward = std::move(forward),
        .inverse = std::move(inverse),
        .affected = {object_id},
        .stable_target = target,
        .read_set = {target},
        .write_set = behavior
                         ? std::vector<std::string>{
                               "/authoring_objects/" + std::to_string(object_id.value) +
                                   "/behaviors",
                               target}
                         : std::vector<std::string>{target},
        .structural_domain = {{"kind", behavior ? "behavior_attachment"
                                                   : "component_slot"},
                              {"object", object_id.value},
                              {"slot", component_name},
                              {"component_index", component_index},
                              {"attachment_handle", behavior && attachment_handle != 0
                                                            ? OrderedJson(attachment_handle)
                                                            : OrderedJson(nullptr)},
                              {"attachment_seq", behavior && attachment_seq != 0
                                                         ? OrderedJson(attachment_seq)
                                                         : OrderedJson(nullptr)}},
    };
}

Json canonicalSpawnObject(const Json &value, std::string_view scene_id,
                          const AuthoringSceneDocument &document) {
    auto object = requireObject(value, "spawn object");
    Json result = object;
    const auto name = result.value("name", std::string{});
    if (name.empty()) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", nullptr}, {"slot", nullptr},
                     {"field_path", "/name"},
                     {"detail", "spawned object requires a name"}},
                    "spawned object requires a name");
    }
    if (objectIdByName(document, scene_id, name)) {
        editFailure(EditorEditErrorCode::name_conflict, {{"name", name}},
                    "spawned object name already exists");
    }
    if (const auto parent = result.find("parent"); parent != result.end()) {
        if (!parent->is_string() || parent->get_ref<const std::string &>().empty() ||
            !objectIdByName(document, scene_id, parent->get_ref<const std::string &>())) {
            editFailure(EditorEditErrorCode::parent_not_found,
                        {{"name", parent->is_string() ? *parent : Json(nullptr)}},
                        "spawn parent does not exist");
        }
    }
    if (!result.contains("components") || !result.at("components").is_array()) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", nullptr}, {"slot", nullptr},
                     {"field_path", "/components"},
                     {"detail", "components must be an array"}},
                    "spawn components must be an array");
    }
    std::unordered_set<std::string> names;
    for (auto &component : result.at("components")) {
        const auto component_name = requireString(component, "name", "spawn component");
        if (component_name != "behavior" &&
            !names.insert(component_name).second) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", nullptr}, {"slot", component_name},
                         {"field_path", "/components"},
                         {"detail", "duplicate component name"}},
                        "spawn component names must be unique");
        }
        if (component_name == "behavior") continue;
        component = canonicalComponent(component, AuthoringObjectId{}, component_name);
    }
    return result;
}

PreparedOperation prepareSpawn(const Json &raw,
                               const AuthoringSceneDocument &document,
                               std::string_view default_scene) {
    constexpr auto context = "spawn";
    requireOnly(raw, {"op", "scene_id", "declaration_index", "object"},
                context);
    const auto scene_id = optionalString(raw, "scene_id", context)
                              .value_or(std::string{default_scene});
    const auto scenes = AuthoringSceneAuthority::query(document);
    const auto scene = std::find_if(scenes.begin(), scenes.end(),
                                    [&](const auto &candidate) {
                                        return candidate.scene_id == scene_id;
                                    });
    if (scene == scenes.end()) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", nullptr}, {"slot", nullptr},
                     {"field_path", "/scene_id"},
                     {"detail", "scene does not exist"}},
                    "spawn scene does not exist");
    }
    auto declaration_index = scene->objects.size();
    if (const auto found = raw.find("declaration_index"); found != raw.end()) {
        declaration_index = static_cast<std::size_t>(exactUnsigned(
            *found, "spawn declaration_index"));
        if (declaration_index > scene->objects.size()) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", nullptr}, {"slot", nullptr},
                         {"field_path", "/declaration_index"},
                         {"detail", "declaration index is outside the scene"}},
                        "spawn declaration index is outside the scene");
        }
    }
    auto object = canonicalSpawnObject(raw.at("object"), scene_id, document);
    const auto name = object.at("name").get<std::string>();
    return PreparedOperation{
        .commands = {makeInsertObjectCommand(scene_id, declaration_index, object)},
        .forward = {{"op", context},
                    {"scene_id", scene_id},
                    {"declaration_index", declaration_index},
                    {"object", object}},
        .inverse = {{"op", "remove_objects"}, {"object_ids", OrderedJson::array()}},
        .stable_target = "/scenes/" + scene_id + "/name_reservations/" + name,
        .read_set = {"/scenes/" + scene_id + "/name_reservations/" + name},
        .write_set = {"/scenes/" + scene_id + "/objects"},
        .structural_domain = {{"kind", "object_existence"},
                              {"scene_id", scene_id},
                              {"name_reservation", name},
                              {"declaration_index", declaration_index}},
    };
}

std::vector<ObjectLocation> subtreeObjects(const AuthoringSceneDocument &document,
                                           const ObjectLocation &root) {
    auto all = objectLocations(document);
    std::vector<ObjectLocation> result{root};
    std::unordered_set<std::string> names;
    if (root.name) names.insert(*root.name);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &candidate : all) {
            if (candidate.scene_id != root.scene_id || !candidate.parent ||
                !names.contains(*candidate.parent) ||
                std::any_of(result.begin(), result.end(), [&](const auto &current) {
                    return current.object_id == candidate.object_id;
                })) {
                continue;
            }
            result.push_back(candidate);
            if (candidate.name) names.insert(*candidate.name);
            changed = true;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.object_index > right.object_index;
    });
    return result;
}

PreparedOperation prepareDestroy(const Json &raw,
                                 const AuthoringSceneDocument &document) {
    constexpr auto context = "destroy";
    requireOnly(raw, {"op", "object_id"}, context);
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object_id"), "destroy object_id", true)};
    const auto root = requireObjectLocation(document, object_id);
    const auto subtree = subtreeObjects(document, root);
    PreparedOperation result;
    result.forward = {{"op", context}, {"object_ids", OrderedJson::array()}};
    result.inverse = {{"op", "restore_objects"}, {"closures", OrderedJson::array()}};
    result.stable_target = "/authoring_objects/" + std::to_string(object_id.value);
    result.read_set = {result.stable_target};
    result.write_set = {result.stable_target};
    result.structural_domain = {{"kind", "object_subtree"},
                                {"scene_id", root.scene_id},
                                {"root", object_id.value},
                                {"objects", OrderedJson::array()},
                                {"declaration_indices", OrderedJson::array()},
                                {"name_reservations", OrderedJson::array()}};
    for (const auto &object : subtree) {
        result.commands.push_back(makeRemoveObjectCommand(object.object_id));
        result.forward["object_ids"].push_back(object.object_id.value);
        result.affected.push_back(object.object_id);
        result.structural_domain["objects"].push_back(object.object_id.value);
        result.structural_domain["declaration_indices"].push_back(
            object.object_index);
        if (object.name) {
            result.structural_domain["name_reservations"].push_back(*object.name);
        }
    }
    return result;
}

PreparedOperation prepareReparent(const Json &raw,
                                  const AuthoringSceneDocument &document) {
    constexpr auto context = "reparent";
    requireOnly(raw, {"op", "object_id", "new_parent_id", "preserve"}, context);
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object_id"), "reparent object_id", true)};
    const auto child = requireObjectLocation(document, object_id);
    if (!child.name) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value}, {"slot", "transform"},
                     {"field_path", "/name"},
                     {"detail", "reparent child requires a name"}},
                    "reparent child requires a name");
    }
    const auto preserve_name = requireString(raw, "preserve", context);
    ReparentPreserve preserve;
    if (preserve_name == "local") {
        preserve = ReparentPreserve::Local;
    } else if (preserve_name == "world") {
        preserve = ReparentPreserve::World;
    } else {
        editFailure(EditorEditErrorCode::preserve_missing,
                    {{"child", object_id.value}},
                    "reparent preserve must be local or world");
    }
    std::optional<AuthoringObjectId> new_parent_id;
    std::optional<std::string> new_parent_name;
    const auto parent_field = raw.find("new_parent_id");
    if (parent_field == raw.end()) {
        editFailure(EditorEditErrorCode::preserve_missing,
                    {{"child", object_id.value}},
                    "reparent requires new_parent_id (null for root)");
    }
    if (!parent_field->is_null()) {
        new_parent_id = AuthoringObjectId{
            exactUnsigned(*parent_field, "reparent new_parent_id", true)};
        const auto parent = requireObjectLocation(document, *new_parent_id,
                                                  EditorEditErrorCode::parent_not_found);
        if (parent.scene_id != child.scene_id || !parent.name) {
            editFailure(EditorEditErrorCode::parent_not_found,
                        {{"name", parent.name ? Json(*parent.name) : Json(nullptr)}},
                        "reparent parent is not a named object in the same scene");
        }
        new_parent_name = parent.name;
    }
    std::optional<AuthoringObjectId> old_parent_id;
    if (child.parent) old_parent_id = objectIdByName(document, child.scene_id, *child.parent);
    const auto [transform_index, old_transform] = requireComponent(child, "transform");
    (void)transform_index;
    const auto target = "/authoring_objects/" + std::to_string(object_id.value) +
                        "/parent";
    OrderedJson descendants = OrderedJson::array();
    for (const auto &descendant : subtreeObjects(document, child)) {
        if (descendant.object_id != object_id) {
            descendants.push_back(descendant.object_id.value);
        }
    }
    OrderedJson forward{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", child.scene_id},
                        {"object_name", *child.name},
                        {"new_parent_id", new_parent_id
                                              ? OrderedJson(new_parent_id->value)
                                              : OrderedJson(nullptr)},
                        {"new_parent_name", new_parent_name
                                                ? OrderedJson(*new_parent_name)
                                                : OrderedJson(nullptr)},
                        {"preserve", preserve_name}};
    OrderedJson inverse{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", child.scene_id},
                        {"object_name", *child.name},
                        {"new_parent_id", old_parent_id
                                              ? OrderedJson(old_parent_id->value)
                                              : OrderedJson(nullptr)},
                        {"new_parent_name", child.parent
                                                ? OrderedJson(*child.parent)
                                                : OrderedJson(nullptr)},
                        {"preserve", preserve_name},
                        {"restore_transform", old_transform}};
    return PreparedOperation{
        .commands = {makeReparentCommand(child.scene_id, *child.name,
                                          new_parent_name, preserve)},
        .forward = std::move(forward),
        .inverse = std::move(inverse),
        .affected = {object_id},
        .stable_target = target,
        .read_set = {target, stableComponentTarget(object_id, "transform")},
        .write_set = {target, stableComponentTarget(object_id, "transform")},
        .structural_domain = {{"kind", "parent_edge"},
                              {"child", object_id.value},
                              {"old_parent", old_parent_id
                                                 ? OrderedJson(old_parent_id->value)
                                                 : OrderedJson(nullptr)},
                              {"new_parent", new_parent_id
                                                 ? OrderedJson(new_parent_id->value)
                                                 : OrderedJson(nullptr)},
                              {"descendants", std::move(descendants)},
                              {"preserve", preserve_name}},
    };
}

PreparedOperation prepareRpcOperation(const Json &raw,
                                      const AuthoringSceneDocument &document,
                                      std::string_view current_scene) {
    requireObject(raw, "edit operation");
    const auto op = requireString(raw, "op", "edit operation");
    if (op == "set_component_value") return prepareSet(raw, document);
    if (op == "add_component") return prepareAdd(raw, document);
    if (op == "remove_component") return prepareRemove(raw, document);
    if (op == "spawn") return prepareSpawn(raw, document, current_scene);
    if (op == "destroy") return prepareDestroy(raw, document);
    if (op == "reparent") return prepareReparent(raw, document);
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
    case EditorProjectionErrorCode::AdapterPrepareFailed:
    case EditorProjectionErrorCode::AdapterPublishFailed:
        return EditFailure{EditorEditErrorCode::method_unavailable,
                           OrderedJson{{"method", method}, {"adapter", path}}, message};
    case EditorProjectionErrorCode::CommandInvalid:
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
        const bool add_behavior =
            op == "add_component" && operation.contains("component") &&
            operation.at("component").is_object() &&
            operation.at("component").value("name", std::string{}) == "behavior";
        const bool target_behavior =
            (op == "remove_component" || op == "set_component_value") &&
            operation.value("component_slot", std::string{}) == "behavior";
        if (!add_behavior && !target_behavior) continue;

        operation.erase("attachment_handle");
        operation.erase("attachment_seq");
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
            if (identity.handle == 0 || identity.attachment_seq == 0 ||
                identity.handle > maxExactJsonInteger ||
                identity.attachment_seq > maxExactJsonInteger) {
                throw std::overflow_error(
                    "behavior attachment identity exceeds the exact JSON range");
            }
            operation["attachment_handle"] = identity.handle;
            operation["attachment_seq"] = identity.attachment_seq;
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
        (void)requireComponentAt(object, attachment_index, "behavior");
        std::optional<EditorBehaviorAttachmentIdentity> identity;
        if (dependencies.resolve_behavior_attachment) {
            identity = dependencies.resolve_behavior_attachment(object_id,
                                                                 attachment_index);
        }
        if (identity) {
            if (identity->handle == 0 || identity->handle > maxExactJsonInteger ||
                identity->attachment_seq > maxExactJsonInteger) {
                throw std::overflow_error(
                    "resolved behavior attachment identity exceeds the exact JSON range");
            }
            operation["attachment_handle"] = identity->handle;
            operation["attachment_seq"] = identity->attachment_seq;
        } else {
            constexpr auto max_part = std::numeric_limits<std::uint32_t>::max();
            if (object.object_index > max_part || attachment_index > max_part) {
                throw std::overflow_error(
                    "scene behavior attachment tuple exceeds 32-bit sequence fields");
            }
            operation["attachment_seq"] =
                (static_cast<std::uint64_t>(object.object_index) << 32U) |
                static_cast<std::uint64_t>(attachment_index);
        }
    }
    return result;
}

PreparedBatch prepareBatch(const Json &raw_operations,
                           const AuthoringSceneDocument &source,
                           std::string_view current_scene) {
    if (!raw_operations.is_array() || raw_operations.empty()) {
        throw std::invalid_argument("edit operations must be a non-empty array");
    }
    LocalDocumentTarget local{source};
    PreparedBatch batch;
    batch.operations.reserve(raw_operations.size());
    for (const auto &raw : raw_operations) {
        auto prepared = prepareRpcOperation(raw, local.projectionDocument(),
                                            current_scene);
        EditorProjectionTransaction transaction{local,
                                                local.projectionDocument().revision()};
        std::vector<EditorProjectionAdapter *> adapters;
        const auto result = transaction.commit(prepared.commands, adapters);
        if (!result.committed()) {
            throw projectionFailure(result, prepared.forward.value("op", "edit"));
        }
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
    if (op == "set_component_value") {
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
                      const AuthoringSceneDocument &document) {
    LocalDocumentTarget local{document};
    std::vector<EditorProjectionCommand> result;
    for (const auto &operation : operations) {
        const auto op = operation.at("op").get<std::string>();
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
                                                   local.projectionDocument()));
        }
        EditorProjectionTransaction transaction{local,
                                                local.projectionDocument().revision()};
        std::vector<EditorProjectionAdapter *> adapters;
        const auto preflight = transaction.commit(current, adapters);
        if (!preflight.committed()) throw projectionFailure(preflight, op);
        result.insert(result.end(), current.begin(), current.end());
    }
    return result;
}

OrderedJson writerJson(const EditorLastWriterStamp &stamp) {
    return {{"revision", stamp.revision.value},
            {"transaction_id", stamp.transaction_id},
            {"actor_id", stamp.actor_id.value}};
}

OrderedJson editErrorJson(EditorEditErrorCode code, const OrderedJson &payload,
                          std::string_view message) {
    return {{"code", editorEditErrorCodeName(code)},
            {"message", message},
            {"payload", payload}};
}

OrderedJson targetState(const Json &operations,
                        const AuthoringSceneDocument &document) {
    OrderedJson result = OrderedJson::object();
    if (!operations.is_array() || operations.empty() || !operations.front().is_object()) {
        return result;
    }
    const auto &operation = operations.front();
    if (const auto id = operation.find("object_id"); id != operation.end()) {
        const auto object_id = AuthoringObjectId{exactUnsigned(*id, "edit object_id", true)};
        result["object_id"] = object_id.value;
        try {
            const auto object = requireObjectLocation(document, object_id);
            result["exists"] = true;
            result["scene_id"] = object.scene_id;
            result["declaration_index"] = object.object_index;
            result["name"] = object.name ? OrderedJson(*object.name) : OrderedJson(nullptr);
            result["parent"] = object.parent ? OrderedJson(*object.parent)
                                              : OrderedJson(nullptr);
            const auto op = operation.value("op", std::string{});
            if (op == "spawn") result["authored_object"] = object.authored;
            if (op == "reparent" && hasComponent(object, "transform")) {
                result["authored_transform"] =
                    requireComponent(object, "transform").second;
            }
            if (const auto slot = operation.find("component_slot");
                slot != operation.end() && slot->is_string()) {
                const auto name = slot->get<std::string>();
                result["component_slot"] = name;
                if (name == "behavior" && operation.contains("attachment_index")) {
                    const auto index = static_cast<std::size_t>(exactUnsigned(
                        operation.at("attachment_index"),
                        "target behavior attachment_index"));
                    const auto &components = object.authored.at("components");
                    const bool exists = index < components.size() &&
                        components.at(index).value("name", std::string{}) == name;
                    result["attachment_index"] = index;
                    if (operation.contains("attachment_handle")) {
                        result["attachment_handle"] = operation.at("attachment_handle");
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
    const auto left_objects = domainObjects(left);
    const auto right_objects = domainObjects(right);
    if (intersects(left_objects, right_objects)) return true;

    const auto left_scene = left.value("scene_id", std::string{});
    const auto right_scene = right.value("scene_id", std::string{});
    if (!left_scene.empty() && left_scene == right_scene) {
        if (intersects(domainNames(left), domainNames(right))) return true;
        if (intersects(domainIndices(left), domainIndices(right))) return true;
    }

    const auto left_kind = left.value("kind", std::string{});
    const auto right_kind = right.value("kind", std::string{});
    if ((left_kind == "component_slot" || left_kind == "value_field") &&
        (right_kind == "component_slot" || right_kind == "value_field") &&
        left.value("object", std::uint64_t{}) ==
            right.value("object", std::uint64_t{}) &&
        left.value("slot", std::string{}) == right.value("slot", std::string{})) {
        return true;
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

bool postconditionsHold(const EditorJournalRecord &record,
                        const AuthoringSceneDocument &document) {
    if (record.commands.size() != record.forward_postconditions.size()) return false;
    for (const auto &command : record.commands) {
        if (targetState(Json::array({command.forward}), document) !=
            command.forward_postcondition) {
            return false;
        }
    }
    return true;
}

void requireLivePreviewCapability(const PreparedBatch &batch,
                                  std::string_view method) {
    for (const auto &operation : batch.operations) {
        const auto op = operation.forward.value("op", std::string{});
        const auto slot = operation.forward.value("component_slot", std::string{});
        if (op != "set_component_value" ||
            (slot != "transform" && slot != "light")) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", method},
                         {"field", op + ":" + slot}},
                        "field has no side-effect-free live preview capability");
        }
    }
}

bool sameStableSet(std::span<const std::string> left,
                   std::span<const std::string> right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

} // namespace

std::string_view editorEditErrorCodeName(EditorEditErrorCode code) noexcept {
    switch (code) {
    case EditorEditErrorCode::stale_revision: return "stale_revision";
    case EditorEditErrorCode::gate_closed: return "gate_closed";
    case EditorEditErrorCode::preview_lease_conflict: return "preview_lease_conflict";
    case EditorEditErrorCode::preview_lease_busy: return "preview_lease_busy";
    case EditorEditErrorCode::not_lease_owner: return "not_lease_owner";
    case EditorEditErrorCode::ticket_not_found: return "ticket_not_found";
    case EditorEditErrorCode::undo_conflict: return "undo_conflict";
    case EditorEditErrorCode::not_editable: return "not_editable";
    case EditorEditErrorCode::schema_violation: return "schema_violation";
    case EditorEditErrorCode::unknown_component_type: return "unknown_component_type";
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
        std::uint64_t accepted_gate_epoch = 0;
        std::uint64_t accepted_transition_epoch = 0;
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
        std::vector<OrderedJson> canonical_operations;
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
        std::uint64_t accepted_transition_epoch = 0;
        std::string accepted_scene_id;
        OrderedJson result;
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
    std::optional<PreviewLease> preview_lease;
    std::optional<PreviewLease> preview_reservation;
    std::vector<PreviewRequest> pending_preview;
    std::unordered_map<std::string, OrderedJson> preview_results;
    std::unordered_map<std::string, PreviewTombstone> preview_tombstones;
    std::uint64_t next_preview_ticket = 1;
    std::uint64_t next_preview_request = 1;
    std::uint64_t preview_epoch = 0;
    std::uint32_t observed_reasons = 0;
    std::uint64_t observed_transition_epoch = 0;
    std::uint64_t gate_epoch = 1;
    bool gate_initialized = false;
    bool hook_installed = false;

    explicit Impl(EditorEditRuntimeDependencies runtime)
        : dependencies{std::move(runtime)} {}

    const AuthoringSceneDocument &document() const { return dependencies.document(); }

    EditorGateSnapshot gateSnapshot() {
        const auto observation = dependencies.gate ? dependencies.gate()
                                                   : EditorGateObservation{};
        if (!gate_initialized) {
            observed_reasons = observation.reasons;
            observed_transition_epoch = observation.transition_epoch;
            gate_initialized = true;
        } else if (observed_reasons != observation.reasons ||
                   observed_transition_epoch != observation.transition_epoch) {
            observed_reasons = observation.reasons;
            observed_transition_epoch = observation.transition_epoch;
            ++gate_epoch;
        }
        return EditorGateSnapshot{.can_edit = observed_reasons == 0,
                                  .can_preview = observed_reasons == 0,
                                  .epoch = gate_epoch,
                                  .reasons = gateReasonNames(observed_reasons)};
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
        return "preview-" + std::to_string(next_preview_ticket++);
    }
    std::string allocatePreviewRequest() {
        return "preview-request-" + std::to_string(next_preview_request++);
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
                {"error", editErrorJson(failure.code, failure.payload,
                                         failure.what())}};
    }

    OrderedJson previewRejected(const PreviewRequest &request,
                                const EditFailure &failure,
                                std::string status = "rejected") const {
        return {{"request_id", request.request_id},
                {"ticket", request.ticket},
                {"actor_id", request.actor_id.value},
                {"status", std::move(status)},
                {"error", editErrorJson(failure.code, failure.payload,
                                         failure.what())}};
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
        if (preview_lease && leaseOverlaps(*preview_lease, writes, domains)) {
            return &*preview_lease;
        }
        if (preview_reservation &&
            leaseOverlaps(*preview_reservation, writes, domains)) {
            return &*preview_reservation;
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
            record.stable_targets.push_back(command.stable_target);
            record.read_set.insert(record.read_set.end(), command.read_set.begin(),
                                   command.read_set.end());
            record.write_set.insert(record.write_set.end(), command.write_set.begin(),
                                    command.write_set.end());
            record.structural_domain.push_back(command.structural_domain);
            record.forward_postconditions.push_back(command.forward_postcondition);
            record.commands.push_back(std::move(command));
        }
        const auto deduplicate = [](std::vector<std::string> &values) {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        };
        deduplicate(record.stable_targets);
        deduplicate(record.read_set);
        deduplicate(record.write_set);
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
            record.stable_targets.push_back(command.stable_target);
            record.read_set.insert(record.read_set.end(), command.read_set.begin(),
                                   command.read_set.end());
            record.write_set.insert(record.write_set.end(), command.write_set.begin(),
                                    command.write_set.end());
            record.structural_domain.push_back(command.structural_domain);
            record.forward_postconditions.push_back(command.forward_postcondition);
            record.commands.push_back(std::move(command));
        }
        const auto deduplicate = [](std::vector<std::string> &values) {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        };
        deduplicate(record.stable_targets);
        deduplicate(record.read_set);
        deduplicate(record.write_set);
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
        if (preview_epoch >= maxExactJsonInteger) {
            throw std::overflow_error("preview_epoch space exhausted");
        }
        ++preview_epoch;
    }

    EditFailure missingPreviewTicket(std::string_view ticket) const {
        OrderedJson payload{{"ticket", ticket}};
        if (const auto found = preview_tombstones.find(std::string{ticket});
            found != preview_tombstones.end()) {
            payload["final_status"] = found->second.final_status;
        }
        return EditFailure{EditorEditErrorCode::ticket_not_found,
                           std::move(payload),
                           "preview ticket no longer exists"};
    }

    EditorProjectionResult executeLivePreview(
        const PreviewLease &lease, std::span<const EditorProjectionCommand> commands,
        std::span<const OrderedJson> operations, bool restore) {
        if (!dependencies.execute_preview) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", restore ? "abort_preview" : "open_preview"},
                         {"adapter", "live_preview"}},
                        "live preview execution is unavailable");
        }
        const EditorPreviewExecutionRequest request{
            .base_revision = document().revision(),
            .commands = commands,
            .operations = operations,
            .restore_committed = restore,
        };
        return dependencies.execute_preview(request);
    }

    bool forceAbort(std::string reason) noexcept {
        if (!preview_lease) return false;
        try {
            const auto lease = *preview_lease;
            const auto projection = executeLivePreview(
                lease, {}, lease.canonical_operations, true);
            if (!projection.committed()) return false;
            if (dependencies.preview_boundary) dependencies.preview_boundary();
            advancePreviewEpoch();
            preview_tombstones[lease.ticket] =
                PreviewTombstone{lease.actor_id, "forced_aborted"};
            completed.push_back({{"event", "ticket_forced_aborted"},
                                 {"ticket", lease.ticket},
                                 {"actor_id", lease.actor_id.value},
                                 {"reason", std::move(reason)},
                                 {"final_status", "forced_aborted"},
                                 {"preview_epoch", preview_epoch}});
            preview_lease.reset();
            return true;
        } catch (...) {
            return false;
        }
    }

    void finishPreviewRequest(PreviewRequest &request, OrderedJson result) {
        request.result = std::move(result);
        preview_results[request.request_id] = request.result;
        completed.push_back(request.result);
    }

    void commitPendingPreview() {
        auto queue = std::move(pending_preview);
        pending_preview.clear();
        for (auto &request : queue) {
            try {
                const auto gate = gateSnapshot();
                const bool gate_changed =
                    gate.epoch != request.accepted_gate_epoch ||
                    observed_transition_epoch != request.accepted_transition_epoch ||
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
                    finishPreviewRequest(request,
                                         previewRejected(request, failure, "failed"));
                    if (request.kind == PreviewRequest::Kind::open &&
                        preview_reservation &&
                        preview_reservation->ticket == request.ticket) {
                        preview_reservation.reset();
                    }
                    continue;
                }

                if (request.kind == PreviewRequest::Kind::open) {
                    if (preview_lease) {
                        const EditFailure failure{
                            EditorEditErrorCode::preview_lease_busy,
                            {{"owner", preview_lease->actor_id.value}},
                            "live preview lease is already occupied"};
                        finishPreviewRequest(request,
                                             previewRejected(request, failure));
                        preview_reservation.reset();
                        continue;
                    }
                    auto batch = prepareBatch(request.raw_operations, document(),
                                              request.accepted_scene_id);
                    requireLivePreviewCapability(batch, "open_preview");
                    PreviewLease lease{
                        .ticket = request.ticket,
                        .actor_id = request.actor_id,
                        .base_revision = document().revision(),
                        .raw_operations = request.raw_operations,
                        .canonical_operations = batch.forward,
                        .write_set = batchWriteSet(batch),
                        .structural_domain = batchStructuralDomains(batch),
                        .affected = batch.affected,
                        .scene_id = request.accepted_scene_id,
                    };
                    const auto projection = executeLivePreview(
                        lease, batch.commands, batch.forward, false);
                    if (!projection.committed()) {
                        throw projectionFailure(projection, "open_preview");
                    }
                    if (dependencies.preview_boundary) dependencies.preview_boundary();
                    advancePreviewEpoch();
                    preview_lease = std::move(lease);
                    preview_reservation.reset();
                    finishPreviewRequest(
                        request,
                        {{"request_id", request.request_id},
                         {"ticket", request.ticket},
                         {"actor_id", request.actor_id.value},
                         {"base_revision", preview_lease->base_revision.value},
                         {"affected_authoring_ids", [&] {
                              OrderedJson values = OrderedJson::array();
                              for (const auto id : preview_lease->affected) {
                                  values.push_back(id.value);
                              }
                              return values;
                          }()},
                         {"status", "open"},
                         {"preview_epoch", preview_epoch}});
                    continue;
                }

                if (!preview_lease || preview_lease->ticket != request.ticket) {
                    if (request.kind == PreviewRequest::Kind::abort) {
                        const auto tombstone = preview_tombstones.find(request.ticket);
                        const auto final_status =
                            tombstone == preview_tombstones.end()
                                ? std::string{"not_found"}
                                : tombstone->second.final_status;
                        finishPreviewRequest(
                            request,
                            {{"request_id", request.request_id},
                             {"ticket", request.ticket},
                             {"actor_id", request.actor_id.value},
                             {"status", "succeeded"},
                             {"final_status", final_status},
                             {"preview_epoch", preview_epoch}});
                    } else {
                        const auto failure = missingPreviewTicket(request.ticket);
                        finishPreviewRequest(request,
                                             previewRejected(request, failure));
                    }
                    continue;
                }
                if (preview_lease->actor_id != request.actor_id) {
                    const EditFailure failure{
                        EditorEditErrorCode::not_lease_owner,
                        {{"ticket", request.ticket},
                         {"owner", preview_lease->actor_id.value}},
                        "preview ticket belongs to another actor"};
                    finishPreviewRequest(request,
                                         previewRejected(request, failure));
                    continue;
                }

                if (request.kind == PreviewRequest::Kind::update) {
                    auto batch = prepareBatch(request.raw_operations, document(),
                                              request.accepted_scene_id);
                    requireLivePreviewCapability(batch, "update_preview");
                    auto writes = batchWriteSet(batch);
                    if (!sameStableSet(writes, preview_lease->write_set)) {
                        editFailure(EditorEditErrorCode::method_unavailable,
                                    {{"method", "update_preview"},
                                     {"field", "lease_write_set"}},
                                    "preview update cannot expand or replace its lease");
                    }
                    const auto projection = executeLivePreview(
                        *preview_lease, batch.commands, batch.forward, false);
                    if (!projection.committed()) {
                        throw projectionFailure(projection, "update_preview");
                    }
                    preview_lease->raw_operations = request.raw_operations;
                    preview_lease->canonical_operations = batch.forward;
                    preview_lease->affected = batch.affected;
                    advancePreviewEpoch();
                    finishPreviewRequest(request,
                                         {{"request_id", request.request_id},
                                          {"ticket", request.ticket},
                                          {"actor_id", request.actor_id.value},
                                          {"status", "updated"},
                                          {"preview_epoch", preview_epoch}});
                    continue;
                }

                if (request.kind == PreviewRequest::Kind::commit) {
                    if (document().revision() != preview_lease->base_revision) {
                        const EditFailure failure{
                            EditorEditErrorCode::stale_revision,
                            {{"current_revision", document().revision().value},
                             {"target", targetState(preview_lease->raw_operations,
                                                    document())}},
                            "preview ticket base SceneRevision is stale"};
                        finishPreviewRequest(request,
                                             previewRejected(request, failure));
                        (void)forceAbort("stale_revision");
                        continue;
                    }
                    auto batch = prepareBatch(preview_lease->raw_operations,
                                              document(), preview_lease->scene_id);
                    const EditorEditExecutionRequest execution{
                        .base_revision = preview_lease->base_revision,
                        .commands = batch.commands,
                        .operations = batch.forward,
                    };
                    const auto projection = dependencies.execute(execution);
                    if (!projection.committed()) {
                        throw projectionFailure(projection, "commit_preview");
                    }
                    Ticket journal_ticket{
                        .id = request.request_id,
                        .kind = Ticket::Kind::edit,
                        .actor_id = request.actor_id,
                        .base_revision = preview_lease->base_revision,
                        .raw_operations = preview_lease->raw_operations,
                    };
                    const auto transaction_id = allocateTransaction();
                    finalizeJournal(journal_ticket, std::move(batch), projection,
                                    transaction_id);
                    advancePreviewEpoch();
                    preview_tombstones[request.ticket] =
                        PreviewTombstone{request.actor_id, "committed"};
                    preview_lease.reset();
                    finishPreviewRequest(
                        request,
                        {{"request_id", request.request_id},
                         {"ticket", request.ticket},
                         {"actor_id", request.actor_id.value},
                         {"status", "committed"},
                         {"committed_revision", projection.committed_revision.value},
                         {"transaction_id", transaction_id},
                         {"preview_epoch", preview_epoch}});
                    continue;
                }

                const auto projection = executeLivePreview(
                    *preview_lease, {}, preview_lease->canonical_operations, true);
                if (!projection.committed()) {
                    throw projectionFailure(projection, "abort_preview");
                }
                if (dependencies.preview_boundary) dependencies.preview_boundary();
                advancePreviewEpoch();
                preview_tombstones[request.ticket] =
                    PreviewTombstone{request.actor_id, "aborted"};
                preview_lease.reset();
                finishPreviewRequest(request,
                                     {{"request_id", request.request_id},
                                      {"ticket", request.ticket},
                                      {"actor_id", request.actor_id.value},
                                      {"status", "succeeded"},
                                      {"final_status", "aborted"},
                                      {"preview_epoch", preview_epoch}});
            } catch (const EditFailure &failure) {
                finishPreviewRequest(request,
                                     previewRejected(request, failure, "failed"));
                if (request.kind == PreviewRequest::Kind::open &&
                    preview_reservation &&
                    preview_reservation->ticket == request.ticket) {
                    preview_reservation.reset();
                }
            } catch (const std::exception &error) {
                const EditFailure failure{
                    EditorEditErrorCode::method_unavailable,
                    {{"method", "preview"}, {"adapter", "execution"}},
                    error.what()};
                finishPreviewRequest(request,
                                     previewRejected(request, failure, "failed"));
                if (request.kind == PreviewRequest::Kind::open) {
                    preview_reservation.reset();
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
            if (preview_lease &&
                (!preview_gate.can_preview ||
                 dependencies.current_scene_id() != preview_lease->scene_id)) {
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
                if (!snapshot.can_edit || snapshot.epoch != ticket.accepted_gate_epoch ||
                    observed_transition_epoch != ticket.accepted_transition_epoch) {
                    const EditFailure failure{
                        EditorEditErrorCode::gate_closed,
                        OrderedJson{{"method", ticket.kind == Ticket::Kind::edit
                                                  ? "edit"
                                                  : ticket.kind == Ticket::Kind::undo
                                                        ? "undo"
                                                        : "redo"},
                                    {"reason", primaryGateReason(snapshot)},
                                    {"gate_epoch", snapshot.epoch}},
                        "editor gate changed after request acceptance"};
                    ticket.result = rejected(ticket.id, ticket.actor_id,
                                             ticket.base_revision, failure);
                    ticket.result["status"] = "failed";
                    results[ticket.id] = ticket.result;
                    completed.push_back(ticket.result);
                    continue;
                }
                if (dependencies.current_scene_id() != ticket.accepted_scene_id) {
                    ++gate_epoch;
                    const EditFailure failure{
                        EditorEditErrorCode::gate_closed,
                        OrderedJson{{"method", "edit"},
                                    {"reason", "reload_scene_transition"},
                                    {"gate_epoch", gate_epoch}},
                        "scene changed after request acceptance"};
                    ticket.result = rejected(ticket.id, ticket.actor_id,
                                             ticket.base_revision, failure);
                    ticket.result["status"] = "failed";
                    results[ticket.id] = ticket.result;
                    completed.push_back(ticket.result);
                    continue;
                }
                if (document().revision() != ticket.base_revision) {
                    const EditFailure failure{
                        EditorEditErrorCode::stale_revision,
                        OrderedJson{{"current_revision", document().revision().value},
                                    {"target", targetState(ticket.raw_operations, document())}},
                        "base SceneRevision is stale at execution"};
                    ticket.result = rejected(ticket.id, ticket.actor_id,
                                             ticket.base_revision, failure);
                    results[ticket.id] = ticket.result;
                    completed.push_back(ticket.result);
                    continue;
                }

                PreparedBatch batch;
                std::optional<EditorJournalRecord> revert_source;
                std::vector<EditorProjectionCommand> revert_commands;
                std::span<const OrderedJson> execution_operations;
                std::span<const EditorProjectionCommand> execution_commands;
                if (ticket.kind == Ticket::Kind::edit) {
                    batch = prepareBatch(ticket.raw_operations, document(),
                                         ticket.accepted_scene_id);
                    const auto writes = batchWriteSet(batch);
                    const auto domains = batchStructuralDomains(batch);
                    requireNoLeaseOverlap(writes, domains);
                    execution_operations = batch.forward;
                    execution_commands = batch.commands;
                } else {
                    revert_source = requireJournal(*ticket.source_transaction_id);
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
                    revert_commands = commandsFromCanonical(
                        revert_source->ordered_inverse, document());
                    execution_operations = revert_source->ordered_inverse;
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
                                            : ticket.kind == Ticket::Kind::undo
                                                  ? std::string{"undo"}
                                                  : std::string{"redo"};
                    const auto failure = projectionFailure(projection, method);
                    ticket.result = rejected(ticket.id, ticket.actor_id,
                                             ticket.base_revision, failure);
                    ticket.result["status"] = projection.status ==
                                                       EditorProjectionStatus::Failed
                                                   ? "failed"
                                                   : "rejected";
                    results[ticket.id] = ticket.result;
                    completed.push_back(ticket.result);
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
                ticket.result = {{"ticket", ticket.id},
                                 {"actor_id", ticket.actor_id.value},
                                 {"base_revision", ticket.base_revision.value},
                                 {"committed_revision",
                                  projection.committed_revision.value},
                                 {"transaction_id", transaction_id},
                                 {"operation", ticket.kind == Ticket::Kind::edit
                                                   ? "edit"
                                                   : ticket.kind == Ticket::Kind::undo
                                                         ? "undo"
                                                         : "redo"},
                                 {"status", "committed"}};
                if (ticket.source_transaction_id) {
                    ticket.result["source_transaction_id"] =
                        *ticket.source_transaction_id;
                }
                results[ticket.id] = ticket.result;
                completed.push_back(ticket.result);
                if (preview_lease) (void)forceAbort("base_revision_stale");
            } catch (const EditFailure &failure) {
                ticket.result = rejected(ticket.id, ticket.actor_id,
                                         ticket.base_revision, failure);
                ticket.result["status"] = "failed";
                results[ticket.id] = ticket.result;
                completed.push_back(ticket.result);
            } catch (const std::exception &error) {
                const EditFailure failure{
                    EditorEditErrorCode::method_unavailable,
                    OrderedJson{{"method", "edit"}, {"adapter", "execution"}},
                    error.what()};
                ticket.result = rejected(ticket.id, ticket.actor_id,
                                         ticket.base_revision, failure);
                ticket.result["status"] = "failed";
                results[ticket.id] = ticket.result;
                completed.push_back(ticket.result);
            } catch (...) {
                const EditFailure failure{
                    EditorEditErrorCode::method_unavailable,
                    OrderedJson{{"method", "edit"}, {"adapter", "execution"}},
                    "unknown editor execution failure"};
                ticket.result = rejected(ticket.id, ticket.actor_id,
                                         ticket.base_revision, failure);
                ticket.result["status"] = "failed";
                results[ticket.id] = ticket.result;
                completed.push_back(ticket.result);
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
    if (impl_->preview_lease || impl_->preview_reservation) {
        reasons.push_back("preview_lease_conflict");
    }
    return {{"can_preview", snapshot.can_preview &&
                                !impl_->preview_lease &&
                                !impl_->preview_reservation},
            {"gate_epoch", snapshot.epoch},
            {"preview_epoch", impl_->preview_epoch},
            {"reasons", reasons}};
}

OrderedJson EditorEditCoordinator::enqueue(const Json &params) {
    requireOnly(params, {"actor_id", "base_revision", "operations",
                         "coalesce_key"}, "edit");
    const auto actor = EditorActorId{exactUnsigned(params.at("actor_id"),
                                                    "edit actor_id", true)};
    (void)impl_->requireBound(actor);
    const auto base = SceneRevision{exactUnsigned(params.at("base_revision"),
                                                   "edit base_revision")};
    const auto &operations = params.at("operations");
    const auto coalesce = optionalString(params, "coalesce_key", "edit");
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
    Json normalized_operations;
    try {
        normalized_operations = normalizeBehaviorAttachmentIdentities(
            operations, impl_->document(), base, impl_->dependencies);
        const auto batch = prepareBatch(normalized_operations, impl_->document(),
                                        impl_->dependencies.current_scene_id());
        const auto writes = batchWriteSet(batch);
        const auto domains = batchStructuralDomains(batch);
        impl_->requireNoLeaseOverlap(writes, domains);
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
        .accepted_gate_epoch = gate.epoch,
        .accepted_transition_epoch = impl_->observed_transition_epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"ticket", ticket},
                   {"actor_id", actor.value},
                   {"base_revision", base.value},
                   {"status", "accepted"}},
    };
    impl_->results[ticket] = pending.result;
    impl_->pending.push_back(std::move(pending));
    return impl_->results.at(ticket);
}

OrderedJson EditorEditCoordinator::enqueueRevert(const Json &params,
                                                  bool redo) {
    auto &impl = *impl_;
    const auto method = redo ? "redo" : "undo";
    requireOnly(params, {"actor_id", "base_revision", "transaction_id"},
                method);
    const auto actor = EditorActorId{exactUnsigned(
        params.at("actor_id"), std::string{method} + " actor_id", true)};
    (void)impl.requireBound(actor);
    const auto base = SceneRevision{exactUnsigned(
        params.at("base_revision"), std::string{method} + " base_revision")};
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
    if (impl.document().revision() != base) {
        return reject(EditFailure{
            EditorEditErrorCode::stale_revision,
            {{"current_revision", impl.document().revision().value},
             {"target", nullptr}},
            "base SceneRevision is stale"});
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
        if (requested && *requested != source_id) {
            const auto &source = impl.requireJournal(source_id);
            impl.throwUndoConflict(
                source,
                source.structural_domain.empty()
                    ? OrderedJson(source.write_set)
                    : source.structural_domain.front(),
                source.transaction_id, impl.document().revision());
        }
        const auto &source = impl.requireJournal(source_id);
        impl.requireRevertPreconditions(source);
        impl.requireNoLeaseOverlap(source.write_set, source.structural_domain);
        (void)commandsFromCanonical(source.ordered_inverse, impl.document());
        Impl::Ticket pending{
            .id = ticket_id,
            .kind = redo ? Impl::Ticket::Kind::redo : Impl::Ticket::Kind::undo,
            .actor_id = actor,
            .base_revision = base,
            .source_transaction_id = source_id,
            .accepted_gate_epoch = gate.epoch,
            .accepted_transition_epoch = impl.observed_transition_epoch,
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
    requireOnly(params, {"actor_id", "operations"}, method);
    const auto actor = EditorActorId{exactUnsigned(params.at("actor_id"),
                                                    "open_preview actor_id", true)};
    (void)impl_->requireBound(actor);
    const auto request_id = impl_->allocatePreviewRequest();
    const auto ticket = impl_->allocatePreviewTicket();
    const auto gate = impl_->gateSnapshot();
    Impl::PreviewRequest request{
        .request_id = request_id,
        .ticket = ticket,
        .kind = Impl::PreviewRequest::Kind::open,
        .actor_id = actor,
        .raw_operations = params.at("operations"),
        .accepted_gate_epoch = gate.epoch,
        .accepted_transition_epoch = impl_->observed_transition_epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id},
                   {"ticket", ticket},
                   {"actor_id", actor.value},
                   {"status", "accepted"}},
    };
    const auto reject = [&](const EditFailure &failure) {
        auto result = impl_->previewRejected(request, failure);
        impl_->preview_results[request_id] = result;
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
    if (impl_->preview_lease || impl_->preview_reservation) {
        const auto owner = impl_->preview_lease
                               ? impl_->preview_lease->actor_id.value
                               : impl_->preview_reservation->actor_id.value;
        return reject(EditFailure{EditorEditErrorCode::preview_lease_busy,
                                  {{"owner", owner}},
                                  "live preview lease is already occupied"});
    }
    try {
        auto batch = prepareBatch(request.raw_operations, impl_->document(),
                                  request.accepted_scene_id);
        requireLivePreviewCapability(batch, method);
        if (!impl_->dependencies.execute_preview) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", method}, {"adapter", "live_preview"}},
                        "live preview execution is unavailable");
        }
        impl_->preview_reservation = Impl::PreviewLease{
            .ticket = ticket,
            .actor_id = actor,
            .base_revision = impl_->document().revision(),
            .raw_operations = request.raw_operations,
            .canonical_operations = batch.forward,
            .write_set = batchWriteSet(batch),
            .structural_domain = batchStructuralDomains(batch),
            .affected = batch.affected,
            .scene_id = request.accepted_scene_id,
        };
    } catch (const EditFailure &failure) {
        return reject(failure);
    }
    impl_->preview_results[request_id] = request.result;
    impl_->pending_preview.push_back(std::move(request));
    return impl_->preview_results.at(request_id);
}

OrderedJson EditorEditCoordinator::updatePreview(const Json &params) {
    constexpr auto method = "update_preview";
    requireOnly(params, {"actor_id", "ticket", "operations"}, method);
    const auto actor = EditorActorId{exactUnsigned(params.at("actor_id"),
                                                    "update_preview actor_id", true)};
    (void)impl_->requireBound(actor);
    const auto ticket = requireString(params, "ticket", method);
    const auto request_id = impl_->allocatePreviewRequest();
    const auto gate = impl_->gateSnapshot();
    Impl::PreviewRequest request{
        .request_id = request_id,
        .ticket = ticket,
        .kind = Impl::PreviewRequest::Kind::update,
        .actor_id = actor,
        .raw_operations = params.at("operations"),
        .accepted_gate_epoch = gate.epoch,
        .accepted_transition_epoch = impl_->observed_transition_epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id}, {"ticket", ticket},
                   {"actor_id", actor.value}, {"status", "accepted"}},
    };
    const auto reject = [&](const EditFailure &failure) {
        auto result = impl_->previewRejected(request, failure);
        impl_->preview_results[request_id] = result;
        return result;
    };
    if (!gate.can_preview) {
        return reject(EditFailure{EditorEditErrorCode::gate_closed,
                                  {{"method", method},
                                   {"reason", primaryGateReason(gate)},
                                   {"gate_epoch", gate.epoch}},
                                  "editor preview gate is closed"});
    }
    if (!impl_->preview_lease || impl_->preview_lease->ticket != ticket) {
        return reject(impl_->missingPreviewTicket(ticket));
    }
    if (impl_->preview_lease->actor_id != actor) {
        return reject(EditFailure{EditorEditErrorCode::not_lease_owner,
                                  {{"ticket", ticket},
                                   {"owner", impl_->preview_lease->actor_id.value}},
                                  "preview ticket belongs to another actor"});
    }
    try {
        auto batch = prepareBatch(request.raw_operations, impl_->document(),
                                  request.accepted_scene_id);
        requireLivePreviewCapability(batch, method);
        const auto writes = batchWriteSet(batch);
        if (!sameStableSet(writes, impl_->preview_lease->write_set)) {
            editFailure(EditorEditErrorCode::method_unavailable,
                        {{"method", method}, {"field", "lease_write_set"}},
                        "preview update cannot expand or replace its lease");
        }
    } catch (const EditFailure &failure) {
        return reject(failure);
    }
    impl_->preview_results[request_id] = request.result;
    impl_->pending_preview.push_back(std::move(request));
    return impl_->preview_results.at(request_id);
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
        .accepted_transition_epoch = impl_->observed_transition_epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id}, {"ticket", ticket},
                   {"actor_id", actor.value}, {"status", "accepted"}},
    };
    const auto reject = [&](const EditFailure &failure) {
        auto result = impl_->previewRejected(request, failure);
        impl_->preview_results[request_id] = result;
        return result;
    };
    if (!gate.can_edit) {
        return reject(EditFailure{EditorEditErrorCode::gate_closed,
                                  {{"method", method},
                                   {"reason", primaryGateReason(gate)},
                                   {"gate_epoch", gate.epoch}},
                                  "editor edit gate is closed"});
    }
    if (!impl_->preview_lease || impl_->preview_lease->ticket != ticket) {
        return reject(impl_->missingPreviewTicket(ticket));
    }
    if (impl_->preview_lease->actor_id != actor) {
        return reject(EditFailure{EditorEditErrorCode::not_lease_owner,
                                  {{"ticket", ticket},
                                   {"owner", impl_->preview_lease->actor_id.value}},
                                  "preview ticket belongs to another actor"});
    }
    if (impl_->document().revision() != impl_->preview_lease->base_revision) {
        const EditFailure failure{
            EditorEditErrorCode::stale_revision,
            {{"current_revision", impl_->document().revision().value},
             {"target", targetState(impl_->preview_lease->raw_operations,
                                    impl_->document())}},
            "preview ticket base SceneRevision is stale"};
        auto result = reject(failure);
        (void)impl_->forceAbort("stale_revision");
        return result;
    }
    impl_->preview_results[request_id] = request.result;
    impl_->pending_preview.push_back(std::move(request));
    return impl_->preview_results.at(request_id);
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
        .accepted_transition_epoch = impl_->observed_transition_epoch,
        .accepted_scene_id = impl_->dependencies.current_scene_id(),
        .result = {{"request_id", request_id}, {"ticket", ticket},
                   {"actor_id", actor.value}, {"status", "accepted"}},
    };
    if (impl_->preview_lease && impl_->preview_lease->ticket == ticket &&
        impl_->preview_lease->actor_id != actor) {
        const EditFailure failure{EditorEditErrorCode::not_lease_owner,
                                  {{"ticket", ticket},
                                   {"owner", impl_->preview_lease->actor_id.value}},
                                  "preview ticket belongs to another actor"};
        auto result = impl_->previewRejected(request, failure);
        impl_->preview_results[request_id] = result;
        return result;
    }
    impl_->preview_results[request_id] = request.result;
    impl_->pending_preview.push_back(std::move(request));
    return impl_->preview_results.at(request_id);
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
    const auto found = impl_->preview_results.find(request_id);
    if (found == impl_->preview_results.end()) {
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
    for (const auto &request : impl_->pending_preview) {
        result.push_back(request.request_id);
    }
    return result;
}

bool EditorEditCoordinator::hasOpenPreviewLease() const noexcept {
    return impl_->preview_lease.has_value();
}

std::uint64_t EditorEditCoordinator::previewEpoch() const noexcept {
    return impl_->preview_epoch;
}

bool EditorEditCoordinator::forceAbortPreview(std::string reason) noexcept {
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
    const auto &operations = inverse ? found->ordered_inverse
                                     : found->ordered_forward;
    auto commands = commandsFromCanonical(operations, impl_->document());
    const EditorEditExecutionRequest request{
        .base_revision = impl_->document().revision(),
        .commands = commands,
        .operations = operations,
    };
    return impl_->dependencies.execute(request);
}

} // namespace Pelican
