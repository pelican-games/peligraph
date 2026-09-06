#include "editorcommandservice.hpp"

#include "../loader/sceneauthoringadapter.hpp"
#include "../shader/raygrouplayout.hpp"
#include "schemavocabularyadapter.hpp"

#include "renderconfigeditor.hpp"
#include "proceduralasseteditor.hpp"
#include "../loader/basicconfig.hpp"
#include "../../project/sceneformat.hpp"
#include "../userpublic/details/behavior/registerer.hpp"
#include "../ecs/componentinfo.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <picosha2.h>
#include <type_traits>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void invalidParams(const std::string &message) {
    throw EditorCommandError{EditorCommandErrorCode::InvalidParams, message};
}

const Json &requireObjectParams(const Json &params, std::string_view method) {
    if (!params.is_object()) invalidParams(std::string{method} + " params must be an object");
    return params;
}

void requireOnlyFields(const Json &params, std::initializer_list<std::string_view> fields,
                       std::string_view method) {
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            invalidParams(std::string{method} + " unknown field: " + it.key());
        }
    }
}

std::optional<std::string> optionalString(const Json &params, std::string_view field,
                                          std::string_view method) {
    const auto found = params.find(field);
    if (found == params.end()) return std::nullopt;
    if (!found->is_string() || found->get_ref<const std::string &>().empty()) {
        invalidParams(std::string{method} + " " + std::string{field} +
                      " must be a non-empty string");
    }
    return found->get<std::string>();
}

std::uint64_t exactUnsignedInteger(const Json &value, std::string_view path) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) invalidParams(std::string{path} + " must be non-negative");
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        invalidParams(std::string{path} + " must be an integer token");
    }
    if (result > maxExactEditorJsonInteger) {
        invalidParams(std::string{path} + " exceeds 2^53-1");
    }
    return result;
}

OrderedJson schemaFieldJson(
    const StructFieldSchema &field,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    OrderedJson result{{"name", field.name}, {"type", resolver(field.type)}};
    std::visit(
        [&](const auto &range) {
            using Range = std::decay_t<decltype(range)>;
            if constexpr (!std::same_as<Range, std::monostate>) {
                result["range"] = OrderedJson::array({range.first, range.second});
            }
        },
        field.range);
    if (!field.unit.empty()) result["unit"] = field.unit;
    if (field.type == StructFieldType::Enum) {
        result["enum"] = OrderedJson::array();
        for (std::size_t index = 0; index < field.enum_value_count; ++index) {
            result["enum"].push_back(field.enum_values[index]);
        }
    }
    return result;
}

OrderedJson schemaNodeJson(
    const Schema::Node &node,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    const auto tag = [&] {
        switch (node.tag) {
        case Schema::NodeTag::Struct: return "struct";
        case Schema::NodeTag::Leaf: return "leaf";
        case Schema::NodeTag::Flags: return "flags";
        case Schema::NodeTag::List: return "list";
        }
        return "unknown";
    }();
    const auto presence = [&] {
        switch (node.presence) {
        case Schema::FieldPresence::Required: return "required";
        case Schema::FieldPresence::Defaulted: return "defaulted";
        case Schema::FieldPresence::OptionalOverride:
            return "optional_override";
        }
        return "unknown";
    }();
    OrderedJson result{{"tag", tag}, {"name", node.name},
                       {"presence", presence}};
    if (node.leaf) {
        result["type"] = resolver(internal::toCoreSchemaType(node.leaf->type));
        std::visit(
            [&](const auto &range) {
                using Range = std::decay_t<decltype(range)>;
                if constexpr (!std::same_as<Range, std::monostate>)
                    result["range"] =
                        OrderedJson::array({range.min, range.max});
            },
            node.leaf->range);
        if (!node.leaf->unit.empty()) result["unit"] = node.leaf->unit;
    } else if (node.tag == Schema::NodeTag::Flags) {
        result["type"] = "flags";
    } else if (node.tag == Schema::NodeTag::List) {
        result["type"] = "list";
    }
    if (node.default_value)
        result["default"] = OrderedJson::parse(node.default_value->dump());
    result["vocabulary"] = OrderedJson::array();
    for (const auto &value : node.vocabulary) {
        result["vocabulary"].push_back(
            {{"name", value.name}, {"value", value.value},
             {"signed", value.signed_value}});
    }
    result["children"] = OrderedJson::array();
    for (const auto &child : node.children)
        result["children"].push_back(schemaNodeJson(child, resolver));
    return result;
}

EditorCommandRpcAdapter::SchemaTypeNameResolver
schemaTypeResolverOrDefault(
    EditorCommandRpcAdapter::SchemaTypeNameResolver resolver) {
    if (!resolver) {
        resolver = [](StructFieldType type) {
            return internal::schemaTypeName(type);
        };
    }
    return resolver;
}

EditorSceneTreeRequest parseSceneTreeRequest(const Json &params) {
    constexpr auto method = "scene_tree";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"scene_id"}, method);
    return {.scene_id = optionalString(params, "scene_id", method)};
}

EditorGetComponentsRequest parseGetComponentsRequest(
    const Json &params, std::string_view method = "get_components") {
    requireObjectParams(params, method);
    requireOnlyFields(params, {"scene_id", "authoring_object_id", "name"}, method);
    EditorGetComponentsRequest request{.scene_id = optionalString(params, "scene_id", method),
                                       .name = optionalString(params, "name", method)};
    if (const auto found = params.find("authoring_object_id"); found != params.end()) {
        const auto value = exactUnsignedInteger(*found, "get_components authoring_object_id");
        if (value == 0) invalidParams("get_components authoring_object_id must be non-zero");
        request.authoring_object_id = AuthoringObjectId{value};
    }
    if (request.authoring_object_id.has_value() == request.name.has_value()) {
        invalidParams("get_components requires exactly one of authoring_object_id or name");
    }
    return request;
}

EditorListAssetsRequest parseListAssetsRequest(const Json &params) {
    constexpr auto method = "list_assets";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"store"}, method);
    return {.store = optionalString(params, "store", method)};
}

ExportSceneSnapshotRequestV1 parseExportSceneSnapshotRequest(const Json &params) {
    constexpr auto method = "export_scene_snapshot";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"schema_version", "allow_pending"}, method);
    const auto version = params.find("schema_version");
    if (version == params.end()) invalidParams("export_scene_snapshot schema_version is required");
    ExportSceneSnapshotRequestV1 request;
    request.schema_version = exactUnsignedInteger(*version, "export_scene_snapshot schema_version");
    if (const auto allow = params.find("allow_pending"); allow != params.end()) {
        if (!allow->is_boolean()) invalidParams("export_scene_snapshot allow_pending must be a boolean");
        request.allow_pending = allow->get<bool>();
    }
    return request;
}

ExportSceneSnapshotRequestV2 parseExportSceneSnapshotRequestV2(
    const Json &params) {
    const auto common = parseExportSceneSnapshotRequest(params);
    if (common.schema_version != 2) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " +
                std::to_string(common.schema_version)};
    }
    return {.schema_version = common.schema_version,
            .allow_pending = common.allow_pending};
}

DigestV1 parseClosureDigest(const Json &value, std::string_view path,
                            bool require_sha256 = true) {
    if (!value.is_object()) {
        invalidParams(std::string{path} + " must be an object");
    }
    requireOnlyFields(value, {"algorithm", "hex"}, path);
    const auto algorithm = value.find("algorithm");
    const auto hex = value.find("hex");
    if (algorithm == value.end() || !algorithm->is_string() ||
        hex == value.end() || !hex->is_string()) {
        invalidParams(std::string{path} +
                      " requires string algorithm and hex fields");
    }
    if (require_sha256 &&
        algorithm->get_ref<const std::string &>() != "sha256") {
        invalidParams(std::string{path} + " algorithm must be sha256");
    }
    return {.algorithm = algorithm->get<std::string>(),
            .hex = hex->get<std::string>()};
}

std::string requiredClosureString(const Json &value, std::string_view field,
                                  std::string_view path) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        invalidParams(std::string{path} + "/" + std::string{field} +
                      " must be a non-empty string");
    }
    return found->get<std::string>();
}

PrefabResolutionClosure parsePrefabClosure(const Json &value) {
    constexpr auto root = "import_scene_snapshot prefab_closure";
    if (!value.is_object()) invalidParams(std::string{root} + " must be an object");
    requireOnlyFields(value,
                      {"scenes", "prefabs", "behaviors",
                       "provider_fingerprint", "resolver_inputs_fingerprint"},
                      root);
    for (const auto field : {"scenes", "prefabs", "behaviors"}) {
        const auto found = value.find(field);
        if (found == value.end() || !found->is_array()) {
            invalidParams(std::string{root} + "/" + field +
                          " must be an array");
        }
    }
    const auto provider = value.find("provider_fingerprint");
    const auto resolver = value.find("resolver_inputs_fingerprint");
    if (provider == value.end() || resolver == value.end()) {
        invalidParams(std::string{root} +
                      " requires both fingerprint fields");
    }

    PrefabResolutionClosure closure;
    std::string previous;
    for (std::size_t index = 0; index < value.at("scenes").size(); ++index) {
        const auto &entry = value.at("scenes").at(index);
        const auto path = std::string{root} + "/scenes/" +
                          std::to_string(index);
        if (!entry.is_object()) invalidParams(path + " must be an object");
        requireOnlyFields(entry, {"scene_id", "digest"}, path);
        auto scene_id = requiredClosureString(entry, "scene_id", path);
        const auto digest = entry.find("digest");
        if (digest == entry.end()) invalidParams(path + "/digest is required");
        if (!previous.empty() && previous >= scene_id) {
            invalidParams(std::string{root} +
                          "/scenes must be strictly sorted by scene_id");
        }
        previous = scene_id;
        closure.scenes.push_back(
            {.scene_id = std::move(scene_id),
             .digest_sha256 = parseClosureDigest(*digest, path + "/digest").hex});
    }

    previous.clear();
    for (std::size_t index = 0; index < value.at("prefabs").size(); ++index) {
        const auto &entry = value.at("prefabs").at(index);
        const auto path = std::string{root} + "/prefabs/" +
                          std::to_string(index);
        if (!entry.is_object()) invalidParams(path + " must be an object");
        requireOnlyFields(entry, {"name", "digest"}, path);
        auto name = requiredClosureString(entry, "name", path);
        const auto digest = entry.find("digest");
        if (digest == entry.end()) invalidParams(path + "/digest is required");
        if (!previous.empty() && previous >= name) {
            invalidParams(std::string{root} +
                          "/prefabs must be strictly sorted by name");
        }
        previous = name;
        closure.prefabs.push_back(
            {.name = std::move(name),
             .digest_sha256 = parseClosureDigest(*digest, path + "/digest").hex});
    }

    previous.clear();
    for (std::size_t index = 0; index < value.at("behaviors").size(); ++index) {
        const auto &entry = value.at("behaviors").at(index);
        const auto path = std::string{root} + "/behaviors/" +
                          std::to_string(index);
        if (!entry.is_object()) invalidParams(path + " must be an object");
        requireOnlyFields(entry,
                          {"stable_name", "schema_version",
                           "params_schema_fingerprint"},
                          path);
        auto stable_name = requiredClosureString(entry, "stable_name", path);
        const auto version = entry.find("schema_version");
        if (version == entry.end()) {
            invalidParams(path + "/schema_version is required");
        }
        const auto schema_version = exactUnsignedInteger(
            *version, path + "/schema_version");
        if (schema_version > std::numeric_limits<std::uint32_t>::max()) {
            invalidParams(path + "/schema_version exceeds u32");
        }
        auto fingerprint = requiredClosureString(
            entry, "params_schema_fingerprint", path);
        if (!previous.empty() && previous >= stable_name) {
            invalidParams(std::string{root} +
                          "/behaviors must be strictly sorted by stable_name");
        }
        previous = stable_name;
        closure.behaviors.push_back(
            {.stable_name = std::move(stable_name),
             .schema_version = static_cast<std::uint32_t>(schema_version),
             .params_schema_fingerprint = std::move(fingerprint)});
    }
    closure.provider_fingerprint_sha256 =
        parseClosureDigest(*provider,
                           std::string{root} + "/provider_fingerprint")
            .hex;
    closure.resolver_inputs_fingerprint_sha256 =
        parseClosureDigest(*resolver,
                           std::string{root} +
                               "/resolver_inputs_fingerprint")
            .hex;
    return closure;
}

std::optional<std::string> closureMismatchDetail(
    const PrefabResolutionClosure &expected,
    const PrefabResolutionClosure &actual) {
    if (expected.scenes.size() != actual.scenes.size())
        return "/prefab_closure/scenes/" +
               std::to_string(std::min(expected.scenes.size(),
                                       actual.scenes.size()));
    for (std::size_t index = 0; index < expected.scenes.size(); ++index) {
        const auto root = "/prefab_closure/scenes/" + std::to_string(index);
        if (expected.scenes[index].scene_id != actual.scenes[index].scene_id)
            return root + "/scene_id";
        if (expected.scenes[index].digest_sha256 !=
            actual.scenes[index].digest_sha256)
            return root + "/digest";
    }
    if (expected.prefabs.size() != actual.prefabs.size())
        return "/prefab_closure/prefabs/" +
               std::to_string(std::min(expected.prefabs.size(),
                                       actual.prefabs.size()));
    for (std::size_t index = 0; index < expected.prefabs.size(); ++index) {
        const auto root = "/prefab_closure/prefabs/" + std::to_string(index);
        if (expected.prefabs[index].name != actual.prefabs[index].name)
            return root + "/name";
        if (expected.prefabs[index].digest_sha256 !=
            actual.prefabs[index].digest_sha256)
            return root + "/digest";
    }
    if (expected.behaviors.size() != actual.behaviors.size())
        return "/prefab_closure/behaviors/" +
               std::to_string(std::min(expected.behaviors.size(),
                                       actual.behaviors.size()));
    for (std::size_t index = 0; index < expected.behaviors.size(); ++index) {
        const auto root = "/prefab_closure/behaviors/" +
                          std::to_string(index);
        if (expected.behaviors[index].stable_name !=
            actual.behaviors[index].stable_name)
            return root + "/stable_name";
        if (expected.behaviors[index].schema_version !=
            actual.behaviors[index].schema_version)
            return root + "/schema_version";
        if (expected.behaviors[index].params_schema_fingerprint !=
            actual.behaviors[index].params_schema_fingerprint)
            return root + "/params_schema_fingerprint";
    }
    if (expected.provider_fingerprint_sha256 !=
        actual.provider_fingerprint_sha256)
        return "/prefab_closure/provider_fingerprint";
    if (expected.resolver_inputs_fingerprint_sha256 !=
        actual.resolver_inputs_fingerprint_sha256)
        return "/prefab_closure/resolver_inputs_fingerprint";
    return std::nullopt;
}

ImportSceneSnapshotRequestV1 parseImportSceneSnapshotRequest(const Json &params) {
    constexpr auto method = "import_scene_snapshot";
    requireObjectParams(params, method);

    // Preserve the normative validation order even when later fields are
    // malformed. A shaped request with an unsupported version must never
    // inspect or allocate its payload.
    const auto version = params.find("schema_version");
    if (version == params.end()) {
        invalidParams("import_scene_snapshot schema_version is required");
    }
    const auto schema_version =
        exactUnsignedInteger(*version, "import_scene_snapshot schema_version");
    if (schema_version != 1) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " + std::to_string(schema_version)};
    }

    requireOnlyFields(params,
                      {"schema_version", "semantic_scene_bytes", "digest",
                       "current_scene_id"},
                      method);
    const auto bytes = params.find("semantic_scene_bytes");
    if (bytes == params.end() || !bytes->is_string()) {
        invalidParams(
            "import_scene_snapshot semantic_scene_bytes must be a string");
    }
    if (bytes->get_ref<const std::string &>().size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }

    const auto digest = params.find("digest");
    if (digest == params.end() || !digest->is_object()) {
        invalidParams("import_scene_snapshot digest must be an object");
    }
    requireOnlyFields(*digest, {"algorithm", "hex"},
                      "import_scene_snapshot digest");
    const auto algorithm = digest->find("algorithm");
    const auto hex = digest->find("hex");
    if (algorithm == digest->end() || !algorithm->is_string() ||
        hex == digest->end() || !hex->is_string()) {
        invalidParams(
            "import_scene_snapshot digest requires string algorithm and hex fields");
    }

    const auto current_scene =
        optionalString(params, "current_scene_id", method);
    if (!current_scene) {
        invalidParams("import_scene_snapshot current_scene_id is required");
    }
    return ImportSceneSnapshotRequestV1{
        .schema_version = schema_version,
        .semantic_scene_bytes = bytes->get<std::string>(),
        .digest = {.algorithm = algorithm->get<std::string>(),
                   .hex = hex->get<std::string>()},
        .current_scene_id = *current_scene,
    };
}

ImportSceneSnapshotRequestV2 parseImportSceneSnapshotRequestV2(
    const Json &params) {
    constexpr auto method = "import_scene_snapshot";
    requireObjectParams(params, method);
    const auto version = params.find("schema_version");
    if (version == params.end()) {
        invalidParams("import_scene_snapshot schema_version is required");
    }
    const auto schema_version =
        exactUnsignedInteger(*version, "import_scene_snapshot schema_version");
    if (schema_version != 2) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " +
                std::to_string(schema_version)};
    }
    requireOnlyFields(params,
                      {"schema_version", "semantic_scene_bytes", "digest",
                       "current_scene_id", "prefab_closure"},
                      method);
    const auto bytes = params.find("semantic_scene_bytes");
    if (bytes == params.end() || !bytes->is_string()) {
        invalidParams(
            "import_scene_snapshot semantic_scene_bytes must be a string");
    }
    if (bytes->get_ref<const std::string &>().size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }
    const auto digest = params.find("digest");
    if (digest == params.end()) {
        invalidParams("import_scene_snapshot digest is required");
    }
    // DigestV1's algorithm is gate 3, not request-shape validation. This is
    // the V1 parser behavior and preserves the five-gate error precedence.
    const auto parsed_digest = parseClosureDigest(
        *digest, "import_scene_snapshot digest", false);
    const auto current_scene = optionalString(params, "current_scene_id", method);
    if (!current_scene) {
        invalidParams("import_scene_snapshot current_scene_id is required");
    }
    const auto closure = params.find("prefab_closure");
    if (closure == params.end()) {
        invalidParams("import_scene_snapshot prefab_closure is required");
    }
    return ImportSceneSnapshotRequestV2{
        .schema_version = schema_version,
        .semantic_scene_bytes = bytes->get<std::string>(),
        .digest = parsed_digest,
        .current_scene_id = *current_scene,
        .prefab_closure = parsePrefabClosure(*closure),
    };
}

void validateSaveSceneRequest(const Json &params) {
    constexpr auto method = "save_scene";
    requireObjectParams(params, method);
    requireOnlyFields(params, {}, method);
}

bool scenePayloadContainsPrefabKey(const Json &document) {
    const auto scenes = document.find("scenes");
    if (scenes == document.end() || !scenes->is_object()) return false;
    for (const auto &[unused, scene] : scenes->items()) {
        (void)unused;
        if (!scene.is_object()) continue;
        const auto objects = scene.find("objects");
        if (objects == scene.end() || !objects->is_array()) continue;
        for (const auto &object : *objects) {
            if (object.is_object() && object.contains("prefab")) return true;
        }
    }
    return false;
}

} // namespace

std::string_view editorCommandErrorCodeName(EditorCommandErrorCode code) noexcept {
    switch (code) {
    case EditorCommandErrorCode::InvalidParams: return "invalid_params";
    case EditorCommandErrorCode::SceneNotFound: return "scene_not_found";
    case EditorCommandErrorCode::ObjectNotFound: return "object_not_found";
    case EditorCommandErrorCode::UnsupportedSnapshotVersion: return "unsupported_snapshot_version";
    case EditorCommandErrorCode::SnapshotBusy: return "snapshot_busy";
    case EditorCommandErrorCode::SnapshotTooLarge: return "snapshot_too_large";
    case EditorCommandErrorCode::DigestMismatch: return "digest_mismatch";
    case EditorCommandErrorCode::SnapshotInvalid: return "snapshot_invalid";
    case EditorCommandErrorCode::ExternalModification: return "external_modification";
    case EditorCommandErrorCode::SaveBusy: return "save_busy";
    case EditorCommandErrorCode::RuntimeOnlyData: return "runtime_only_data";
    case EditorCommandErrorCode::ResolvedSceneProviderUnavailable:
        return "resolved_scene_provider_unavailable";
    case EditorCommandErrorCode::SaveUnavailable: return "save_unavailable";
    case EditorCommandErrorCode::SaveFailed: return "save_failed";
    case EditorCommandErrorCode::PrefabPersistenceUnsupported:
        return "prefab_persistence_unsupported";
    case EditorCommandErrorCode::PrefabGeneratedReadOnly:
        return "prefab_generated_read_only";
    case EditorCommandErrorCode::PrefabDependencyMismatch:
        return "prefab_dependency_mismatch";
    }
    return "unknown_editor_error";
}

EditorCommandError::EditorCommandError(EditorCommandErrorCode code,
                                       const std::string &message,
                                       std::optional<std::string> detail)
    : std::runtime_error{message}, code_{code}, detail_{std::move(detail)} {}

EditorCommandService::EditorCommandService(EditorCommandServiceDependencies dependencies)
    : dependencies_{std::move(dependencies)},
      render_config_editor_{
          std::move(dependencies_.render_config_editor)},
      procedural_asset_editor_{std::move(dependencies_.procedural_asset_editor)} {
    if (!dependencies_.document || !dependencies_.current_scene_id) {
        throw std::invalid_argument("EditorCommandService requires document and current_scene_id providers");
    }
    if (dependencies_.edit) {
        if (render_config_editor_ || procedural_asset_editor_) {
            auto previous = std::move(
                dependencies_.edit->frame_boundary_extension);
            const auto render_config_editor =
                render_config_editor_;
            const auto asset_editor = procedural_asset_editor_;
            dependencies_.edit->frame_boundary_extension =
                [this, previous = std::move(previous),
                 render_config_editor, asset_editor] {
                    if (previous) previous();
                    if (render_config_editor) render_config_editor->commitPending();
                    if (asset_editor && !edit_->hasOpenPreviewLease()) asset_editor->commitPending();
                };
        }
        edit_ = std::make_unique<EditorEditCoordinator>(
            std::move(*dependencies_.edit));
        dependencies_.edit.reset();
    }
    if (dependencies_.preview) {
        auto preview_dependencies = std::move(*dependencies_.preview);
        const auto runtime_snapshot =
            std::move(preview_dependencies.shared_state_snapshot);
        preview_dependencies.shared_state_snapshot =
            [this, runtime_snapshot]() -> OrderedJson {
                auto result = runtime_snapshot();
                auto journal = OrderedJson::array();
                auto pending_tickets = OrderedJson::array();
                auto preview_epoch = std::uint64_t{};
                auto open_preview_lease = false;
                if (edit_) {
                    for (const auto &record : edit_->journal()) {
                        journal.push_back(editorJournalJson(record));
                    }
                    for (const auto &ticket : edit_->pendingTicketIds()) {
                        pending_tickets.push_back(ticket);
                    }
                    preview_epoch = edit_->previewEpoch();
                    open_preview_lease = edit_->hasOpenPreviewLease();
                }
                result["journal"] = std::move(journal);
                result["editor_coordinator"] = {
                    {"pending_ticket_ids", std::move(pending_tickets)},
                    {"preview_epoch", preview_epoch},
                    {"open_preview_lease", open_preview_lease},
                };
                return result;
            };
        preview_ = std::make_unique<EditorPreviewService>(
            std::move(preview_dependencies));
        dependencies_.preview.reset();
    }
}

EditorCommandService::~EditorCommandService() = default;

const AuthoringSceneDocument &EditorCommandService::document() const {
    return dependencies_.document();
}

const ResolvedScene &EditorCommandService::resolved() const {
    if (!dependencies_.resolved_scene) {
        throw EditorCommandError{
            EditorCommandErrorCode::ResolvedSceneProviderUnavailable,
            "resolved_scene_provider_unavailable: EditorCommandService requires an explicitly wired resolved_scene provider"};
    }
    return dependencies_.resolved_scene();
}

const ResolvedSceneView &
EditorCommandService::selectScene(std::span<const ResolvedSceneView> scenes,
                                  const std::optional<std::string> &requested_scene) const {
    const auto scene_id = requested_scene.value_or(dependencies_.current_scene_id());
    const auto found = std::find_if(scenes.begin(), scenes.end(),
                                    [&](const auto &scene) { return scene.scene_id == scene_id; });
    if (found == scenes.end()) {
        throw EditorCommandError{EditorCommandErrorCode::SceneNotFound,
                                 "scene not found: " + scene_id};
    }
    return *found;
}

EditorObjectQueryResult EditorCommandService::queryObject(
    const ResolvedScene &source, const ResolvedSceneView &scene,
    const ResolvedObject &object) const {
    EditorRuntimeObjectState runtime;
    if (dependencies_.runtime_query) runtime = dependencies_.runtime_query(scene, object);
    if (!runtime.component_runtime_json.empty() &&
        runtime.component_runtime_json.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a component count mismatch");
    }
    if (!runtime.component_pending.empty() && runtime.component_pending.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a pending count mismatch");
    }
    if (!runtime.behavior_attachments.empty() &&
        runtime.behavior_attachments.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a behavior attachment count mismatch");
    }

    EditorObjectQueryResult result{.scene_revision = source.revision(),
                                   .authoring_object_id = object.authoring_object_id,
                                   .declaration_index = object.authoring_object_index,
                                   .name = object.name,
                                   .parent = object.parent,
                                   .entity_id = runtime.entity_id,
                                   .prefab_instance = object.prefab_instance};
    result.components.reserve(object.components.size());
    for (std::size_t index = 0; index < object.components.size(); ++index) {
        const auto &component = object.components[index];
        const auto &name = component.name;
        EditorComponentQueryResult component_result{
            .name = name,
            .component_index = component.origin.isGenerated()
                                   ? component.merged_component_index
                                   : component.authoring_component_index,
            .authored_json = component.source_json_exact,
            .editable = component.codec.editable,
            .codec_state = component.codec.state,
            .codec_name = std::string{component.codec.codec_name},
            .pending = !runtime.component_pending.empty() && runtime.component_pending[index],
            .resolution_failure = component.runtime_resolution_error,
        };
        if (const auto *generated = component.origin.generated()) {
            component_result.generated = *generated;
            component_result.generated_resolved_json = component.effective_json;
            component_result.editable = false;
            component_result.schema_state = EditorComponentSchemaState::Missing;
        }
        if (!runtime.component_runtime_json.empty()) {
            component_result.runtime_json = runtime.component_runtime_json[index];
        }
        if (name == "behavior") {
            const auto stable_name = component.effective_json.value(
                "type", std::string{});
            const auto *registration =
                internal::getBehaviorRegisterer().findByName(stable_name);
            if (!runtime.behavior_attachments.empty() &&
                runtime.behavior_attachments[index]) {
                const auto &attachment = *runtime.behavior_attachments[index];
                component_result.behavior_attachment_handle = attachment.handle;
                component_result.behavior_attachment_seq = attachment.attachment_seq;
                component_result.behavior_owner = attachment.owner;
                component_result.behavior_owner_generation =
                    attachment.owner_generation;
                component_result.pending = attachment.pending;
            }
            if (!component.origin.isGenerated() && registration != nullptr &&
                !component_result.pending &&
                component.behavior_canonical_params) {
                component_result.editable = true;
                component_result.codec_state = ComponentCodecState::Registered;
                component_result.codec_name = "behavior_params";
                component_result.schema_state =
                    EditorComponentSchemaState::Available;
                component_result.schema_fields = registration->params_schema;
                if (const auto *node = internal::getBehaviorRegisterer()
                                           .schemaFor(*registration))
                    component_result.schema_node = *node;
            } else {
                component_result.editable = false;
            }
        } else if (const auto *codec = findComponentCodec(name)) {
            component_result.schema_state = EditorComponentSchemaState::Available;
            const auto fields = codec->fieldSchema();
            component_result.schema_fields.assign(fields.begin(), fields.end());
        } else if (const auto *manager =
                       FastModuleContainer::tryGet<ComponentInfoManager>();
                   manager != nullptr && manager->hasComponent(name)) {
            const auto &registration = manager->get(
                manager->getComponentIdByName(name));
            if (registration.schema_v3) {
                component_result.schema_state =
                    EditorComponentSchemaState::Available;
                component_result.schema_node = *registration.schema_v3;
            }
        }
        result.components.push_back(std::move(component_result));
    }
    return result;
}

EditorSceneTreeResult EditorCommandService::sceneTree(const EditorSceneTreeRequest &request) const {
    const auto &source = resolved();
    const auto &scene = selectScene(source.scenes(), request.scene_id);
    EditorSceneTreeResult result{.scene_revision = source.revision(), .scene_id = scene.scene_id};
    result.objects.reserve(scene.objects.size());
    for (const auto &object : scene.objects) result.objects.push_back(queryObject(source, scene, object));
    return result;
}

EditorSceneRevisionResult EditorCommandService::getSceneRevision() const {
    if (edit_) synchronizePreviewWatch();
    const auto revision = resolved().revision();
    if (revision.value > maxExactEditorJsonInteger) {
        throw std::overflow_error("SceneRevision exceeds 2^53-1");
    }

    auto preview_epoch = std::uint64_t{};
    if (dependencies_.snapshot_state) {
        preview_epoch = dependencies_.snapshot_state().preview_epoch;
    }
    if (edit_) preview_epoch = std::max(preview_epoch, edit_->previewEpoch());
    if (preview_epoch > maxExactEditorJsonInteger) {
        throw std::overflow_error("preview_epoch exceeds 2^53-1");
    }

    EditorSceneRevisionResult result{
        .token = {.scene_revision = revision, .preview_epoch = preview_epoch},
    };
    if (edit_ && !edit_->journal().empty()) {
        const auto &record = edit_->journal().back();
        result.last_transaction = EditorLastTransactionResult{
            .actor_id = record.actor_id,
            .display_name = record.actor_display_name,
            .affected_authoring_ids = record.affected_authoring_ids,
        };
    }
    if (edit_ && edit_->hasOpenPreviewLease() && preview_lease_) {
        result.preview_lease = preview_lease_;
    }
    return result;
}

EditorObjectQueryResult
EditorCommandService::getComponents(const EditorGetComponentsRequest &request) const {
    if (request.authoring_object_id.has_value() == request.name.has_value()) {
        throw EditorCommandError{EditorCommandErrorCode::InvalidParams,
                                 "get_components requires exactly one object selector"};
    }
    const auto &source = resolved();
    const auto &scene = selectScene(source.scenes(), request.scene_id);
    const auto found = std::find_if(scene.objects.begin(), scene.objects.end(), [&](const auto &object) {
        if (request.authoring_object_id) return object.authoring_object_id == *request.authoring_object_id;
        return object.name && *object.name == *request.name;
    });
    if (found == scene.objects.end()) {
        throw EditorCommandError{EditorCommandErrorCode::ObjectNotFound,
                                 "object not found in scene: " + scene.scene_id};
    }
    return queryObject(source, scene, *found);
}

OrderedJson EditorCommandService::getComponentAddCandidates(
    const EditorGetComponentsRequest &request) const {
    const auto object = getComponents(request);
    auto candidates = OrderedJson::array();
    const auto assets = dependencies_.assets ? dependencies_.assets() : std::vector<EditorAssetQueryResult>{};
    const auto schema_type_name = schemaTypeResolverOrDefault({});
    for (const auto &codec : componentCodecs()) {
        if (codec.name == "behavior" || std::any_of(object.components.begin(), object.components.end(),
                [&](const auto &component) { return component.name == codec.name; })) continue;
        OrderedJson candidate{{"key", codec.name}, {"name", codec.name}, {"available", false}};
        try {
            auto decoded = codec.decodeAuthored(Json{{"name", codec.name}});
            if (decoded) {
                candidate["component"] = codec.encodeCanonical(decoded.value());
                candidate["available"] = true;
            } else {
                candidate["unavailable_reason"] = decoded.error().leaf_message;
            }
        } catch (const LegacyRejectException &error) {
            candidate["unavailable_reason"] = error.what();
        }
        candidate["component_template"] = candidate.value("component", OrderedJson{{"name", codec.name}});
        candidate["schema_fields"] = OrderedJson::array();
        for (const auto &field : codec.fieldSchema())
            candidate["schema_fields"].push_back(schemaFieldJson(field, schema_type_name));
        std::string_view asset_kind, asset_field;
        if (codec.name == "simplemodelview") { asset_kind = "model"; asset_field = "model"; }
        else if (codec.name == "sprite_view") { asset_kind = "texture"; asset_field = "texture"; }
        else if (codec.name == "pelican_procedural_geometry") { asset_kind = "procedural_geometry"; asset_field = "assets"; }
        if (!asset_kind.empty()) {
            const bool multiple = codec.name == "pelican_procedural_geometry";
            OrderedJson selection{{"field", asset_field}, {"multiple", multiple}, {"choices", OrderedJson::array()}};
            if (multiple) selection["single_field"] = "asset";
            for (const auto &asset : assets)
                if (asset.kind == asset_kind)
                    selection["choices"].push_back({{"id", asset.id}, {"path", asset.path}, {"status", asset.status}});
            candidate["asset_selection"] = std::move(selection);
        }
        candidates.push_back(std::move(candidate));
    }
    const auto &behaviors = internal::getBehaviorRegisterer();
    for (const auto &registration : behaviors.registeredBehaviors()) {
        // Match the registration that scene decoding would actually select.
        if (behaviors.findByName(registration.stable_name) != &registration) continue;
        OrderedJson declaration{{"name", "behavior"}, {"type", registration.stable_name},
                                {"params", OrderedJson::object()}};
        OrderedJson candidate{{"key", "behavior:" + registration.stable_name},
            {"name", "behavior"}, {"behavior_type", registration.stable_name},
            {"available", false}, {"schema_fields", OrderedJson::array()}};
        try {
            declaration["params"] = OrderedJson::parse(registration.canonicalize_params(Json::object()));
            candidate["available"] = true;
            candidate["component"] = declaration;
        } catch (const std::exception &error) {
            candidate["unavailable_reason"] = error.what();
        }
        candidate["component_template"] = std::move(declaration);
        for (const auto &field : registration.params_schema)
            candidate["schema_fields"].push_back(schemaFieldJson(field, schema_type_name));
        if (const auto *node = behaviors.schemaFor(registration))
            candidate["schema_node"] = schemaNodeJson(*node, schema_type_name);
        candidates.push_back(std::move(candidate));
    }
    return {{"object_id", object.authoring_object_id.value},
            {"scene_revision", object.scene_revision.value}, {"candidates", std::move(candidates)}};
}

EditorRayOccurrencesResult
EditorCommandService::getRayOccurrences(
    const EditorGetComponentsRequest &request) const {
    if (!dependencies_.ray_occurrences) return {};
    const auto object = getComponents(request);
    return dependencies_.ray_occurrences(object.authoring_object_id.value);
}

RayOccurrenceResolution EditorCommandService::resolveRayOccurrence(
    std::string_view occurrence_id) const {
    if (!dependencies_.resolve_ray_occurrence) return {};
    return dependencies_.resolve_ray_occurrence(occurrence_id);
}

EditorListAssetsResult EditorCommandService::listAssets(const EditorListAssetsRequest &request) const {
    EditorListAssetsResult result;
    if (!dependencies_.assets) return result;
    for (auto asset : dependencies_.assets()) {
        if (!request.store || asset.store == *request.store) result.assets.push_back(std::move(asset));
    }
    return result;
}

ExportSceneSnapshotResponseV1
EditorCommandService::exportSceneSnapshot(const ExportSceneSnapshotRequestV1 &request) const {
    if (document().formatVersion() != 1 || document().usesPrefabs()) {
        throw EditorCommandError{
            EditorCommandErrorCode::PrefabPersistenceUnsupported,
            "prefab_persistence_unsupported: SnapshotV1 requires formatVersion 1 without prefabs"};
    }
    if (request.schema_version != 1) {
        throw EditorCommandError{EditorCommandErrorCode::UnsupportedSnapshotVersion,
                                 "unsupported snapshot version: " +
                                     std::to_string(request.schema_version)};
    }
    auto snapshot_state = dependencies_.snapshot_state ? dependencies_.snapshot_state()
                                                        : EditorSnapshotState{};
    if (edit_) {
        auto pending = edit_->pendingTicketIds();
        snapshot_state.pending_ticket_ids.insert(
            snapshot_state.pending_ticket_ids.end(), pending.begin(), pending.end());
        snapshot_state.open_preview_lease =
            snapshot_state.open_preview_lease || edit_->hasOpenPreviewLease();
        snapshot_state.preview_epoch =
            std::max(snapshot_state.preview_epoch, edit_->previewEpoch());
    }
    if (snapshot_state.preview_epoch > maxExactEditorJsonInteger) {
        throw std::overflow_error("preview_epoch exceeds 2^53-1");
    }
    if (!request.allow_pending &&
        (!snapshot_state.pending_ticket_ids.empty() || snapshot_state.open_preview_lease)) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotBusy,
                                 "snapshot export is busy"};
    }

    const auto &source = document();
    if (source.revision().value > maxExactEditorJsonInteger) {
        throw std::overflow_error("SceneRevision exceeds 2^53-1");
    }
    const auto current_scene = dependencies_.current_scene_id();
    if (resolved().findScene(current_scene) == nullptr) {
        throw EditorCommandError{EditorCommandErrorCode::SceneNotFound,
                                 "scene not found: " + current_scene};
    }
    auto bytes = internal::encodeAuthoringSceneSemantic(source);
    if (bytes.size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }

    auto digest = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
    return ExportSceneSnapshotResponseV1{
        .schema_version = 1,
        .scene_revision = source.revision(),
        .current_scene_id = current_scene,
        .semantic_scene_bytes = std::move(bytes),
        .digest = {.algorithm = "sha256", .hex = std::move(digest)},
        .pending_ticket_ids = snapshot_state.pending_ticket_ids,
        .preview_epoch = snapshot_state.preview_epoch,
    };
}

ExportSceneSnapshotResponseV2
EditorCommandService::exportSceneSnapshotV2(
    const ExportSceneSnapshotRequestV2 &request) const {
    if (request.schema_version != 2) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " +
                std::to_string(request.schema_version)};
    }
    auto snapshot_state = dependencies_.snapshot_state
                              ? dependencies_.snapshot_state()
                              : EditorSnapshotState{};
    if (edit_) {
        auto pending = edit_->pendingTicketIds();
        snapshot_state.pending_ticket_ids.insert(
            snapshot_state.pending_ticket_ids.end(), pending.begin(),
            pending.end());
        snapshot_state.open_preview_lease =
            snapshot_state.open_preview_lease || edit_->hasOpenPreviewLease();
        snapshot_state.preview_epoch =
            std::max(snapshot_state.preview_epoch, edit_->previewEpoch());
    }
    if (snapshot_state.preview_epoch > maxExactEditorJsonInteger) {
        throw std::overflow_error("preview_epoch exceeds 2^53-1");
    }
    if (!request.allow_pending &&
        (!snapshot_state.pending_ticket_ids.empty() ||
         snapshot_state.open_preview_lease)) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotBusy,
                                 "snapshot export is busy"};
    }

    const auto &source = document();
    const auto &projection = resolved();
    if (source.revision().value > maxExactEditorJsonInteger) {
        throw std::overflow_error("SceneRevision exceeds 2^53-1");
    }
    const auto current_scene = dependencies_.current_scene_id();
    if (projection.findScene(current_scene) == nullptr) {
        throw EditorCommandError{EditorCommandErrorCode::SceneNotFound,
                                 "scene not found: " + current_scene};
    }
    auto bytes = internal::encodeAuthoringSceneSemantic(source);
    if (bytes.size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }
    auto digest = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
    return ExportSceneSnapshotResponseV2{
        .schema_version = 2,
        .scene_revision = source.revision(),
        .current_scene_id = current_scene,
        .semantic_scene_bytes = std::move(bytes),
        .digest = {.algorithm = "sha256", .hex = std::move(digest)},
        .pending_ticket_ids = snapshot_state.pending_ticket_ids,
        .preview_epoch = snapshot_state.preview_epoch,
        .prefab_closure = projection.prefabClosure(),
    };
}

ImportSceneSnapshotResult EditorCommandService::importSceneSnapshot(
    const ImportSceneSnapshotRequestV1 &request) {
    if (document().formatVersion() != 1 || document().usesPrefabs()) {
        throw EditorCommandError{
            EditorCommandErrorCode::PrefabPersistenceUnsupported,
            "prefab_persistence_unsupported: SnapshotV1 requires a formatVersion 1 live scene without prefabs"};
    }
    // Incoming prefab data is routed before the request-version decision so
    // a V2-shaped closure cannot escape through SnapshotV1's version gate.
    try {
        if (scenePayloadContainsPrefabKey(
                nlohmann::json::parse(request.semantic_scene_bytes))) {
            throw EditorCommandError{
                EditorCommandErrorCode::PrefabPersistenceUnsupported,
                "prefab_persistence_unsupported: snapshot import contains prefab instances"};
        }
    } catch (const EditorCommandError &) {
        throw;
    } catch (const std::exception &) {
        // Preserve SNAPSHOT0's later named snapshot-invalid boundary.
    }
    // SNAPSHOT0 validation order is part of the wire contract. Do not merge
    // these gates or move parsing ahead of the digest check.
    if (request.schema_version != 1) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " +
                std::to_string(request.schema_version)};
    }
    if (request.semantic_scene_bytes.size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }

    const auto actual_digest = picosha2::hash256_hex_string(
        request.semantic_scene_bytes.begin(),
        request.semantic_scene_bytes.end());
    if (request.digest.algorithm != "sha256" ||
        request.digest.hex != actual_digest) {
        throw EditorCommandError{EditorCommandErrorCode::DigestMismatch,
                                 "snapshot digest does not match semantic_scene_bytes"};
    }
    AuthoringSceneDocument validated;
    try {
        validated = AuthoringSceneDocument::load(
            request.semantic_scene_bytes, SceneRevision{1});
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotInvalid,
                                 "snapshot parse or semantic validation failed",
                                 error.what()};
    }
    if (validated.formatVersion() != 1 || validated.usesPrefabs()) {
        throw EditorCommandError{
            EditorCommandErrorCode::PrefabPersistenceUnsupported,
            "prefab_persistence_unsupported: SnapshotV1 payload requires formatVersion 1 without prefabs"};
    }
    if (!internal::authoringSceneContains(validated,
                                          request.current_scene_id)) {
        throw EditorCommandError{
            EditorCommandErrorCode::SceneNotFound,
            "scene not found: " + request.current_scene_id};
    }

    if (!dependencies_.import_scene_snapshot) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotInvalid,
                                 "snapshot import is unavailable",
                                 "runtime import surface is unavailable"};
    }
    try {
        auto imported = dependencies_.import_scene_snapshot(
            request.semantic_scene_bytes, request.current_scene_id);
        const auto revision = imported.revision;
        if (revision.value > maxExactEditorJsonInteger) {
            throw std::overflow_error("SceneRevision exceeds 2^53-1");
        }
        return ImportSceneSnapshotResult{
            .scene_revision = revision,
            .current_scene_id = request.current_scene_id,
            .effect_errors = std::holds_alternative<CommittedWithEffectFailure>(
                                 imported.load_result)
                ? std::move(std::get<CommittedWithEffectFailure>(
                                imported.load_result).effect_errors)
                : std::vector<CommittedWithEffectFailure::EffectError>{},
            .diagnostics = std::holds_alternative<CommittedWithEffectFailure>(
                               imported.load_result)
                               ? std::get<CommittedWithEffectFailure>(
                                     imported.load_result)
                                     .diagnostics
                               : std::get<SceneLoadCommitted>(
                                     imported.load_result)
                                     .diagnostics,
        };
    } catch (const EditorCommandError &) {
        throw;
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotInvalid,
                                 "snapshot reload failed", error.what()};
    }
}

ImportSceneSnapshotResult EditorCommandService::importSceneSnapshotV2(
    const ImportSceneSnapshotRequestV2 &request) {
    // SnapshotV2 retains SNAPSHOT0's five gates and inserts closure matching
    // after parse/semantic validation but before current-scene selection.
    if (request.schema_version != 2) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " +
                std::to_string(request.schema_version)};
    }
    if (request.semantic_scene_bytes.size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }
    const auto actual_digest = picosha2::hash256_hex_string(
        request.semantic_scene_bytes.begin(),
        request.semantic_scene_bytes.end());
    if (request.digest.algorithm != "sha256" ||
        request.digest.hex != actual_digest) {
        throw EditorCommandError{
            EditorCommandErrorCode::DigestMismatch,
            "snapshot digest does not match semantic_scene_bytes"};
    }

    AuthoringSceneDocument validated;
    ResolvedScene validated_projection;
    try {
        validated = AuthoringSceneDocument::load(
            request.semantic_scene_bytes, SceneRevision{1});
        const auto &live = resolved();
        validated_projection = ResolvedSceneResolver::resolve(
            validated, SceneResolverGeneration{1}, live.defaults(),
            live.prefabRegistry(), live.bindableProvider());
    } catch (const PrefabError &error) {
        // Resolution participates in gate 4 (parse/semantic validation), but
        // it also needs the receiving project's registry/provider in order to
        // construct the closure checked by gate 5'. Keep payload violations
        // at snapshot_invalid; only failures whose missing input belongs to
        // the receiving project are dependency mismatches.
        const bool dependency_failure =
            error.code() == PrefabErrorCode::PrefabNotFound ||
            error.code() == PrefabErrorCode::PrefabBindingNotBindable ||
            error.code() == PrefabErrorCode::PrefabBindingInactive ||
            error.code() == PrefabErrorCode::PrefabProviderStale ||
            error.code() == PrefabErrorCode::PrefabDependencyMismatch;
        const auto detail = error.context().json_pointer.value_or(
            dependency_failure ? std::string{"/prefab_closure"}
                               : std::string{"/prefab"});
        throw EditorCommandError{
            dependency_failure
                ? EditorCommandErrorCode::PrefabDependencyMismatch
                : EditorCommandErrorCode::SnapshotInvalid,
            dependency_failure
                ? "prefab_dependency_mismatch: snapshot dependencies cannot be resolved"
                : "snapshot prefab semantic validation failed",
            detail};
    } catch (const EditorCommandError &) {
        throw;
    } catch (const std::exception &error) {
        throw EditorCommandError{
            EditorCommandErrorCode::SnapshotInvalid,
            "snapshot parse or semantic validation failed", error.what()};
    }
    if (const auto detail = closureMismatchDetail(
            request.prefab_closure, validated_projection.prefabClosure())) {
        throw EditorCommandError{
            EditorCommandErrorCode::PrefabDependencyMismatch,
            "prefab_dependency_mismatch: snapshot closure differs from the current project",
            *detail};
    }
    if (!internal::authoringSceneContains(validated,
                                          request.current_scene_id)) {
        throw EditorCommandError{
            EditorCommandErrorCode::SceneNotFound,
            "scene not found: " + request.current_scene_id};
    }
    if (!dependencies_.import_scene_snapshot_versioned) {
        throw EditorCommandError{
            EditorCommandErrorCode::SnapshotInvalid,
            "snapshot import is unavailable",
            "runtime SnapshotV2 import surface is unavailable"};
    }
    try {
        auto imported = dependencies_.import_scene_snapshot_versioned(
            request.semantic_scene_bytes, request.current_scene_id, 2);
        const auto revision = imported.revision;
        if (revision.value > maxExactEditorJsonInteger) {
            throw std::overflow_error("SceneRevision exceeds 2^53-1");
        }
        return {.scene_revision = revision,
                .current_scene_id = request.current_scene_id,
                .effect_errors =
                    std::holds_alternative<CommittedWithEffectFailure>(
                        imported.load_result)
                    ? std::move(std::get<CommittedWithEffectFailure>(
                                    imported.load_result).effect_errors)
                    : std::vector<CommittedWithEffectFailure::EffectError>{},
                .diagnostics =
                    std::holds_alternative<CommittedWithEffectFailure>(
                        imported.load_result)
                        ? std::get<CommittedWithEffectFailure>(
                              imported.load_result)
                              .diagnostics
                        : std::get<SceneLoadCommitted>(imported.load_result)
                              .diagnostics};
    } catch (const EditorCommandError &) {
        throw;
    } catch (const PrefabError &error) {
        throw EditorCommandError{
            error.code() == PrefabErrorCode::PrefabDependencyMismatch
                ? EditorCommandErrorCode::PrefabDependencyMismatch
                : EditorCommandErrorCode::SnapshotInvalid,
            error.what(), error.context().json_pointer};
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotInvalid,
                                 "snapshot reload failed", error.what()};
    }
}

SaveSceneResult EditorCommandService::saveScene() {
    auto state = dependencies_.snapshot_state ? dependencies_.snapshot_state()
                                               : EditorSnapshotState{};
    if (edit_) {
        auto pending = edit_->pendingTicketIds();
        state.pending_ticket_ids.insert(state.pending_ticket_ids.end(),
                                        pending.begin(), pending.end());
        state.open_preview_lease =
            state.open_preview_lease || edit_->hasOpenPreviewLease();
    }
    if (!state.pending_ticket_ids.empty() || state.open_preview_lease) {
        throw EditorCommandError{EditorCommandErrorCode::SaveBusy,
                                 "scene save is busy while an edit ticket or preview is pending"};
    }
    if (!dependencies_.save_scene) {
        throw EditorCommandError{EditorCommandErrorCode::SaveUnavailable,
                                 "scene save is unavailable"};
    }

    try {
        auto result = dependencies_.save_scene();
        if (result.scene_revision.value > maxExactEditorJsonInteger) {
            throw std::overflow_error("SceneRevision exceeds 2^53-1");
        }
        return result;
    } catch (const PrefabError &error) {
        if (error.code() == PrefabErrorCode::PrefabDependencyMismatch) {
            throw EditorCommandError{
                EditorCommandErrorCode::PrefabDependencyMismatch,
                error.what(), error.context().json_pointer};
        }
        throw EditorCommandError{EditorCommandErrorCode::SaveFailed,
                                 error.what(),
                                 error.context().json_pointer};
    } catch (const SceneSaveError &error) {
        switch (error.code()) {
        case SceneSaveErrorCode::ExternalModification:
            throw EditorCommandError{EditorCommandErrorCode::ExternalModification,
                                     error.what()};
        case SceneSaveErrorCode::Unavailable:
            throw EditorCommandError{EditorCommandErrorCode::SaveUnavailable,
                                     error.what()};
        case SceneSaveErrorCode::IoFailure:
            throw EditorCommandError{EditorCommandErrorCode::SaveFailed,
                                     error.what()};
        }
    } catch (const EditorCommandError &) {
        throw;
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::SaveFailed,
                                 error.what()};
    }
    throw EditorCommandError{EditorCommandErrorCode::SaveFailed,
                             "scene save failed"};
}

OrderedJson EditorCommandService::openEditorSession(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->openSession(params);
    actor_display_names_.insert_or_assign(
        result.at("actor_id").get<std::uint64_t>(),
        result.at("display_name").get<std::string>());
    return result;
}

OrderedJson EditorCommandService::resumeEditorSession(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->resumeSession(params);
    actor_display_names_.insert_or_assign(
        result.at("actor_id").get<std::uint64_t>(),
        result.at("display_name").get<std::string>());
    return result;
}

OrderedJson EditorCommandService::canEdit(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->canEdit(params);
}

OrderedJson EditorCommandService::canPreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->canPreview(params);
}

OrderedJson EditorCommandService::evalPreview(const Json &params) {
    if (!preview_ || !edit_) {
        auto payload = OrderedJson::object();
        payload["method"] = "eval_preview";
        payload["reason"] = "preview_service_unavailable";
        throw EditorPreviewError{
            EditorPreviewErrorCode::method_unavailable,
            "eval_preview is unavailable",
            std::move(payload)};
    }
    return preview_->evalPreview(params, [this] {
        const auto gate = edit_->canPreview(Json::object());
        return EditorPreviewGateSnapshot{
            .can_preview = gate.at("can_preview").get<bool>(),
            .epoch = gate.at("gate_epoch").get<std::uint64_t>(),
            .reasons = gate.at("reasons").get<std::vector<std::string>>(),
        };
    });
}

OrderedJson EditorCommandService::renderPreview(const Json &params) {
    if (!preview_ || !edit_) {
        auto payload = OrderedJson::object();
        payload["method"] = "render_preview";
        payload["reason"] = "preview_service_unavailable";
        throw EditorPreviewError{
            EditorPreviewErrorCode::method_unavailable,
            "render_preview is unavailable",
            std::move(payload)};
    }
    return preview_->renderPreview(params, [this] {
        const auto gate = edit_->canPreview(Json::object());
        return EditorPreviewGateSnapshot{
            .can_preview = gate.at("can_preview").get<bool>(),
            .epoch = gate.at("gate_epoch").get<std::uint64_t>(),
            .reasons = gate.at("reasons").get<std::vector<std::string>>(),
        };
    });
}

void EditorCommandService::validateGeneratedSelectors(
    const Json &params, std::string_view method) const {
    EditorMutationRequestAst request;
    try {
        request = parseEditorMutationRequest(params, method);
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::InvalidParams,
                                 "invalid_params: " +
                                     std::string{error.what()}};
    }
    for (const auto &selector : request.generated_selectors) {
        const ResolvedComponent *selected = nullptr;
        for (const auto &scene : resolved().scenes()) {
            for (const auto &object : scene.objects) {
                if (object.authoring_object_id != selector.object_id) continue;
                const auto found = std::find_if(
                    object.components.begin(), object.components.end(),
                    [&](const auto &component) {
                        const auto *generated = component.origin.generated();
                        return generated != nullptr &&
                               generated->stable_generated_id ==
                                   selector.generated_id;
                    });
                if (found != object.components.end()) selected = &*found;
            }
        }
        if (selected == nullptr) {
            throw EditorCommandError{EditorCommandErrorCode::ObjectNotFound,
                                     "generated component was not found",
                                     selector.generated_id};
        }
        throw EditorCommandError{
            EditorCommandErrorCode::PrefabGeneratedReadOnly,
            "prefab_generated_read_only: generated components cannot be edited",
            selector.generated_id};
    }
}

OrderedJson EditorCommandService::edit(const Json &params) {
    validateGeneratedSelectors(params, "edit");
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->enqueue(params);
}

OrderedJson EditorCommandService::undo(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->enqueueUndo(params);
}

OrderedJson EditorCommandService::redo(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->enqueueRedo(params);
}

OrderedJson EditorCommandService::openPreview(const Json &params) {
    validateGeneratedSelectors(params, "open_preview");
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->openPreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Open);
    return result;
}

OrderedJson EditorCommandService::updatePreview(const Json &params) {
    validateGeneratedSelectors(params, "update_preview");
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->updatePreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Update);
    return result;
}

OrderedJson EditorCommandService::commitPreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->commitPreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Commit);
    return result;
}

OrderedJson EditorCommandService::abortPreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->abortPreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Abort);
    return result;
}

OrderedJson EditorCommandService::getRenderFeatures(
    const Json &params) const {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->getRenderFeatures(params);
}

OrderedJson EditorCommandService::listRenderFeatures(
    const Json &params) const {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->listRenderFeatures(params);
}

OrderedJson EditorCommandService::editRenderFeatures(
    const Json &params) {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->editRenderFeatures(params);
}

OrderedJson EditorCommandService::getRenderAuthoringContext(
    const Json &params) const {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->getRenderAuthoringContext(params);
}

OrderedJson EditorCommandService::forkRenderPipeline(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->forkRenderPipeline(params);
}

OrderedJson EditorCommandService::editPresetPass(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->editPresetPass(params);
}

OrderedJson EditorCommandService::getComputeTaskAuthoring(const Json &params) const {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->getComputeTaskAuthoring(params);
}

OrderedJson EditorCommandService::getFrameNodeOrder(const Json &params) const {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->getFrameNodeOrder(params);
}

OrderedJson EditorCommandService::editFrameNodeOrder(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->editFrameNodeOrder(params);
}

OrderedJson EditorCommandService::editComputeTask(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->editComputeTask(params);
}

OrderedJson EditorCommandService::addComputeTask(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->addComputeTask(params);
}

OrderedJson EditorCommandService::removeComputeTask(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->removeComputeTask(params);
}

OrderedJson EditorCommandService::getRayCallableRecords(const Json &params) const {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->getRayCallableRecords(params);
}

OrderedJson EditorCommandService::editRayCallableRecords(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->editRayCallableRecords(params);
}

OrderedJson EditorCommandService::getSceneAccelerationStructures(const Json &params) const {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->getSceneAccelerationStructures(params);
}

OrderedJson EditorCommandService::editSceneAccelerationStructures(const Json &params) {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->editSceneAccelerationStructures(params);
}

OrderedJson EditorCommandService::addAuthoredPass(
    const Json &params) {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->addAuthoredPass(params);
}

OrderedJson EditorCommandService::removeAuthoredPass(
    const Json &params) {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->removeAuthoredPass(params);
}

OrderedJson EditorCommandService::getEditResult(const Json &params) const {
    if (procedural_asset_editor_ && params.is_object() && params.contains("ticket") &&
        params.at("ticket").is_string() && params.at("ticket").get<std::string>().starts_with("procedural-asset-edit-"))
        return procedural_asset_editor_->result(params);
    if (render_config_editor_ && params.is_object()) {
        const auto found = params.find("ticket");
        if (found != params.end() && found->is_string()) {
            const auto ticket = found->get<std::string>();
            if (ticket.starts_with("render-config-edit-") ||
                render_config_editor_->ownsTicket(ticket)) {
                return render_config_editor_->getResult(params);
            }
        }
    }
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->getResult(params);
}

OrderedJson EditorCommandService::getPreviewResult(const Json &params) const {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    synchronizePreviewWatch();
    return edit_->getPreviewResult(params);
}

OrderedJson EditorCommandService::queryJournal(const Json &params) const {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->queryJournal(params);
}

std::vector<OrderedJson> EditorCommandService::takeCompletedEditResults() {
    auto result =
        edit_ ? edit_->takeCompletedResults()
              : std::vector<OrderedJson>{};
    if (procedural_asset_editor_) {
        auto assets = procedural_asset_editor_->takeCompletedResults();
        result.insert(result.end(), std::make_move_iterator(assets.begin()), std::make_move_iterator(assets.end()));
    }
    if (render_config_editor_) {
        auto render_results =
            render_config_editor_->takeCompletedResults();
        result.insert(result.end(),
                      std::make_move_iterator(render_results.begin()),
                      std::make_move_iterator(render_results.end()));
    }
    return result;
}

void EditorCommandService::commitPendingEdits() noexcept {
    if (edit_) {
        edit_->commitPending();
        synchronizePreviewWatch();
    } else {
        if (render_config_editor_) render_config_editor_->commitPending();
        if (procedural_asset_editor_) procedural_asset_editor_->commitPending();
    }
}

OrderedJson EditorCommandService::getProceduralAssets(const Json &params) const {
    if (!procedural_asset_editor_) throw std::logic_error("Procedural asset editing is unavailable");
    return procedural_asset_editor_->get(params);
}
OrderedJson EditorCommandService::editProceduralAsset(const Json &params) {
    if (!procedural_asset_editor_) throw std::logic_error("Procedural asset editing is unavailable");
    if (edit_ && edit_->hasOpenPreviewLease())
        return {{"status", "rejected"}, {"error", {{"code", "preview_lease_conflict"}, {"message", "Finish the preview before editing assets"}}}};
    return procedural_asset_editor_->edit(params);
}
OrderedJson EditorCommandRpcAdapter::getProceduralAssets(const Json &params) const {
    return service_.getProceduralAssets(params);
}
OrderedJson EditorCommandRpcAdapter::editProceduralAsset(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("Editor service is read-only");
    return mutable_service_->editProceduralAsset(params);
}

bool EditorCommandService::forceAbortPreview(std::string reason) {
    if (!edit_ || !edit_->forceAbortPreview(std::move(reason))) return false;
    preview_lease_.reset();
    return true;
}

void EditorCommandService::trackPreviewTransition(
    const OrderedJson &response, const Json &params,
    PreviewWatchTransitionKind kind) {
    synchronizePreviewWatch();
    if (response.value("status", std::string{}) != "accepted") return;
    pending_preview_watch_.push_back(PendingPreviewWatchTransition{
        .request_id = response.at("request_id").get<std::string>(),
        .ticket = response.at("ticket").get<std::string>(),
        .actor_id = EditorActorId{params.at("actor_id").get<std::uint64_t>()},
        .kind = kind,
    });
}

void EditorCommandService::synchronizePreviewWatch() const noexcept {
    try {
        for (auto current = pending_preview_watch_.begin();
             current != pending_preview_watch_.end();) {
            const auto result = edit_->getPreviewResult(
                {{"request_id", current->request_id}});
            const auto status = result.value("status", std::string{});
            if (status == "accepted") {
                ++current;
                continue;
            }
            if (current->kind == PreviewWatchTransitionKind::Open &&
                status == "open") {
                EditorPreviewLeaseResult lease{
                    .actor = {.actor_id = current->actor_id},
                    .ticket = current->ticket,
                    .state = "open",
                };
                if (const auto actor = actor_display_names_.find(
                        current->actor_id.value);
                    actor != actor_display_names_.end()) {
                    lease.actor.display_name = actor->second;
                }
                for (const auto &id : result.at("affected_authoring_ids")) {
                    lease.affected_ids.push_back(
                        AuthoringObjectId{id.get<std::uint64_t>()});
                }
                preview_lease_ = std::move(lease);
            } else if (current->kind == PreviewWatchTransitionKind::Update &&
                       status == "updated" && preview_lease_ &&
                       preview_lease_->ticket == current->ticket) {
                preview_lease_->state = "updated";
            } else if ((current->kind == PreviewWatchTransitionKind::Commit ||
                        current->kind == PreviewWatchTransitionKind::Abort) &&
                       preview_lease_ &&
                       preview_lease_->ticket == current->ticket) {
                preview_lease_.reset();
            }
            current = pending_preview_watch_.erase(current);
        }
        if (!edit_->hasOpenPreviewLease()) preview_lease_.reset();
    } catch (...) {
        if (edit_ && !edit_->hasOpenPreviewLease()) preview_lease_.reset();
    }
}

OrderedJson editorComponentQueryJson(
    const EditorComponentQueryResult &component,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    if (component.generated && component.generated_resolved_json) {
        const auto &generated = *component.generated;
        OrderedJson result{{"generated",
                            OrderedJson{
                                {"stable_generated_id", generated.stable_generated_id},
                                {"source",
                                 OrderedJson{
                                     {"prefab", generated.prefab},
                                     {"instance_id", generated.instance_id},
                                     {"component_key", generated.component_key},
                                     {"digest", OrderedJson{{"algorithm", "sha256"},
                                                            {"hex", generated.digest_sha256}}},
                                 }},
                                {"resolved_json", *component.generated_resolved_json},
                                {"editable", false},
                            }}};
        if (component.resolution_failure) {
            result["resolution_error"] =
                resolutionFailureJson(*component.resolution_failure);
        }
        return result;
    }
    OrderedJson result{{"name", component.name},
                       {"component_index", component.component_index},
                       {"authored_json", component.authored_json}};
    if (component.runtime_json) result["runtime_json"] = *component.runtime_json;
    result["editable"] = component.editable;
    result["codec"] = OrderedJson{{"state", component.codec_state == ComponentCodecState::Registered
                                                   ? "registered"
                                                   : "missing"}};
    if (!component.codec_name.empty()) result["codec"]["name"] = component.codec_name;
    result["schema"] = OrderedJson{{"state", component.schema_state == EditorComponentSchemaState::Available
                                                    ? "available"
                                                    : "missing"}};
    if (component.schema_state == EditorComponentSchemaState::Available) {
        result["schema"]["fields"] = OrderedJson::array();
        for (const auto &field : component.schema_fields) {
            result["schema"]["fields"].push_back(
                schemaFieldJson(field, resolver));
        }
        if (component.schema_node)
            result["schema"]["node"] =
                schemaNodeJson(*component.schema_node, resolver);
    }
    result["pending"] = component.pending;
    if (component.name == "behavior") {
        result["behavior"] = OrderedJson{
            {"attachment_index", component.component_index},
            {"attachment_handle", component.behavior_attachment_handle
                                      ? OrderedJson(*component.behavior_attachment_handle)
                                      : OrderedJson(nullptr)},
            {"attachment_seq", component.behavior_attachment_seq
                                   ? OrderedJson(*component.behavior_attachment_seq)
                                   : OrderedJson(nullptr)},
            {"owner", component.behavior_owner
                          ? OrderedJson(*component.behavior_owner)
                          : OrderedJson(nullptr)},
            {"owner_generation", component.behavior_owner_generation
                                     ? OrderedJson(*component.behavior_owner_generation)
                                     : OrderedJson(nullptr)},
            {"status", component.pending ? "pending" : "available"},
        };
    }
    if (component.resolution_failure) {
        result["resolution_error"] =
            resolutionFailureJson(*component.resolution_failure);
    }
    return result;
}

OrderedJson editorQueryJson(const EditorComponentQueryResult &component) {
    const auto resolver = schemaTypeResolverOrDefault({});
    return editorComponentQueryJson(component, resolver);
}

OrderedJson editorQueryJson(const EditorSceneRevisionResult &revision) {
    OrderedJson result{
        {"scene_revision", revision.token.scene_revision.value},
        {"preview_epoch", revision.token.preview_epoch},
    };
    if (revision.last_transaction) {
        result["last_transaction"] = OrderedJson{
            {"actor_id", revision.last_transaction->actor_id.value},
            {"display_name", revision.last_transaction->display_name},
            {"affected_authoring_ids", OrderedJson::array()},
        };
        for (const auto id :
             revision.last_transaction->affected_authoring_ids) {
            result["last_transaction"]["affected_authoring_ids"].push_back(
                id.value);
        }
    } else {
        result["last_transaction"] = nullptr;
    }
    if (revision.preview_lease) {
        result["preview_lease"] = OrderedJson{
            {"actor",
             OrderedJson{{"actor_id", revision.preview_lease->actor.actor_id.value},
                         {"display_name",
                          revision.preview_lease->actor.display_name}}},
            {"ticket", revision.preview_lease->ticket},
            {"affected_ids", OrderedJson::array()},
            {"state", revision.preview_lease->state},
        };
        for (const auto id : revision.preview_lease->affected_ids) {
            result["preview_lease"]["affected_ids"].push_back(id.value);
        }
    } else {
        result["preview_lease"] = nullptr;
    }
    return result;
}

OrderedJson editorObjectQueryJson(
    const EditorObjectQueryResult &object,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    OrderedJson result{{"scene_revision", object.scene_revision.value},
                       {"authoring_object_id", object.authoring_object_id.value},
                       {"declaration_index", object.declaration_index}};
    if (object.name) result["name"] = *object.name;
    if (object.parent) result["parent"] = *object.parent;
    if (object.entity_id) {
        result["entity_id"] = OrderedJson{{"index", object.entity_id->index},
                                           {"gen", object.entity_id->generation}};
    }
    if (object.prefab_instance) {
        const auto &instance = *object.prefab_instance;
        OrderedJson prefab{{"ref", instance.ref},
                           {"instance_id", instance.instance_id},
                           {"closure_generation", instance.closure_generation},
                           {"provider_generation", instance.provider_generation},
                           {"parameters", OrderedJson::array()}};
        for (const auto &parameter : instance.parameters) {
            OrderedJson encoded{{"name", parameter.name},
                                {"value_resolved", parameter.value_resolved},
                                {"source", parameter.is_override ? "override" : "default"}};
            if (parameter.is_override) encoded["value_authored"] = *parameter.value_authored;
            prefab["parameters"].push_back(std::move(encoded));
        }
        result["prefab_instance"] = std::move(prefab);
    }
    result["components"] = OrderedJson::array();
    for (const auto &component : object.components) {
        result["components"].push_back(
            editorComponentQueryJson(component, resolver));
    }
    return result;
}

OrderedJson editorQueryJson(const EditorObjectQueryResult &object) {
    const auto resolver = schemaTypeResolverOrDefault({});
    return editorObjectQueryJson(object, resolver);
}

OrderedJson editorSceneQueryJson(
    const EditorSceneTreeResult &scene,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    OrderedJson result{{"scene_revision", scene.scene_revision.value}, {"scene_id", scene.scene_id}};
    result["objects"] = OrderedJson::array();
    for (const auto &object : scene.objects) {
        result["objects"].push_back(editorObjectQueryJson(object, resolver));
    }
    return result;
}

OrderedJson editorQueryJson(const EditorSceneTreeResult &scene) {
    const auto resolver = schemaTypeResolverOrDefault({});
    return editorSceneQueryJson(scene, resolver);
}

OrderedJson editorQueryJson(const EditorListAssetsResult &assets) {
    OrderedJson result;
    result["assets"] = OrderedJson::array();
    for (const auto &asset : assets.assets) {
        result["assets"].push_back(OrderedJson{{"id", asset.id},
                                               {"kind", asset.kind},
                                               {"path", asset.path},
                                               {"store", asset.store},
                                               {"status", asset.status}});
    }
    return result;
}

OrderedJson editorQueryJson(const ExportSceneSnapshotResponseV1 &snapshot) {
    return OrderedJson{
        {"schema_version", snapshot.schema_version},
        {"scene_revision", snapshot.scene_revision.value},
        {"current_scene_id", snapshot.current_scene_id},
        {"semantic_scene_bytes", snapshot.semantic_scene_bytes},
        {"digest", OrderedJson{{"algorithm", snapshot.digest.algorithm}, {"hex", snapshot.digest.hex}}},
        {"pending_ticket_ids", snapshot.pending_ticket_ids},
        {"preview_epoch", snapshot.preview_epoch},
    };
}

OrderedJson prefabClosureJson(const PrefabResolutionClosure &closure) {
    OrderedJson result;
    result["scenes"] = OrderedJson::array();
    for (const auto &scene : closure.scenes) {
        result["scenes"].push_back(
            OrderedJson{{"scene_id", scene.scene_id},
                        {"digest", OrderedJson{{"algorithm", "sha256"},
                                               {"hex", scene.digest_sha256}}}});
    }
    result["prefabs"] = OrderedJson::array();
    for (const auto &prefab : closure.prefabs) {
        result["prefabs"].push_back(
            OrderedJson{{"name", prefab.name},
                        {"digest", OrderedJson{{"algorithm", "sha256"},
                                               {"hex", prefab.digest_sha256}}}});
    }
    result["behaviors"] = OrderedJson::array();
    for (const auto &behavior : closure.behaviors) {
        result["behaviors"].push_back(OrderedJson{
            {"stable_name", behavior.stable_name},
            {"schema_version", behavior.schema_version},
            {"params_schema_fingerprint",
             behavior.params_schema_fingerprint}});
    }
    result["provider_fingerprint"] =
        OrderedJson{{"algorithm", "sha256"},
                    {"hex", closure.provider_fingerprint_sha256}};
    result["resolver_inputs_fingerprint"] =
        OrderedJson{{"algorithm", "sha256"},
                    {"hex", closure.resolver_inputs_fingerprint_sha256}};
    return result;
}

OrderedJson editorQueryJson(const ExportSceneSnapshotResponseV2 &snapshot) {
    return OrderedJson{
        {"schema_version", snapshot.schema_version},
        {"scene_revision", snapshot.scene_revision.value},
        {"current_scene_id", snapshot.current_scene_id},
        {"semantic_scene_bytes", snapshot.semantic_scene_bytes},
        {"digest", OrderedJson{{"algorithm", snapshot.digest.algorithm},
                                {"hex", snapshot.digest.hex}}},
        {"pending_ticket_ids", snapshot.pending_ticket_ids},
        {"preview_epoch", snapshot.preview_epoch},
        {"prefab_closure", prefabClosureJson(snapshot.prefab_closure)},
    };
}

OrderedJson editorQueryJson(const ImportSceneSnapshotResult &snapshot) {
    return OrderedJson{
        {"status", "imported"},
        {"committed", true},
        {"scene_revision", snapshot.scene_revision.value},
        {"current_scene_id", snapshot.current_scene_id},
        {"effect_errors", effectErrorsJson(snapshot.effect_errors)},
        {"diagnostics", sceneLoadDiagnosticsJson(snapshot.diagnostics)},
    };
}

OrderedJson editorQueryJson(const SaveSceneResult &save) {
    return OrderedJson{
        {"status", "saved"},
        {"scene_revision", save.scene_revision.value},
        {"digest", OrderedJson{{"algorithm", save.digest.algorithm},
                                {"hex", save.digest.hex}}},
        {"byte_count", save.byte_count},
        {"scene_hot_reload", save.scene_hot_reload},
    };
}

EditorCommandRpcAdapter::EditorCommandRpcAdapter(
    const EditorCommandService &service, SchemaTypeNameResolver resolver)
    : service_{service},
      schema_type_name_resolver_{
          schemaTypeResolverOrDefault(std::move(resolver))} {}

EditorCommandRpcAdapter::EditorCommandRpcAdapter(
    EditorCommandService &service, SchemaTypeNameResolver resolver)
    : service_{service}, mutable_service_{&service},
      schema_type_name_resolver_{
          schemaTypeResolverOrDefault(std::move(resolver))} {}

OrderedJson EditorCommandRpcAdapter::sceneTree(const Json &params) const {
    return editorSceneQueryJson(service_.sceneTree(parseSceneTreeRequest(params)),
                                schema_type_name_resolver_);
}

OrderedJson EditorCommandRpcAdapter::getSceneRevision(const Json &params) const {
    requireObjectParams(params, "get_scene_revision");
    requireOnlyFields(params, {}, "get_scene_revision");
    return editorQueryJson(service_.getSceneRevision());
}

OrderedJson EditorCommandRpcAdapter::getComponentAddCandidates(const Json &params) const {
    return service_.getComponentAddCandidates(
        parseGetComponentsRequest(params, "get_component_add_candidates"));
}

OrderedJson EditorCommandRpcAdapter::getComponents(const Json &params) const {
    return editorObjectQueryJson(
        service_.getComponents(parseGetComponentsRequest(params)),
        schema_type_name_resolver_);
}

namespace {

OrderedJson rayOccurrenceJson(const RayOccurrenceCatalogEntry &entry) {
    OrderedJson eligibility;
    switch (entry.eligibility) {
    case RayOccurrenceEligibilityKind::ok:
        eligibility = "ok";
        break;
    case RayOccurrenceEligibilityKind::not_ray_traced:
        eligibility = OrderedJson{{"state", "not_ray_traced"},
                                  {"reason", entry.reason}};
        break;
    case RayOccurrenceEligibilityKind::ambiguous:
        eligibility = OrderedJson{{"state", "ambiguous"},
                                  {"reason", entry.reason}};
        break;
    case RayOccurrenceEligibilityKind::unrepresentable:
        eligibility = OrderedJson{{"state", "unrepresentable"},
                                  {"reason", entry.reason}};
        break;
    }
    return OrderedJson{
        {"occurrence_id", entry.occurrence_id.empty()
                              ? OrderedJson(nullptr)
                              : OrderedJson(entry.occurrence_id)},
        {"display_name", entry.display_name},
        {"asset", entry.asset.empty() ? OrderedJson(nullptr)
                                        : OrderedJson(entry.asset)},
        {"node_path", entry.node_path},
        {"mesh", entry.mesh},
        {"primitive", entry.primitive},
        {"material", entry.material},
        {"variant", entry.variant},
        {"eligibility", std::move(eligibility)},
        {"authoring_object_id", entry.authoring_object_id},
        {"q", entry.q ? OrderedJson(*entry.q) : OrderedJson(nullptr)},
        {"effective", entry.effective
            ? OrderedJson{{"mask", entry.effective->mask},
                          {"instance_flags", entry.effective->instance_flags},
                          {"custom_index", entry.effective->custom_index}}
            : OrderedJson(nullptr)},
    };
}

} // namespace

namespace {
OrderedJson recordFieldsJson(const RaySceneRowSnapshot &snapshot, const RaySceneRow &row) {
    auto fields = OrderedJson::array();
    for (const auto &member : snapshot.record_members) {
        std::string_view type;
        std::size_t count = 1;
        switch (member.type) {
        case RayRecordType::i32: type = "i32"; break;
        case RayRecordType::u32: type = "u32"; break;
        case RayRecordType::f32: type = "f32"; break;
        case RayRecordType::vec2: type = "vec2"; count = 2; break;
        case RayRecordType::vec3: type = "vec3"; count = 3; break;
        case RayRecordType::vec4: type = "vec4"; count = 4; break;
        }
        if (member.offset > row.record_data.size() || count * 4 > row.record_data.size() - member.offset)
            throw std::runtime_error("Published ray record schema exceeds row data");
        const auto *bytes = row.record_data.data() + member.offset;
        OrderedJson value;
        if (member.type == RayRecordType::i32) {
            std::int32_t number;
            std::memcpy(&number, bytes, sizeof(number));
            value = number;
        } else if (member.type == RayRecordType::u32) {
            std::uint32_t number;
            std::memcpy(&number, bytes, sizeof(number));
            value = number;
        } else {
            auto components = OrderedJson::array();
            for (std::size_t index = 0; index < count; ++index) {
                float number;
                std::memcpy(&number, bytes + index * sizeof(number), sizeof(number));
                components.push_back(number);
            }
            value = count == 1 ? components.at(0) : components;
        }
        fields.push_back({{"name", member.name}, {"type", type},
                          {"offset", member.offset}, {"value", std::move(value)}});
    }
    return fields;
}
} // namespace

OrderedJson EditorCommandRpcAdapter::getRayOccurrences(
    const Json &params) const {
    auto entries = OrderedJson::array();
    const auto inventory = service_.getRayOccurrences(parseGetComponentsRequest(params));
    std::map<std::uint32_t, const RaySceneRow *> rows_by_q;
    if (inventory.plan) {
        for (const auto &row : inventory.plan->rows) rows_by_q.emplace(row.q, &row);
    }
    for (const auto &entry : inventory.entries) {
        auto encoded = rayOccurrenceJson(entry);
        encoded["binding"] = nullptr;
        if (entry.q) {
            const auto found = rows_by_q.find(*entry.q);
            if (found != rows_by_q.end()) {
                const auto &row = *found->second;
                encoded["binding"] = {
                    {"group_index", row.group_index},
                    {"group_name", inventory.plan->group_names.at(row.group_index)},
                    {"group_source", row.binding_index ? "binding" : "layout_default"},
                    {"layout_reference", inventory.plan->layout_reference},
                    {"binding_reference", row.binding_index ? OrderedJson(inventory.plan->bindings_reference) : OrderedJson(nullptr)},
                    {"binding_index", row.binding_index ? OrderedJson(*row.binding_index) : OrderedJson(nullptr)},
                    {"record_value_source", row.geometry_kind == RayGeometryKind::aabbs
                        ? "procedural_record" : row.binding_index ? "binding" : "zero_default"},
                    {"material_reference", row.material_reference},
                    {"material_name", row.material_name},
                    {"material_index", row.material_index == std::numeric_limits<std::uint32_t>::max()
                        ? OrderedJson(nullptr) : OrderedJson(row.material_index)},
                    {"geometry_index", row.geometry_index == std::numeric_limits<std::uint32_t>::max()
                        ? OrderedJson(nullptr) : OrderedJson(row.geometry_index)},
                    {"geometry_flags", row.geometry_flags},
                    {"record_bytes", row.record_data.size()},
                    {"record_source", "scene_plan"},
                    {"record_fields", recordFieldsJson(*inventory.plan, row)}};
            }
        }
        entries.push_back(std::move(encoded));
    }
    auto overrides = OrderedJson::array();
    auto rejected_rows = OrderedJson::array();
    if (inventory.plan) {
        for (const auto &row : inventory.plan->occurrence_overrides) {
            if (row.authoring_object_id != inventory.authoring_object_id) continue;
            overrides.push_back({
                {"address", {{"object", row.authoring_object_id},
                             {"component", row.ray_instance_component_witness},
                             {"list_path", "/overrides"}, {"row_id", row.row_id}}},
                {"occurrence_id", row.occurrence_id},
                {"q", row.q ? OrderedJson(*row.q) : OrderedJson(nullptr)},
                {"status", row.error.empty() ? "resolved" : "rejected"},
                {"effective", row.error.empty()
                    ? OrderedJson{{"mask", row.mask}, {"instance_flags", row.instance_flags},
                                  {"custom_index", row.custom_index}}
                    : OrderedJson(nullptr)},
            });
        }
        rejected_rows = sceneLoadDiagnosticsJson(rayScenePlanDiagnostics(
            *inventory.plan, inventory.authoring_object_id)).at("rejected_rows");
    }
    return OrderedJson{{"entries", std::move(entries)},
                       {"overrides", std::move(overrides)},
                       {"rejected_rows", std::move(rejected_rows)},
                       {"generation", inventory.plan ? OrderedJson(inventory.plan->generation)
                                                      : OrderedJson(nullptr)}};
}

OrderedJson EditorCommandRpcAdapter::resolveRayOccurrence(
    const Json &params) const {
    requireObjectParams(params, "resolve_ray_occurrence");
    requireOnlyFields(params, {"occurrence_id"},
                      "resolve_ray_occurrence");
    const auto found = params.find("occurrence_id");
    if (found == params.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty())
        invalidParams("resolve_ray_occurrence occurrence_id must be a non-empty string");
    const auto resolved = service_.resolveRayOccurrence(
        found->get_ref<const std::string &>());
    if (resolved.state == RayOccurrenceResolveState::absent)
        return OrderedJson{{"status", "absent"},
                           {"occurrence_id", *found}};
    if (resolved.state == RayOccurrenceResolveState::found) {
        auto entry = rayOccurrenceJson(resolved.candidates.front());
        entry["status"] = "found";
        return entry;
    }
    auto candidates = OrderedJson::array();
    for (const auto &entry : resolved.candidates) {
        auto value = rayOccurrenceJson(entry);
        candidates.push_back(std::move(value));
    }
    return OrderedJson{{"status", "ambiguous"},
                       {"occurrence_id", *found},
                       {"candidates", std::move(candidates)}};
}

OrderedJson EditorCommandRpcAdapter::listAssets(const Json &params) const {
    return editorQueryJson(service_.listAssets(parseListAssetsRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::exportSceneSnapshot(const Json &params) const {
    const auto request = parseExportSceneSnapshotRequest(params);
    if (request.schema_version == 1) {
        return editorQueryJson(service_.exportSceneSnapshot(request));
    }
    if (request.schema_version == 2) {
        return editorQueryJson(
            service_.exportSceneSnapshotV2(parseExportSceneSnapshotRequestV2(params)));
    }
    throw EditorCommandError{
        EditorCommandErrorCode::UnsupportedSnapshotVersion,
        "unsupported snapshot version: " +
            std::to_string(request.schema_version)};
}

OrderedJson EditorCommandRpcAdapter::importSceneSnapshot(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    requireObjectParams(params, "import_scene_snapshot");
    const auto version_field = params.find("schema_version");
    if (version_field == params.end()) {
        invalidParams("import_scene_snapshot schema_version is required");
    }
    const auto version = exactUnsignedInteger(
        *version_field, "import_scene_snapshot schema_version");
    if (version == 2) {
        return editorQueryJson(mutable_service_->importSceneSnapshotV2(
            parseImportSceneSnapshotRequestV2(params)));
    }
    if (version != 1) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " + std::to_string(version)};
    }
    if (params.is_object()) {
        const auto bytes = params.find("semantic_scene_bytes");
        if (bytes != params.end() && bytes->is_string()) {
            try {
                if (scenePayloadContainsPrefabKey(nlohmann::json::parse(
                        bytes->get_ref<const std::string &>()))) {
                    throw EditorCommandError{
                        EditorCommandErrorCode::PrefabPersistenceUnsupported,
                        "prefab_persistence_unsupported: snapshot import contains prefab instances"};
                }
            } catch (const EditorCommandError &) {
                throw;
            } catch (const std::exception &) {
                // The ordinary request/parser gates retain ownership of
                // malformed JSON and malformed request fields.
            }
        }
    }
    return editorQueryJson(mutable_service_->importSceneSnapshot(
        parseImportSceneSnapshotRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::saveScene(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    validateSaveSceneRequest(params);
    return editorQueryJson(mutable_service_->saveScene());
}

OrderedJson EditorCommandRpcAdapter::openEditorSession(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->openEditorSession(params);
}

OrderedJson EditorCommandRpcAdapter::resumeEditorSession(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->resumeEditorSession(params);
}

OrderedJson EditorCommandRpcAdapter::canEdit(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->canEdit(params);
}

OrderedJson EditorCommandRpcAdapter::canPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->canPreview(params);
}

OrderedJson EditorCommandRpcAdapter::evalPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->evalPreview(params);
}

OrderedJson EditorCommandRpcAdapter::renderPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->renderPreview(params);
}

OrderedJson EditorCommandRpcAdapter::edit(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->edit(params);
}

OrderedJson EditorCommandRpcAdapter::setPrefabParameter(
    const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    constexpr auto method = "set_prefab_parameter";
    requireObjectParams(params, method);
    requireOnlyFields(params,
                      {"actor_id", "scene_revision", "closure_generation",
                       "provider_generation", "object", "parameter", "value"},
                      method);
    for (const auto field : {"actor_id", "scene_revision",
                             "closure_generation", "provider_generation",
                             "object"}) {
        if (!params.contains(field)) {
            invalidParams(std::string{method} + " " + field + " is required");
        }
    }
    const auto parameter = optionalString(params, "parameter", method);
    if (!parameter || !params.contains("value")) {
        invalidParams(
            "set_prefab_parameter requires parameter and value");
    }
    return mutable_service_->edit(
        Json{{"actor_id", exactUnsignedInteger(params.at("actor_id"),
                                                "set_prefab_parameter actor_id")},
             {"scene_revision",
              exactUnsignedInteger(params.at("scene_revision"),
                                   "set_prefab_parameter scene_revision")},
             {"closure_generation",
              exactUnsignedInteger(params.at("closure_generation"),
                                   "set_prefab_parameter closure_generation")},
             {"provider_generation",
              exactUnsignedInteger(params.at("provider_generation"),
                                   "set_prefab_parameter provider_generation")},
             {"operations",
              Json::array({Json{{"op", method},
                                {"object", exactUnsignedInteger(
                                               params.at("object"),
                                               "set_prefab_parameter object")},
                                {"parameter", *parameter},
                                {"value", params.at("value")}}})}});
}

OrderedJson EditorCommandRpcAdapter::unsetPrefabParameter(
    const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    constexpr auto method = "unset_prefab_parameter";
    requireObjectParams(params, method);
    requireOnlyFields(params,
                      {"actor_id", "scene_revision", "closure_generation",
                       "provider_generation", "object", "parameter"},
                      method);
    for (const auto field : {"actor_id", "scene_revision",
                             "closure_generation", "provider_generation",
                             "object"}) {
        if (!params.contains(field)) {
            invalidParams(std::string{method} + " " + field + " is required");
        }
    }
    const auto parameter = optionalString(params, "parameter", method);
    if (!parameter) {
        invalidParams("unset_prefab_parameter parameter is required");
    }
    return mutable_service_->edit(
        Json{{"actor_id", exactUnsignedInteger(params.at("actor_id"),
                                                "unset_prefab_parameter actor_id")},
             {"scene_revision",
              exactUnsignedInteger(params.at("scene_revision"),
                                   "unset_prefab_parameter scene_revision")},
             {"closure_generation",
              exactUnsignedInteger(params.at("closure_generation"),
                                   "unset_prefab_parameter closure_generation")},
             {"provider_generation",
              exactUnsignedInteger(params.at("provider_generation"),
                                   "unset_prefab_parameter provider_generation")},
             {"operations",
              Json::array({Json{{"op", method},
                                {"object", exactUnsignedInteger(
                                               params.at("object"),
                                               "unset_prefab_parameter object")},
                                {"parameter", *parameter}}})}});
}

OrderedJson EditorCommandRpcAdapter::unpackPrefab(
    const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    constexpr auto method = "unpack_prefab";
    requireObjectParams(params, method);
    requireOnlyFields(params,
                      {"actor_id", "scene_revision", "closure_generation",
                       "provider_generation", "object"},
                      method);
    for (const auto field : {"actor_id", "scene_revision",
                             "closure_generation", "provider_generation",
                             "object"}) {
        if (!params.contains(field)) {
            invalidParams(std::string{method} + " " + field + " is required");
        }
    }
    return mutable_service_->edit(
        Json{{"actor_id", exactUnsignedInteger(params.at("actor_id"),
                                                "unpack_prefab actor_id")},
             {"scene_revision",
              exactUnsignedInteger(params.at("scene_revision"),
                                   "unpack_prefab scene_revision")},
             {"closure_generation",
              exactUnsignedInteger(params.at("closure_generation"),
                                   "unpack_prefab closure_generation")},
             {"provider_generation",
              exactUnsignedInteger(params.at("provider_generation"),
                                   "unpack_prefab provider_generation")},
             {"operations",
              Json::array({Json{{"op", method},
                                {"object", exactUnsignedInteger(
                                               params.at("object"),
                                               "unpack_prefab object")}}})}});
}

OrderedJson EditorCommandRpcAdapter::undo(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->undo(params);
}

OrderedJson EditorCommandRpcAdapter::redo(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->redo(params);
}

OrderedJson EditorCommandRpcAdapter::openPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->openPreview(params);
}

OrderedJson EditorCommandRpcAdapter::updatePreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->updatePreview(params);
}

OrderedJson EditorCommandRpcAdapter::commitPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->commitPreview(params);
}

OrderedJson EditorCommandRpcAdapter::abortPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->abortPreview(params);
}

OrderedJson EditorCommandRpcAdapter::getRenderFeatures(
    const Json &params) const {
    return service_.getRenderFeatures(params);
}

OrderedJson EditorCommandRpcAdapter::listRenderFeatures(
    const Json &params) const {
    return service_.listRenderFeatures(params);
}

OrderedJson EditorCommandRpcAdapter::editRenderFeatures(
    const Json &params) const {
    if (!mutable_service_) {
        throw std::logic_error("editor RPC adapter is read-only");
    }
    return mutable_service_->editRenderFeatures(params);
}

OrderedJson EditorCommandRpcAdapter::forkRenderPipeline(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->forkRenderPipeline(params);
}

OrderedJson EditorCommandRpcAdapter::editPresetPass(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("render config editing requires a mutable editor service");
    return mutable_service_->editPresetPass(params);
}

OrderedJson EditorCommandRpcAdapter::getComputeTaskAuthoring(const Json &params) const {
    return service_.getComputeTaskAuthoring(params);
}

OrderedJson EditorCommandRpcAdapter::getFrameNodeOrder(const Json &params) const {
    return service_.getFrameNodeOrder(params);
}

OrderedJson EditorCommandRpcAdapter::editFrameNodeOrder(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("render config editing requires a mutable editor service");
    return mutable_service_->editFrameNodeOrder(params);
}

OrderedJson EditorCommandRpcAdapter::editComputeTask(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("render config editing requires a mutable editor service");
    return mutable_service_->editComputeTask(params);
}

OrderedJson EditorCommandRpcAdapter::addComputeTask(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("render config editing requires a mutable editor service");
    return mutable_service_->addComputeTask(params);
}

OrderedJson EditorCommandRpcAdapter::removeComputeTask(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("render config editing requires a mutable editor service");
    return mutable_service_->removeComputeTask(params);
}

OrderedJson EditorCommandRpcAdapter::getRayCallableRecords(const Json &params) const {
    return service_.getRayCallableRecords(params);
}

OrderedJson EditorCommandRpcAdapter::editRayCallableRecords(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->editRayCallableRecords(params);
}

OrderedJson EditorCommandRpcAdapter::getSceneAccelerationStructures(const Json &params) const {
    return service_.getSceneAccelerationStructures(params);
}

OrderedJson EditorCommandRpcAdapter::editSceneAccelerationStructures(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->editSceneAccelerationStructures(params);
}

OrderedJson EditorCommandRpcAdapter::getRenderAuthoringContext(
    const Json &params) const {
    return service_.getRenderAuthoringContext(params);
}

OrderedJson EditorCommandRpcAdapter::addAuthoredPass(
    const Json &params) const {
    if (mutable_service_ == nullptr) {
        throw std::logic_error(
            "add_authored_pass requires a mutable editor service");
    }
    return mutable_service_->addAuthoredPass(params);
}

OrderedJson EditorCommandRpcAdapter::removeAuthoredPass(
    const Json &params) const {
    if (mutable_service_ == nullptr) {
        throw std::logic_error(
            "remove_authored_pass requires a mutable editor service");
    }
    return mutable_service_->removeAuthoredPass(params);
}

OrderedJson EditorCommandRpcAdapter::getEditResult(const Json &params) const {
    return service_.getEditResult(params);
}

OrderedJson EditorCommandRpcAdapter::getPreviewResult(const Json &params) const {
    return service_.getPreviewResult(params);
}

OrderedJson EditorCommandRpcAdapter::queryJournal(const Json &params) const {
    return service_.queryJournal(params);
}

std::vector<OrderedJson> EditorCommandRpcAdapter::takeCompletedEditResults() const {
    if (!mutable_service_) return {};
    return mutable_service_->takeCompletedEditResults();
}

bool EditorCommandRpcAdapter::forceAbortPreview(std::string reason) const {
    return mutable_service_ &&
           mutable_service_->forceAbortPreview(std::move(reason));
}

} // namespace Pelican
