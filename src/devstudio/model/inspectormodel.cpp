#include "inspectormodel.hpp"

#include <schemawire.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::string escapeJsonPointerSegment(std::string_view segment);

const Json *inspectorSchemaField(const Json &node, const std::string &path,
                                 const std::string &target) {
    if (path == target) return &node;
    if (node.value("tag", "") != "struct") return nullptr;
    for (const auto &child : node.at("children")) {
        const auto next = path + "/" + escapeJsonPointerSegment(child.at("name").get<std::string>());
        if (target != next && !target.starts_with(next + "/")) continue;
        if (const auto *found = inspectorSchemaField(child, next, target)) return found;
    }
    return nullptr;
}

void collectInspectorLists(const Json &node, const Json &authored,
                           const std::string &path, const std::string &prefix,
                           std::vector<InspectorListDescriptor> &lists) {
    if (node.value("tag", "") == "list") {
        InspectorListDescriptor list{.key = prefix + path, .path = path, .schema = node};
        const Json::json_pointer pointer{path};
        if (authored.contains(pointer)) {
            const auto &value = authored.at(pointer);
            if (value.is_object() && value.contains("rows") && value.at("rows").is_array())
                list.rows = value.at("rows");
        }
        lists.push_back(std::move(list));
        return;
    }
    if (node.value("tag", "") != "struct") return;
    for (const auto &child : node.value("children", Json::array()))
        collectInspectorLists(child, authored, path + "/" + escapeJsonPointerSegment(child.at("name").get<std::string>()), prefix, lists);
}

bool containsNestedList(const Json &node) {
    for (const auto &child : node.value("children", Json::array()))
        if (child.value("tag", "") == "list" || containsNestedList(child)) return true;
    return false;
}

void validateListRowKeys(const Json &schema, const Json &row) {
    if (!row.is_object()) throw std::invalid_argument("A list row must be an object.");
    const auto children = schema.value("children", Json::array());
    for (const auto &[key, value] : row.items()) {
        const auto field = std::find_if(children.begin(), children.end(),
            [&](const auto &child) { return child.value("name", "") == key; });
        if (key == "$row" || field == children.end())
            throw std::invalid_argument("Unknown or reserved list row field: " + key);
        if (field->value("tag", "") == "struct") validateListRowKeys(*field, value);
    }
}

void listRowDiff(const Json &schema, const Json &old_row, const Json &new_row,
                 const std::string &path, const Json &address, Json &operations) {
    for (const auto &field : schema.value("children", Json::array())) {
        const auto name = field.at("name").get<std::string>();
        const auto leaf_path = path + "/" + escapeJsonPointerSegment(name);
        const bool had = old_row.contains(name), has = new_row.contains(name);
        if (field.value("tag", "") == "struct") {
            listRowDiff(field, had ? old_row.at(name) : Json::object(),
                        has ? new_row.at(name) : Json::object(), leaf_path, address, operations);
            continue;
        }
        if ((!had && !has) || (had && has && old_row.at(name) == new_row.at(name))) continue;
        auto leaf_address = address;
        leaf_address["leaf_path"] = leaf_path;
        if (!has) {
            if (field.value("presence", "required") == "required")
                throw std::invalid_argument("Required list field cannot be removed: " + leaf_path);
            operations.push_back({{"op", "remove_component_field"}, {"address", leaf_address}});
        } else {
            if (!had) {
                if (!field.contains("default"))
                    throw std::invalid_argument("List field insertion requires a declared default: " + leaf_path);
                operations.push_back({{"op", "insert_component_field"}, {"address", leaf_address}});
                if (new_row.at(name) == field.at("default")) continue;
            }
            operations.push_back({{"op", "set_component_value"}, {"address", leaf_address}, {"value", new_row.at(name)}});
        }
    }
}

const Json *findInspectorRow(const InspectorListDescriptor &list, std::uint32_t id) {
    if (id == 0) return nullptr;
    for (const auto &row : list.rows)
        if (row.is_object() && row.contains("$row") && row.at("$row").is_object() &&
            row.at("$row").value("id", Json{}) == id) return &row;
    return nullptr;
}

constexpr std::uint64_t MaximumExactJsonInteger = 9007199254740991ULL;

struct SchemaWidgetRule {
    Pelican::Schema::FieldType type;
    InspectorWidgetKind kind;
    std::size_t columns;
};

constexpr std::array SchemaWidgetRules{
    SchemaWidgetRule{Pelican::Schema::FieldType::I8, InspectorWidgetKind::SignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::I16, InspectorWidgetKind::SignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::I32, InspectorWidgetKind::SignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::I64, InspectorWidgetKind::SignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::U8, InspectorWidgetKind::UnsignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::U16, InspectorWidgetKind::UnsignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::U32, InspectorWidgetKind::UnsignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::U64, InspectorWidgetKind::UnsignedIntegerDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::F32, InspectorWidgetKind::FloatingPointDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::F64, InspectorWidgetKind::FloatingPointDrag, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::Bool, InspectorWidgetKind::BooleanCheckbox, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::Enum, InspectorWidgetKind::EnumCombo, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::String, InspectorWidgetKind::StringInput, 1},
    SchemaWidgetRule{Pelican::Schema::FieldType::Vec2, InspectorWidgetKind::VectorDrag, 2},
    SchemaWidgetRule{Pelican::Schema::FieldType::Vec3, InspectorWidgetKind::VectorDrag, 3},
    SchemaWidgetRule{Pelican::Schema::FieldType::Vec4, InspectorWidgetKind::VectorDrag, 4},
    SchemaWidgetRule{Pelican::Schema::FieldType::Quat, InspectorWidgetKind::QuaternionDrag, 4},
};

std::string escapeJsonPointerSegment(std::string_view segment) {
    std::string result;
    result.reserve(segment.size());
    for (const char character : segment) {
        if (character == '~') {
            result += "~0";
        } else if (character == '/') {
            result += "~1";
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::uint64_t unsignedInteger(const Json &value, std::string_view path,
                              bool allow_zero = true) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(std::string{path} +
                                     " must be non-negative");
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        throw std::runtime_error(std::string{path} +
                                 " must be an integer");
    }
    if ((!allow_zero && result == 0) || result > MaximumExactJsonInteger) {
        throw std::runtime_error(std::string{path} +
                                 (result == 0 ? " must be non-zero"
                                              : " exceeds 2^53-1"));
    }
    return result;
}

std::size_t sizeValue(const Json &value, std::string_view path) {
    const auto result = unsignedInteger(value, path);
    if (result > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(std::string{path} + " exceeds size_t");
    }
    return static_cast<std::size_t>(result);
}

const Json &requiredField(const Json &object, std::string_view name,
                          std::string_view context) {
    if (!object.is_object()) {
        throw std::runtime_error(std::string{context} + " must be an object");
    }
    const auto field = object.find(name);
    if (field == object.end()) {
        throw std::runtime_error(std::string{context} + " requires field '" +
                                 std::string{name} + "'");
    }
    return *field;
}

std::string requiredString(const Json &object, std::string_view name,
                           std::string_view context) {
    const auto &field = requiredField(object, name, context);
    if (!field.is_string() || field.get_ref<const std::string &>().empty()) {
        throw std::runtime_error(std::string{context} + " field '" +
                                 std::string{name} +
                                 "' must be a non-empty string");
    }
    return field.get<std::string>();
}

std::optional<std::uint64_t> optionalUnsigned(const Json &object,
                                              std::string_view name,
                                              std::string_view context) {
    const auto field = object.find(name);
    if (field == object.end() || field->is_null()) {
        return std::nullopt;
    }
    return unsignedInteger(*field,
                           std::string{context} + " field '" +
                               std::string{name} + "'");
}

const SchemaWidgetRule &widgetRule(Pelican::Schema::FieldType type) {
    const auto rule = std::find_if(
        SchemaWidgetRules.begin(), SchemaWidgetRules.end(),
        [type](const SchemaWidgetRule &candidate) {
            return candidate.type == type;
        });
    if (rule == SchemaWidgetRules.end()) {
        throw std::logic_error("leaf schema type has no inspector widget rule");
    }
    return *rule;
}

bool valueMatchesDescriptor(const Json &value,
                            const InspectorWidgetDescriptor &descriptor) {
    try {
        (void)Pelican::Schema::resolveValue(descriptor.schema, value,
                                            descriptor.json_pointer);
        return true;
    } catch (const Pelican::Schema::Error &) {
        return false;
    }
}

std::string resultErrorCode(const Json &result) {
    const auto error = result.find("error");
    if (error == result.end() || !error->is_object()) {
        return {};
    }
    const auto code = error->find("code");
    return code != error->end() && code->is_string()
               ? code->get<std::string>()
               : std::string{};
}

std::string resultErrorMessage(const Json &result) {
    const auto error = result.find("error");
    if (error == result.end() || !error->is_object()) {
        return "editor command did not report a committed result";
    }
    const auto message = error->find("message");
    return message != error->end() && message->is_string()
               ? message->get<std::string>()
               : std::string{"editor command failed"};
}

std::string describeFailure(const Json &result) {
    const auto code = resultErrorCode(result);
    const auto message = resultErrorMessage(result);
    return code.empty() ? message : code + " - " + message;
}

struct WatchToken {
    std::uint64_t scene_revision = 0;
    std::uint64_t preview_epoch = 0;

    bool operator==(const WatchToken &) const = default;
};

WatchToken parseWatchToken(const Json &result) {
    return {
        .scene_revision = unsignedInteger(
            requiredField(result, "scene_revision", "get_scene_revision result"),
            "get_scene_revision scene_revision"),
        .preview_epoch = unsignedInteger(
            requiredField(result, "preview_epoch", "get_scene_revision result"),
            "get_scene_revision preview_epoch"),
    };
}

} // namespace

std::string inspectorJsonPointer(std::string_view schema_field_name) {
    std::string result;
    std::string segment;
    const auto flush = [&] {
        if (segment.empty()) {
            return;
        }
        result += "/" + escapeJsonPointerSegment(segment);
        segment.clear();
    };

    for (std::size_t index = 0; index < schema_field_name.size();) {
        const char character = schema_field_name[index];
        if (character == '.') {
            flush();
            ++index;
            continue;
        }
        if (character == '[') {
            const auto close = schema_field_name.find(']', index + 1);
            if (close != std::string_view::npos) {
                flush();
                result += "/" + escapeJsonPointerSegment(
                                      schema_field_name.substr(
                                          index + 1, close - index - 1));
                index = close + 1;
                continue;
            }
        }
        segment.push_back(character);
        ++index;
    }
    flush();
    return result;
}

std::vector<InspectorWidgetDescriptor>
makeInspectorWidgetPlan(const Json &component,
                        std::uint64_t authoring_object_id) {
    const std::string component_name =
        requiredString(component, "name", "component");
    const std::size_t component_index = sizeValue(
        requiredField(component, "component_index", "component"),
        "component component_index");
    const Json &authored = requiredField(component, "authored_json", "component");
    const Json &schema = requiredField(component, "schema", "component");
    if (requiredString(schema, "state", "component schema") != "available") {
        return {};
    }
    const Json &fields = requiredField(schema, "fields", "component schema");
    if (!fields.is_array()) {
        throw std::runtime_error("component schema fields must be an array");
    }

    const std::string pointer_prefix =
        component_name == "behavior" ? "/params" : std::string{};
    std::vector<InspectorWidgetDescriptor> result;
    result.reserve(fields.size());
    for (const Json &field : fields) {
        auto declaration =
            Pelican::parseSchemaFieldDeclaration(field, "schema field");
        const std::string field_name = declaration.name;
        const SchemaWidgetRule &rule = widgetRule(declaration.type);
        InspectorWidgetDescriptor descriptor{
            .field_name = field_name,
            .component_slot = component_name,
            .component_index = component_index,
            .json_pointer = pointer_prefix + inspectorJsonPointer(field_name),
            .schema = declaration,
            .kind = rule.kind,
            .columns = rule.columns,
            .behavior_attachment_handle =
                component_name == "behavior" && component.contains("behavior")
                    ? optionalUnsigned(component.at("behavior"),
                                       "attachment_handle", "behavior metadata")
                    : std::nullopt,
        };
        descriptor.field_key =
            std::to_string(authoring_object_id) + ":" + component_name + ":" +
            std::to_string(component_index) + ":" + descriptor.json_pointer;

        if (schema.contains("node")) {
            const auto *node = inspectorSchemaField(schema.at("node"), "",
                                                    inspectorJsonPointer(field_name));
            descriptor.default_available = node && node->contains("default") &&
                node->value("tag", "") != "struct" && node->value("tag", "") != "list";
        } else {
            // Flat schemas carry no default literal. The existing insert RPC
            // derives the selected default from the component codec.
            descriptor.default_available = true;
        }

        std::visit(
            [&](const auto &range) {
                using Range = std::decay_t<decltype(range)>;
                if constexpr (!std::same_as<Range, std::monostate>) {
                    descriptor.range_min = static_cast<double>(range.min);
                    descriptor.range_max = static_cast<double>(range.max);
                }
            },
            declaration.range);
        descriptor.unit = declaration.unit;
        descriptor.enum_values = declaration.enum_values;

        try {
            descriptor.value =
                authored.at(Json::json_pointer{descriptor.json_pointer});
            descriptor.authored = true;
            descriptor.value_matches_schema =
                valueMatchesDescriptor(descriptor.value, descriptor);
        } catch (const Json::exception &) {
            descriptor.value = nullptr;
        }
        result.push_back(std::move(descriptor));
    }
    return result;
}

const std::vector<std::string> &enginePrimitiveModelReferences() {
    static const std::vector<std::string> references{
        "engine://primitives/cube",
        "engine://primitives/plane",
        "engine://primitives/sphere",
    };
    return references;
}

std::string_view addObjectBaseName(AddObjectKind kind) {
    switch (kind) {
    case AddObjectKind::Empty: return "Empty";
    case AddObjectKind::Cube: return "Cube";
    case AddObjectKind::Plane: return "Plane";
    case AddObjectKind::Sphere: return "Sphere";
    }
    throw std::invalid_argument("unknown AddObjectKind");
}

std::string uniqueObjectName(std::string_view base,
                             const std::vector<std::string> &taken) {
    const auto used = [&taken](const std::string &name) {
        return std::find(taken.begin(), taken.end(), name) != taken.end();
    };
    std::string name{base};
    if (!used(name)) return name;
    for (unsigned suffix = 1;; ++suffix) {
        std::string number = std::to_string(suffix);
        if (number.size() < 3) number.insert(0, 3 - number.size(), '0');
        name = std::string{base} + "_" + number;
        if (!used(name)) return name;
    }
}

nlohmann::json addObjectDeclaration(AddObjectKind kind, const std::string &name) {
    Json components = Json::array({
        {{"name", "transform"},
         {"pos", Json::array({0.0, 0.0, 0.0})},
         {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
         {"scale", Json::array({1.0, 1.0, 1.0})}},
    });
    if (kind != AddObjectKind::Empty) {
        const auto index = static_cast<std::size_t>(kind) - 1;
        components.push_back({{"name", "simplemodelview"},
                              {"model", enginePrimitiveModelReferences().at(index)}});
    }
    return Json{{"name", name}, {"components", std::move(components)}};
}

bool isModelReferenceField(const InspectorWidgetDescriptor &descriptor) {
    return descriptor.component_slot == "simplemodelview" &&
           descriptor.json_pointer == "/model" &&
           descriptor.kind == InspectorWidgetKind::StringInput;
}

std::string builtinModelLabel(std::string_view reference) {
    constexpr std::string_view prefix = "engine://primitives/";
    if (!reference.starts_with(prefix) || reference.size() == prefix.size()) return {};
    std::string label{reference.substr(prefix.size())};
    if (label.front() >= 'a' && label.front() <= 'z')
        label.front() = static_cast<char>(label.front() - 'a' + 'A');
    return label + " (built-in)";
}

std::vector<ModelReferenceChoice>
modelReferenceChoices(std::string_view current,
                      const std::vector<std::string> &engine_models) {
    std::vector<ModelReferenceChoice> choices;
    const auto listed = [&choices](std::string_view value) {
        return std::any_of(choices.begin(), choices.end(),
                           [value](const auto &choice) { return choice.value == value; });
    };
    for (const auto &reference : enginePrimitiveModelReferences())
        choices.push_back({reference, builtinModelLabel(reference), true});
    for (const auto &id : engine_models) {
        if (id.empty() || listed(id)) continue;
        auto label = builtinModelLabel(id);
        const bool builtin = !label.empty();
        choices.push_back({id, builtin ? std::move(label) : id, builtin});
    }
    if (!current.empty() && !listed(current)) {
        auto label = builtinModelLabel(current);
        const bool builtin = !label.empty();
        choices.push_back({std::string{current},
                           builtin ? std::move(label) : std::string{current}, builtin});
    }
    // Built-ins first, then the project's, each in the order given.
    std::stable_partition(choices.begin(), choices.end(),
                          [](const auto &choice) { return choice.builtin; });
    return choices;
}

std::vector<std::string> modelAssetIds(const Json &list_assets_result) {
    const auto &assets = requiredField(list_assets_result, "assets", "list_assets result");
    if (!assets.is_array()) throw std::runtime_error("list_assets assets must be an array");
    std::vector<std::string> ids;
    for (const auto &asset : assets) {
        if (!asset.is_object() || asset.value("kind", std::string{}) != "model") continue;
        const auto id = asset.find("id");
        if (id != asset.end() && id->is_string() && !id->get_ref<const std::string &>().empty())
            ids.push_back(id->get<std::string>());
    }
    return ids;
}

struct InspectorModel::Impl {
    enum class RequestPurpose : std::uint8_t {
        OpenSession,
        SceneTree,
        Components,
        ComponentAddCandidates,
        ModelAssets,
        RayOccurrences,
        WatchPoll,
        WatchSync,
        EditSubmit,
        EditPoll,
        PreviewSubmit,
        PreviewPoll,
        Save,
    };

    enum class PreviewRequestKind : std::uint8_t {
        Open,
        Update,
        Commit,
        Abort,
    };

    struct PendingEdit {
        std::string ticket;
        std::string description;
        std::string component_slot;
        bool poll_in_flight = false;
    };

    struct RequestState {
        RequestPurpose purpose = RequestPurpose::OpenSession;
        std::uint64_t selection_generation = 0;
        bool synchronize_watch = false;
        PreviewRequestKind preview_kind = PreviewRequestKind::Open;
        std::string field_key;
        std::string ticket;
        std::string description;
        std::string component_slot;
        std::uint64_t snapshot_revision = 0;
        std::uint64_t ray_catalog_epoch = 0;
    };

    struct PreviewState {
        std::string field_key;
        std::string component_slot;
        std::string ticket;
        std::string outstanding_request;
        PreviewRequestKind outstanding_kind = PreviewRequestKind::Open;
        Json latest_operation;
        Json sent_operation;
        std::string failure_message;
        bool submit_in_flight = false;
        bool poll_in_flight = false;
        bool dirty = false;
        bool release_requested = false;
        bool abort_requested = false;
    };

    bool connected = false;
    std::optional<std::uint64_t> actor_id;
    std::optional<OutlinerObjectKey> selection;
    std::optional<OutlinerObjectKey> pending_selection;
    bool pending_selection_set = false;
    std::optional<InspectorObjectSnapshot> snapshot;
    std::optional<Json> component_add_candidates;
    // G-2: the model ids list_assets reported, for the model field.
    std::optional<std::vector<std::string>> model_assets;
    std::uint64_t model_assets_revision = 0;
    std::optional<Json> ray_occurrences;
    std::uint64_t ray_catalog_epoch = 0;
    InspectorNotice notice;
    std::uint64_t content_revision = 0;
    std::optional<std::uint64_t> current_scene_revision;
    std::optional<std::uint64_t> saved_scene_revision;
    bool scene_revision_uncertain = false;
    bool first_session_revision = true;
    std::uint64_t save_completion = 0;
    bool last_save_succeeded = false;
    std::uint64_t selection_generation = 0;
    std::uint64_t next_request_id = 1;
    std::vector<InspectorRpcRequest> outgoing;
    std::unordered_map<std::uint64_t, RequestState> requests;
    std::vector<PendingEdit> pending_edits;
    std::optional<PreviewState> preview;
    std::unordered_set<std::string> preview_unavailable;
    std::unordered_set<std::string> editing_widgets;
    std::unordered_map<std::string, Json> staged_operations;
    std::optional<WatchToken> observed_watch;
    bool refresh_in_flight = false;
    bool refresh_pending = false;
    bool refresh_pending_sync = false;
    bool watch_request_in_flight = false;
    bool watch_poll_pending = false;
    bool watch_sync_pending = false;

    void setNotice(InspectorNoticeKind kind, std::string message) {
        notice = {.kind = kind, .message = std::move(message)};
    }

    void observeSceneRevision(std::uint64_t revision) {
        if (first_session_revision && current_scene_revision && revision < *current_scene_revision) {
            // A restarted engine begins its revision sequence again. Retain
            // the warning about edits lost with that session until Save or
            // Discard, without comparing future saves to an obsolete counter.
            scene_revision_uncertain = scene_revision_uncertain ||
                (saved_scene_revision && *current_scene_revision != *saved_scene_revision);
            current_scene_revision = revision;
            saved_scene_revision = revision;
        }
        first_session_revision = false;
        if (!saved_scene_revision) saved_scene_revision = revision;
        current_scene_revision = std::max(current_scene_revision.value_or(revision), revision);
    }

    void clearSnapshot() {
        invalidateRayOccurrences();
        component_add_candidates.reset();
        if (snapshot) {
            snapshot.reset();
            ++content_revision;
        }
    }

    bool refreshBlocked() const noexcept {
        return preview.has_value() || !editing_widgets.empty();
    }

    bool editSubmissionInFlight() const noexcept {
        return std::any_of(
            requests.begin(), requests.end(), [&](const auto &request) {
                return request.second.purpose == RequestPurpose::EditSubmit;
            });
    }

    bool editPending() const noexcept {
        return !pending_edits.empty() || editSubmissionInFlight();
    }

    bool saveInFlight() const noexcept {
        return std::any_of(
            requests.begin(), requests.end(), [](const auto &request) {
                return request.second.purpose == RequestPurpose::Save;
            });
    }

    void abandonSessionOperations() {
        // An edit may commit before its reply is lost. Dropping its ticket is
        // not evidence of a clean document.
        scene_revision_uncertain = scene_revision_uncertain || editPending() ||
                                   preview.has_value() || !staged_operations.empty();
        if (saveInFlight()) {
            last_save_succeeded = false;
            ++save_completion;
        }
    }

    bool refreshSuppressed() const noexcept {
        // A direct edit stops being a widget interaction before its ticket is
        // terminal. Keep an already-issued refresh from briefly restoring the
        // pre-edit value during that gap as well.
        return refreshBlocked() || editPending();
    }

    std::uint64_t queue(std::string method, Json params,
                        RequestState state) {
        if (next_request_id == 0 ||
            next_request_id > MaximumExactJsonInteger) {
            throw std::overflow_error("inspector RPC request identifier space exhausted");
        }
        const std::uint64_t request_id = next_request_id++;
        requests.emplace(request_id, std::move(state));
        outgoing.push_back({
            .request_id = request_id,
            .method = std::move(method),
            .params = std::move(params),
        });
        return request_id;
    }

    void queueWatch(RequestPurpose purpose) {
        if (!connected || !actor_id || !selection) {
            return;
        }
        if (watch_request_in_flight) {
            if (purpose == RequestPurpose::WatchSync) {
                watch_sync_pending = true;
            }
            return;
        }
        if (purpose == RequestPurpose::WatchSync) {
            // A synchronized read after a completed refresh subsumes any
            // watch poll deferred while the widget was active.
            watch_poll_pending = false;
        }
        watch_request_in_flight = true;
        queue("get_scene_revision", Json::object(),
              {.purpose = purpose,
               .selection_generation = selection_generation});
    }

    void requestRefresh(bool synchronize_watch) {
        invalidateRayOccurrences();
        if (!selection) {
            refresh_pending = false;
            refresh_pending_sync = false;
            clearSnapshot();
            return;
        }
        if (!connected || !actor_id || refreshSuppressed() ||
            refresh_in_flight) {
            refresh_pending = true;
            refresh_pending_sync = refresh_pending_sync || synchronize_watch;
            return;
        }

        refresh_in_flight = true;
        const bool sync = std::exchange(refresh_pending_sync, false) ||
                          synchronize_watch;
        refresh_pending = false;
        queue("scene_tree", Json{{"scene_id", selection->scene_id}},
              {.purpose = RequestPurpose::SceneTree,
               .selection_generation = selection_generation,
               .synchronize_watch = sync});
    }

    void flushDeferredRefresh() {
        if (refreshSuppressed()) {
            return;
        }
        if (refresh_pending) {
            const bool sync = std::exchange(refresh_pending_sync, false);
            refresh_pending = false;
            requestRefresh(sync);
            return;
        }
        if (refresh_in_flight) {
            return;
        }
        if (watch_poll_pending) {
            watch_poll_pending = false;
            queueWatch(RequestPurpose::WatchPoll);
        }
    }

    InspectorWidgetDescriptor *findWidget(std::string_view field_key) {
        if (!snapshot) {
            return nullptr;
        }
        for (auto &component : snapshot->components) {
            const auto found = std::find_if(
                component.widgets.begin(), component.widgets.end(),
                [field_key](const InspectorWidgetDescriptor &widget) {
                    return widget.field_key == field_key;
                });
            if (found != component.widgets.end()) {
                return &*found;
            }
        }
        return nullptr;
    }

    InspectorComponentSnapshot *findComponent(
        const InspectorWidgetDescriptor &widget) {
        if (!snapshot) {
            return nullptr;
        }
        const auto found = std::find_if(
            snapshot->components.begin(), snapshot->components.end(),
            [&](const InspectorComponentSnapshot &component) {
                return component.name == widget.component_slot &&
                       component.component_index == widget.component_index;
            });
        return found == snapshot->components.end() ? nullptr : &*found;
    }

    Json operationFor(const InspectorWidgetDescriptor &widget,
                      const Json &value) const {
        Json operation{{"op", "set_component_value"},
                       {"object_id", snapshot->authoring_object_id},
                       {"component_slot", widget.component_slot},
                       {"field_path", widget.json_pointer},
                       {"value", value}};
        if (widget.component_slot == "behavior") {
            operation["attachment_index"] = widget.component_index;
            if (widget.behavior_attachment_handle) {
                operation["attachment_handle"] =
                    *widget.behavior_attachment_handle;
            }
        }
        return operation;
    }

    bool stageValue(std::string_view field_key, Json value) {
        InspectorWidgetDescriptor *widget = findWidget(field_key);
        if (widget == nullptr || !widget->authored ||
            !widget->value_matches_schema) {
            setNotice(InspectorNoticeKind::Error,
                      "The selected inspector field is not editable.");
            return false;
        }
        if (!valueMatchesDescriptor(value, *widget)) {
            setNotice(InspectorNoticeKind::Error,
                      "The value does not match the field schema.");
            return false;
        }
        InspectorComponentSnapshot *component = findComponent(*widget);
        if (component == nullptr || !component->editable || component->pending) {
            setNotice(InspectorNoticeKind::Error,
                      "The selected component is read-only.");
            return false;
        }

        widget->value = value;
        try {
            component->authored_json[Json::json_pointer{widget->json_pointer}] =
                value;
        } catch (const Json::exception &error) {
            setNotice(InspectorNoticeKind::Error,
                      "Could not stage the inspector value: " +
                          std::string{error.what()});
            return false;
        }
        staged_operations[std::string{field_key}] = operationFor(*widget, value);
        return true;
    }

    void handleEditCompletion(const Json &result,
                              const PendingEdit &pending) {
        const std::string status =
            result.value("status", std::string{});
        if (status == "committed") {
            const auto revision = result.find("committed_revision");
            const std::string suffix =
                revision != result.end()
                    ? " at revision " +
                          std::to_string(unsignedInteger(
                              *revision, "edit committed_revision"))
                    : std::string{};
            setNotice(InspectorNoticeKind::Success,
                      pending.description + " committed" + suffix + ".");
            requestRefresh(true);
            return;
        }

        const auto code = resultErrorCode(result);
        if (code == "stale_revision") {
            setNotice(InspectorNoticeKind::Error,
                      "Value changed elsewhere; refreshing the authored value. " +
                          describeFailure(result));
            requestRefresh(true);
            return;
        }
        setNotice(InspectorNoticeKind::Error,
                  pending.description + " failed: " +
                      describeFailure(result));
        requestRefresh(true);
    }

    std::pair<const InspectorComponentSnapshot *, const InspectorListDescriptor *>
    findList(std::string_view key) const {
        if (snapshot) for (const auto &component : snapshot->components)
            for (const auto &list : component.lists)
                if (list.key == key) return {&component, &list};
        return {nullptr, nullptr};
    }

    Json listAddress(const InspectorComponentSnapshot &component,
                     const InspectorListDescriptor &list) const {
        const auto type = component.name == "behavior"
            ? component.authored_json.at("type").get<std::string>() : component.name;
        return {{"authoring_object_id", snapshot->authoring_object_id},
                {"component", {{"type", type}, {"occurrence", component.component_index},
                    {"origin", {{"kind", "authored"}, {"authoring_component_index", component.component_index}}}}},
                {"list_path", list.path}};
    }

    void invalidateRayOccurrences() {
        ray_occurrences.reset();
        ++ray_catalog_epoch;
    }

    void enqueueListEdit(Json operations, const InspectorComponentSnapshot &component) {
        invalidateRayOccurrences();
        queue("edit", Json{{"actor_id", *actor_id}, {"base_revision", snapshot->scene_revision},
            {"operations", std::move(operations)}},
            {.purpose = RequestPurpose::EditSubmit, .selection_generation = selection_generation,
             .description = "Edit " + component.title + " list", .component_slot = component.name});
    }

    void enqueueEdit(Json operation, std::string description,
                     std::string component_slot) {
        invalidateRayOccurrences();
        if (!connected || !actor_id || !snapshot) {
            setNotice(InspectorNoticeKind::Error,
                      "The engine editor session is not ready.");
            return;
        }
        queue("edit",
              Json{{"actor_id", *actor_id},
                   {"base_revision", snapshot->scene_revision},
                   {"operations", Json::array({std::move(operation)})},
                   {"coalesce_key", "devstudio-inspector"}},
              {.purpose = RequestPurpose::EditSubmit,
               .selection_generation = selection_generation,
               .description = std::move(description),
               .component_slot = std::move(component_slot)});
    }

    void enqueueUndoRedo(bool redo) {
        if (!connected || !actor_id || !snapshot || editPending() || preview ||
            !editing_widgets.empty()) {
            setNotice(InspectorNoticeKind::Error,
                      "Undo and redo require an idle inspector session.");
            return;
        }
        const std::string description = redo ? "Redo" : "Undo";
        queue(redo ? "redo" : "undo",
              Json{{"actor_id", *actor_id},
                   {"base_revision", snapshot->scene_revision}},
              {.purpose = RequestPurpose::EditSubmit,
               .selection_generation = selection_generation,
               .description = description});
    }

    void sendPreviewRequest(PreviewRequestKind kind) {
        if (!preview || preview->submit_in_flight ||
            !preview->outstanding_request.empty() || !actor_id) {
            return;
        }

        Json params{{"actor_id", *actor_id}};
        std::string method;
        if (kind == PreviewRequestKind::Update) {
            method = "update_preview";
            params["ticket"] = preview->ticket;
            params["operations"] = Json::array({preview->latest_operation});
            preview->sent_operation = preview->latest_operation;
            preview->dirty = false;
        } else if (kind == PreviewRequestKind::Commit) {
            method = "commit_preview";
            params["ticket"] = preview->ticket;
        } else if (kind == PreviewRequestKind::Abort) {
            method = "abort_preview";
            params["ticket"] = preview->ticket;
        } else {
            return;
        }
        preview->submit_in_flight = true;
        queue(std::move(method), std::move(params),
              {.purpose = RequestPurpose::PreviewSubmit,
               .selection_generation = selection_generation,
               .preview_kind = kind,
               .field_key = preview->field_key,
               .component_slot = preview->component_slot});
    }

    void drivePreview() {
        if (!preview || preview->submit_in_flight ||
            !preview->outstanding_request.empty()) {
            return;
        }
        if (preview->abort_requested) {
            sendPreviewRequest(PreviewRequestKind::Abort);
        } else if (preview->dirty) {
            sendPreviewRequest(PreviewRequestKind::Update);
        } else if (preview->release_requested) {
            sendPreviewRequest(PreviewRequestKind::Commit);
        }
    }

    void applySelection(std::optional<OutlinerObjectKey> next) {
        if (selection == next) {
            return;
        }
        selection = std::move(next);
        ++selection_generation;
        refresh_in_flight = false;
        refresh_pending = false;
        refresh_pending_sync = false;
        watch_request_in_flight = false;
        watch_poll_pending = false;
        watch_sync_pending = false;
        observed_watch.reset();
        editing_widgets.clear();
        staged_operations.clear();
        // Accepted edits belong to the scene, not the selected property panel.
        // Keep polling them even after selecting another object.
        snapshot.reset();
        invalidateRayOccurrences();
        ++content_revision;
        if (selection) {
            setNotice(InspectorNoticeKind::Information,
                      actor_id ? "Loading inspector properties..."
                               : "Waiting for the engine editor session...");
            requestRefresh(true);
        } else {
            setNotice(InspectorNoticeKind::None, {});
        }
    }

    void applyPendingSelection() {
        if (!pending_selection_set || preview) {
            return;
        }
        pending_selection_set = false;
        auto next = std::move(pending_selection);
        pending_selection.reset();
        applySelection(std::move(next));
    }

    void failPreview(std::string message, bool try_abort = true) {
        if (!preview) {
            setNotice(InspectorNoticeKind::Error, std::move(message));
            return;
        }
        if (try_abort && connected && actor_id && !preview->ticket.empty()) {
            preview->failure_message = std::move(message);
            preview->submit_in_flight = false;
            preview->poll_in_flight = false;
            preview->outstanding_request.clear();
            preview->dirty = false;
            preview->release_requested = false;
            preview->abort_requested = true;
            setNotice(InspectorNoticeKind::Error,
                      preview->failure_message +
                          " Restoring the committed value...");
            drivePreview();
            return;
        }
        staged_operations.erase(preview->field_key);
        preview.reset();
        setNotice(InspectorNoticeKind::Error, std::move(message));
        applyPendingSelection();
        if (!pending_selection_set) {
            requestRefresh(true);
        }
        flushDeferredRefresh();
    }

    void handlePreviewSubmit(const RequestState &state, const Json &result) {
        if (!preview || preview->field_key != state.field_key) {
            return;
        }
        preview->submit_in_flight = false;
        if (result.value("status", std::string{}) != "accepted") {
            if (state.preview_kind == PreviewRequestKind::Open &&
                resultErrorCode(result) == "method_unavailable") {
                const std::string field_key = preview->field_key;
                const bool release = preview->release_requested ||
                                     !editing_widgets.contains(field_key);
                preview_unavailable.insert(field_key);
                preview.reset();
                setNotice(InspectorNoticeKind::Information,
                          "Live preview is unavailable for this field; the edit will commit on release.");
                if (release) {
                    const auto staged = staged_operations.find(field_key);
                    if (staged != staged_operations.end()) {
                        InspectorWidgetDescriptor *widget = findWidget(field_key);
                        const std::string slot =
                            widget != nullptr ? widget->component_slot : std::string{};
                        Json operation = std::move(staged->second);
                        staged_operations.erase(staged);
                        enqueueEdit(std::move(operation), "Edit " + field_key,
                                    slot);
                    }
                }
                applyPendingSelection();
                flushDeferredRefresh();
                return;
            }
            failPreview("Live preview failed: " + describeFailure(result),
                        state.preview_kind != PreviewRequestKind::Abort);
            return;
        }

        const std::string request_id =
            requiredString(result, "request_id", "preview response");
        if (state.preview_kind == PreviewRequestKind::Open) {
            preview->ticket = requiredString(result, "ticket", "preview response");
        }
        preview->outstanding_request = request_id;
        preview->outstanding_kind = state.preview_kind;
        setNotice(InspectorNoticeKind::Information,
                  state.preview_kind == PreviewRequestKind::Open
                      ? "Live preview is opening..."
                      : "Applying live preview...");
    }

    void handlePreviewPoll(const RequestState &state, const Json &result) {
        if (!preview || preview->field_key != state.field_key ||
            preview->outstanding_request != state.ticket) {
            return;
        }
        preview->poll_in_flight = false;
        const std::string status = result.value("status", std::string{});
        if (status == "accepted") {
            return;
        }

        const PreviewRequestKind kind = preview->outstanding_kind;
        preview->outstanding_request.clear();
        if ((kind == PreviewRequestKind::Open && status == "open") ||
            (kind == PreviewRequestKind::Update && status == "updated")) {
            setNotice(InspectorNoticeKind::Information,
                      status == "open" ? "Live preview is active."
                                       : "Live preview updated.");
            drivePreview();
            return;
        }
        if (kind == PreviewRequestKind::Commit && status == "committed") {
            const std::string field_key = preview->field_key;
            const auto revision = result.find("committed_revision");
            const std::string suffix =
                revision != result.end()
                    ? " at revision " +
                          std::to_string(unsignedInteger(
                              *revision, "preview committed_revision"))
                    : std::string{};
            staged_operations.erase(field_key);
            preview.reset();
            setNotice(InspectorNoticeKind::Success,
                      "Preview committed" + suffix + ".");
            applyPendingSelection();
            if (!pending_selection_set) {
                requestRefresh(true);
            }
            flushDeferredRefresh();
            return;
        }
        if (kind == PreviewRequestKind::Abort && status == "succeeded") {
            std::string failure_message =
                std::move(preview->failure_message);
            staged_operations.erase(preview->field_key);
            preview.reset();
            if (failure_message.empty()) {
                setNotice(
                    InspectorNoticeKind::Information,
                    "Live preview aborted; the committed value was restored.");
            } else {
                setNotice(InspectorNoticeKind::Error,
                          std::move(failure_message) +
                              " The committed value was restored.");
            }
            applyPendingSelection();
            if (!pending_selection_set) {
                requestRefresh(true);
            }
            flushDeferredRefresh();
            return;
        }
        failPreview("Live preview did not apply: " + describeFailure(result),
                    kind != PreviewRequestKind::Abort);
    }

    InspectorObjectSnapshot parseObject(const Json &result) const {
        InspectorObjectSnapshot object{
            .scene_id = selection->scene_id,
            .scene_revision = unsignedInteger(
                requiredField(result, "scene_revision", "get_components result"),
                "get_components scene_revision"),
            .authoring_object_id = unsignedInteger(
                requiredField(result, "authoring_object_id", "get_components result"),
                "get_components authoring_object_id", false),
            .declaration_index = sizeValue(
                requiredField(result, "declaration_index", "get_components result"),
                "get_components declaration_index"),
        };
        if (const auto name = result.find("name"); name != result.end()) {
            if (!name->is_string()) {
                throw std::runtime_error("get_components name must be a string");
            }
            object.name = name->get<std::string>();
        }
        if (object.declaration_index != selection->declaration_index) {
            throw std::runtime_error(
                "get_components returned a different declaration_index");
        }
        if (const auto instance = result.find("prefab_instance");
            instance != result.end()) {
            if (!instance->is_object()) {
                throw std::runtime_error("get_components prefab_instance must be an object");
            }
            object.prefab_instance = *instance;
            const auto parameters = instance->find("parameters");
            if (parameters == instance->end() || !parameters->is_array()) {
                throw std::runtime_error(
                    "get_components prefab_instance parameters must be an array");
            }
            object.prefab_parameters.reserve(parameters->size());
            for (const auto &parameter : *parameters) {
                if (!parameter.is_object()) {
                    throw std::runtime_error(
                        "prefab parameter entry must be an object");
                }
                InspectorPrefabParameterSnapshot parsed{
                    .name = requiredString(parameter, "name",
                                           "prefab parameter"),
                    .value_resolved = requiredField(
                        parameter, "value_resolved", "prefab parameter"),
                    .source = requiredString(parameter, "source",
                                             "prefab parameter"),
                };
                if (parsed.source != "default" &&
                    parsed.source != "override") {
                    throw std::runtime_error(
                        "prefab parameter source must be default or override");
                }
                const auto authored = parameter.find("value_authored");
                if (parsed.source == "override") {
                    if (authored == parameter.end()) {
                        throw std::runtime_error(
                            "override prefab parameter requires value_authored");
                    }
                    parsed.value_authored = *authored;
                } else if (authored != parameter.end()) {
                    throw std::runtime_error(
                        "default prefab parameter forbids value_authored");
                }
                object.prefab_parameters.push_back(std::move(parsed));
            }
        }

        const Json &components =
            requiredField(result, "components", "get_components result");
        if (!components.is_array()) {
            throw std::runtime_error("get_components components must be an array");
        }
        object.components.reserve(components.size());
        for (std::size_t wire_index = 0; wire_index < components.size(); ++wire_index) {
            const Json &component = components.at(wire_index);
            if (const auto generated = component.find("generated");
                generated != component.end()) {
                if (!generated->is_object()) {
                    throw std::runtime_error("generated component branch must be an object");
                }
                const auto &resolved = requiredField(
                    *generated, "resolved_json", "generated component");
                InspectorComponentSnapshot parsed{
                    .name = requiredString(resolved, "name", "generated resolved_json"),
                    .component_index = wire_index,
                    .authored_json = resolved,
                    .editable = false,
                    .pending = false,
                    .schema_available = false,
                    .read_only_reason =
                        "Read-only: generated by a prefab instance.",
                    .generated = true,
                    .stable_generated_id = requiredString(
                        *generated, "stable_generated_id", "generated component"),
                    .generated_source = requiredField(
                        *generated, "source", "generated component"),
                };
                parsed.title = parsed.name + " [generated]";
                object.components.push_back(std::move(parsed));
                continue;
            }
            InspectorComponentSnapshot parsed{
                .name = requiredString(component, "name", "component"),
                .component_index = sizeValue(
                    requiredField(component, "component_index", "component"),
                    "component component_index"),
                .authored_json = requiredField(component, "authored_json", "component"),
            };
            parsed.title = parsed.name;
            if (const auto runtime = component.find("runtime_json");
                runtime != component.end()) {
                parsed.runtime_json = *runtime;
            }
            const Json &editable = requiredField(component, "editable", "component");
            const Json &pending = requiredField(component, "pending", "component");
            if (!editable.is_boolean() || !pending.is_boolean()) {
                throw std::runtime_error(
                    "component editable and pending fields must be booleans");
            }
            parsed.editable = editable.get<bool>();
            parsed.pending = pending.get<bool>();
            const Json &schema = requiredField(component, "schema", "component");
            parsed.schema_available =
                requiredString(schema, "state", "component schema") == "available";

            if (parsed.name == "behavior") {
                const auto type = parsed.authored_json.find("type");
                parsed.title +=
                    " " + (type != parsed.authored_json.end() && type->is_string()
                                ? type->get<std::string>()
                                : std::string{"<unknown>"});
                parsed.title += " [index " +
                                std::to_string(parsed.component_index) + "]";
            }
            if (parsed.pending) {
                parsed.read_only_reason =
                    "Pending: the game-logic component is unavailable.";
            } else if (!parsed.editable) {
                const Json &codec = requiredField(component, "codec", "component");
                const std::string codec_state =
                    requiredString(codec, "state", "component codec");
                parsed.read_only_reason =
                    codec_state == "missing" ? "Read-only: codec missing."
                                             : "Read-only: codec is not editable.";
            } else if (!parsed.schema_available) {
                parsed.read_only_reason = "Read-only: schema missing.";
            } else {
                parsed.widgets =
                    makeInspectorWidgetPlan(component, object.authoring_object_id);
            }
            if (parsed.schema_available && schema.contains("node")) {
                const auto &authored = parsed.name == "behavior"
                    ? parsed.authored_json.value("params", Json::object()) : parsed.authored_json;
                collectInspectorLists(schema.at("node"), authored, "",
                    object.scene_id + ":" + std::to_string(object.authoring_object_id) + ":" +
                    std::to_string(object.scene_revision) + ":" + std::to_string(parsed.component_index) + ":", parsed.lists);
            }
            object.components.push_back(std::move(parsed));
        }
        return object;
    }

    void handleSceneTree(const RequestState &state, const Json &result) {
        if (state.selection_generation != selection_generation || !selection) {
            return;
        }
        if (refreshSuppressed()) {
            refresh_in_flight = false;
            refresh_pending = true;
            refresh_pending_sync =
                refresh_pending_sync || state.synchronize_watch;
            return;
        }
        const std::string scene_id =
            requiredString(result, "scene_id", "scene_tree result");
        if (scene_id != selection->scene_id) {
            throw std::runtime_error("scene_tree returned a different scene_id");
        }
        const Json &objects = requiredField(result, "objects", "scene_tree result");
        if (!objects.is_array()) {
            throw std::runtime_error("scene_tree objects must be an array");
        }

        std::optional<std::uint64_t> authoring_object_id;
        for (const Json &object : objects) {
            const auto index = sizeValue(
                requiredField(object, "declaration_index", "scene_tree object"),
                "scene_tree declaration_index");
            if (index == selection->declaration_index) {
                authoring_object_id = unsignedInteger(
                    requiredField(object, "authoring_object_id", "scene_tree object"),
                    "scene_tree authoring_object_id", false);
                break;
            }
        }
        if (!authoring_object_id) {
            throw std::runtime_error(
                "scene_tree does not contain the selected declaration_index");
        }
        queue("get_components",
              Json{{"scene_id", selection->scene_id},
                   {"authoring_object_id", *authoring_object_id}},
              {.purpose = RequestPurpose::Components,
               .selection_generation = selection_generation,
               .synchronize_watch = state.synchronize_watch});
    }

    void handleComponents(const RequestState &state, const Json &result) {
        if (state.selection_generation != selection_generation || !selection) {
            return;
        }
        if (refreshSuppressed()) {
            refresh_in_flight = false;
            refresh_pending = true;
            refresh_pending_sync =
                refresh_pending_sync || state.synchronize_watch;
            return;
        }

        snapshot = parseObject(result);
        requestModelAssets();
        component_add_candidates.reset();
        invalidateRayOccurrences();
        ++content_revision;
        staged_operations.clear();
        refresh_in_flight = false;
        if (state.synchronize_watch) {
            queueWatch(RequestPurpose::WatchSync);
        }
        if (refresh_pending) {
            const bool sync = std::exchange(refresh_pending_sync, false);
            refresh_pending = false;
            requestRefresh(sync);
        }
    }

    // The model field lists the project's models. Asked again each time an
    // object with a simplemodelview is loaded, so an asset registered since
    // shows up on the next selection.
    void requestModelAssets() {
        if (!connected || !actor_id || !snapshot) return;
        const bool has_model_view = std::any_of(
            snapshot->components.begin(), snapshot->components.end(),
            [](const InspectorComponentSnapshot &component) {
                return component.name == "simplemodelview" && !component.generated;
            });
        if (!has_model_view) return;
        if (std::any_of(requests.begin(), requests.end(), [](const auto &request) {
                return request.second.purpose == RequestPurpose::ModelAssets;
            })) return;
        queue("list_assets", Json::object(),
              {.purpose = RequestPurpose::ModelAssets,
               .selection_generation = selection_generation});
    }

    void resetModelAssets() {
        if (!model_assets) return;
        model_assets.reset();
        ++model_assets_revision;
    }

    void handleWatch(const RequestState &state, const Json &result) {
        if (state.selection_generation != selection_generation) {
            return;
        }
        watch_request_in_flight = false;
        const WatchToken current = parseWatchToken(result);
        if (state.purpose == RequestPurpose::WatchSync) {
            const bool revision_changed_during_refresh =
                snapshot &&
                snapshot->scene_revision != current.scene_revision;
            observed_watch = current;
            if (revision_changed_during_refresh) {
                requestRefresh(false);
            }
        } else if (!observed_watch) {
            observed_watch = current;
        } else if (*observed_watch != current) {
            observed_watch = current;
            if (refreshSuppressed()) {
                refresh_pending = true;
            } else {
                requestRefresh(false);
            }
        }
        if (watch_sync_pending) {
            watch_sync_pending = false;
            queueWatch(RequestPurpose::WatchSync);
        }
    }

    void handleEditSubmit(const RequestState &state, const Json &result) {
        PendingEdit pending{
            .description = state.description,
            .component_slot = state.component_slot,
        };
        if (result.value("status", std::string{}) == "accepted") {
            pending.ticket = requiredString(result, "ticket", "edit response");
            pending_edits.push_back(std::move(pending));
            setNotice(InspectorNoticeKind::Information,
                      state.description +
                          " accepted; waiting for the player frame boundary.");
            return;
        }
        handleEditCompletion(result, pending);
    }

    void handleEditPoll(const RequestState &state, const Json &result) {
        const auto pending = std::find_if(
            pending_edits.begin(), pending_edits.end(),
            [&](const PendingEdit &candidate) {
                return candidate.ticket == state.ticket;
            });
        if (pending == pending_edits.end()) {
            return;
        }
        pending->poll_in_flight = false;
        if (result.value("status", std::string{}) == "accepted") {
            return;
        }
        PendingEdit completed = std::move(*pending);
        pending_edits.erase(pending);
        handleEditCompletion(result, completed);
    }

    void handleSave(const Json &result) {
        if (requiredString(result, "status", "save_scene result") != "saved") {
            throw std::runtime_error("save_scene result status must be 'saved'");
        }
        const auto revision = unsignedInteger(
            requiredField(result, "scene_revision", "save_scene result"),
            "save_scene scene_revision");
        observeSceneRevision(revision);
        saved_scene_revision = revision;
        last_save_succeeded = true;
        scene_revision_uncertain = false;
        ++save_completion;
        setNotice(InspectorNoticeKind::Success,
                  "Scene saved at revision " + std::to_string(revision) + ".");
    }

    void handleResult(const RequestState &state, const Json &result) {
        // A selection change cannot undo a committed edit. Track its revision
        // before dropping a response whose property panel is now stale.
        if (result.value("status", std::string{}) == "committed" &&
            result.contains("committed_revision")) {
            observeSceneRevision(unsignedInteger(result.at("committed_revision"),
                                                  "edit committed_revision"));
        }
        if ((state.purpose == RequestPurpose::SceneTree ||
             state.purpose == RequestPurpose::Components ||
             state.purpose == RequestPurpose::WatchPoll ||
             state.purpose == RequestPurpose::WatchSync) && result.contains("scene_revision")) {
            observeSceneRevision(unsignedInteger(result.at("scene_revision"), "scene_revision"));
        }
        if (state.purpose != RequestPurpose::OpenSession &&
            state.purpose != RequestPurpose::Save &&
            state.purpose != RequestPurpose::EditSubmit &&
            state.purpose != RequestPurpose::EditPoll &&
            state.purpose != RequestPurpose::ModelAssets &&
            state.selection_generation != selection_generation) {
            return;
        }
        switch (state.purpose) {
        case RequestPurpose::OpenSession:
            actor_id = unsignedInteger(
                requiredField(result, "actor_id", "open_editor_session result"),
                "open_editor_session actor_id", false);
            setNotice(InspectorNoticeKind::Information,
                      selection ? "Loading inspector properties..."
                                : "Engine editor session is ready.");
            requestRefresh(true);
            break;
        case RequestPurpose::SceneTree:
            handleSceneTree(state, result);
            break;
        case RequestPurpose::Components:
            handleComponents(state, result);
            break;
        case RequestPurpose::ComponentAddCandidates:
            if (snapshot && result.at("object_id") == snapshot->authoring_object_id &&
                result.at("scene_revision") == snapshot->scene_revision &&
                result.at("candidates").is_array()) {
                component_add_candidates = result;
                setNotice(InspectorNoticeKind::None, {});
            } else {
                setNotice(InspectorNoticeKind::Information,
                          "The object changed. Refresh the Inspector and load component types again.");
            }
            break;
        case RequestPurpose::ModelAssets:
            model_assets = modelAssetIds(result);
            ++model_assets_revision;
            break;
        case RequestPurpose::RayOccurrences:
            if (snapshot && snapshot->scene_revision == state.snapshot_revision &&
                state.ray_catalog_epoch == ray_catalog_epoch && !refresh_in_flight &&
                !refreshBlocked() && !editPending()) {
                for (const auto *key : {"entries", "overrides", "rejected_rows"}) {
                    if (!result.at(key).is_array() ||
                        !std::all_of(result.at(key).begin(), result.at(key).end(),
                            [](const auto &row) { return row.is_object(); }))
                        throw std::runtime_error("Ray occurrence catalog requires arrays of objects.");
                }
                for (const auto &row : result.at("overrides")) {
                    if (!row.contains("address") || !row.at("address").is_object())
                        throw std::runtime_error("Ray occurrence override requires an address object.");
                }
                ray_occurrences = result;
                setNotice(InspectorNoticeKind::None, {});
            }
            break;
        case RequestPurpose::WatchPoll:
        case RequestPurpose::WatchSync:
            handleWatch(state, result);
            break;
        case RequestPurpose::EditSubmit:
            handleEditSubmit(state, result);
            break;
        case RequestPurpose::EditPoll:
            handleEditPoll(state, result);
            break;
        case RequestPurpose::PreviewSubmit:
            handlePreviewSubmit(state, result);
            break;
        case RequestPurpose::PreviewPoll:
            handlePreviewPoll(state, result);
            break;
        case RequestPurpose::Save:
            handleSave(result);
            break;
        }
    }

    void handleFailure(const RequestState &state, std::string message) {
        if (state.purpose != RequestPurpose::OpenSession &&
            state.purpose != RequestPurpose::Save &&
            state.purpose != RequestPurpose::EditSubmit &&
            state.purpose != RequestPurpose::EditPoll &&
            state.purpose != RequestPurpose::ModelAssets &&
            state.selection_generation != selection_generation) {
            return;
        }
        switch (state.purpose) {
        case RequestPurpose::SceneTree:
        case RequestPurpose::Components:
            refresh_in_flight = false;
            break;
        case RequestPurpose::WatchPoll:
        case RequestPurpose::WatchSync:
            watch_request_in_flight = false;
            break;
        case RequestPurpose::EditPoll: {
            scene_revision_uncertain = true;
            const auto pending = std::find_if(
                pending_edits.begin(), pending_edits.end(),
                [&](const PendingEdit &candidate) {
                    return candidate.ticket == state.ticket;
                });
            if (pending != pending_edits.end()) {
                pending_edits.erase(pending);
            }
            requestRefresh(true);
            break;
        }
        case RequestPurpose::PreviewSubmit:
        case RequestPurpose::PreviewPoll: {
            const bool failed_commit =
                state.purpose == RequestPurpose::PreviewSubmit
                    ? state.preview_kind == PreviewRequestKind::Commit
                    : preview && preview->outstanding_kind == PreviewRequestKind::Commit;
            scene_revision_uncertain = scene_revision_uncertain || failed_commit;
            const bool failed_abort =
                state.purpose == RequestPurpose::PreviewSubmit
                    ? state.preview_kind == PreviewRequestKind::Abort
                    : preview && preview->outstanding_kind ==
                                     PreviewRequestKind::Abort;
            failPreview(
                "Live preview RPC failed: " + message, !failed_abort);
            return;
        }
        case RequestPurpose::OpenSession:
            actor_id.reset();
            break;
        case RequestPurpose::EditSubmit:
            scene_revision_uncertain = true;
            requestRefresh(true);
            break;
        case RequestPurpose::Save:
            last_save_succeeded = false;
            ++save_completion;
            break;
        case RequestPurpose::ComponentAddCandidates:
            component_add_candidates.reset();
            break;
        case RequestPurpose::ModelAssets:
            // The primitives are listed without it; nothing to report.
            return;
        case RequestPurpose::RayOccurrences:
            if (state.ray_catalog_epoch != ray_catalog_epoch) return;
            ray_occurrences.reset();
            break;
        }
        setNotice(InspectorNoticeKind::Error, std::move(message));
    }
};

InspectorModel::InspectorModel() : impl_{std::make_unique<Impl>()} {}

InspectorModel::~InspectorModel() = default;

void InspectorModel::startSession() {
    impl_->abandonSessionOperations();
    impl_->first_session_revision = true;
    impl_->connected = true;
    impl_->actor_id.reset();
    impl_->requests.clear();
    impl_->outgoing.clear();
    impl_->pending_edits.clear();
    impl_->preview.reset();
    impl_->pending_selection.reset();
    impl_->pending_selection_set = false;
    impl_->preview_unavailable.clear();
    impl_->editing_widgets.clear();
    impl_->staged_operations.clear();
    impl_->observed_watch.reset();
    impl_->refresh_in_flight = false;
    impl_->refresh_pending = impl_->selection.has_value();
    impl_->refresh_pending_sync = impl_->selection.has_value();
    impl_->watch_request_in_flight = false;
    impl_->watch_poll_pending = false;
    impl_->watch_sync_pending = false;
    impl_->resetModelAssets();
    impl_->clearSnapshot();
    impl_->setNotice(InspectorNoticeKind::Information,
                     "Opening the engine editor session...");
    impl_->queue("open_editor_session",
                 Json{{"display_name", "Pelican Studio Inspector"}},
                 {.purpose = Impl::RequestPurpose::OpenSession});
}

void InspectorModel::stopSession(std::string message) {
    impl_->abandonSessionOperations();
    impl_->connected = false;
    impl_->actor_id.reset();
    impl_->requests.clear();
    impl_->outgoing.clear();
    impl_->pending_edits.clear();
    impl_->preview.reset();
    impl_->pending_selection.reset();
    impl_->pending_selection_set = false;
    impl_->editing_widgets.clear();
    impl_->staged_operations.clear();
    impl_->observed_watch.reset();
    impl_->refresh_in_flight = false;
    impl_->refresh_pending = impl_->selection.has_value();
    impl_->refresh_pending_sync = impl_->selection.has_value();
    impl_->watch_request_in_flight = false;
    impl_->watch_poll_pending = false;
    impl_->watch_sync_pending = false;
    impl_->resetModelAssets();
    impl_->clearSnapshot();
    impl_->setNotice(message.empty() ? InspectorNoticeKind::None
                                     : InspectorNoticeKind::Information,
                     std::move(message));
}

void InspectorModel::selectObject(
    std::optional<OutlinerObjectKey> selection) {
    if (impl_->preview) {
        impl_->pending_selection = std::move(selection);
        impl_->pending_selection_set = true;
        impl_->preview->abort_requested = true;
        impl_->drivePreview();
        return;
    }
    impl_->applySelection(std::move(selection));
}

void InspectorModel::requestRefresh() {
    impl_->requestRefresh(true);
}

std::vector<InspectorRpcRequest> InspectorModel::takeRpcRequests() {
    return std::exchange(impl_->outgoing, {});
}

void InspectorModel::receiveRpcResult(std::uint64_t request_id,
                                      std::string_view result_json) {
    const auto request = impl_->requests.find(request_id);
    if (request == impl_->requests.end()) {
        return;
    }
    Impl::RequestState state = std::move(request->second);
    impl_->requests.erase(request);
    try {
        Json result = Json::parse(result_json);
        if (!result.is_object()) {
            throw std::runtime_error("inspector RPC result must be an object");
        }
        impl_->handleResult(state, result);
    } catch (const std::exception &error) {
        impl_->handleFailure(
            state, "Invalid " +
                       std::string{state.purpose == Impl::RequestPurpose::OpenSession
                                       ? "open_editor_session"
                                       : "inspector"} +
                       " RPC result: " + error.what());
    }
}

void InspectorModel::receiveRpcFailure(std::uint64_t request_id,
                                       std::string message) {
    const auto request = impl_->requests.find(request_id);
    if (request == impl_->requests.end()) {
        return;
    }
    Impl::RequestState state = std::move(request->second);
    impl_->requests.erase(request);
    if (message.empty()) {
        message = "Inspector RPC failed without an error message.";
    }
    impl_->handleFailure(state, std::move(message));
}

bool InspectorModel::pendingPollWanted() const noexcept {
    return !impl_->pending_edits.empty() ||
           (impl_->preview &&
            !impl_->preview->outstanding_request.empty());
}

bool InspectorModel::externalWatchWanted() const noexcept {
    return impl_->connected && impl_->actor_id.has_value() &&
           impl_->selection.has_value();
}

void InspectorModel::pollPendingOperations() {
    if (!pendingPollWanted()) return;
    for (auto &pending : impl_->pending_edits) {
        if (pending.poll_in_flight) {
            continue;
        }
        pending.poll_in_flight = true;
        impl_->queue("get_edit_result", Json{{"ticket", pending.ticket}},
                     {.purpose = Impl::RequestPurpose::EditPoll,
                      .selection_generation = impl_->selection_generation,
                      .ticket = pending.ticket});
    }
    if (impl_->preview && !impl_->preview->outstanding_request.empty() &&
        !impl_->preview->poll_in_flight) {
        impl_->preview->poll_in_flight = true;
        impl_->queue(
            "get_preview_result",
            Json{{"request_id", impl_->preview->outstanding_request}},
            {.purpose = Impl::RequestPurpose::PreviewPoll,
             .selection_generation = impl_->selection_generation,
             .field_key = impl_->preview->field_key,
             .ticket = impl_->preview->outstanding_request});
    }
}

void InspectorModel::pollExternalChanges() {
    if (!externalWatchWanted()) return;
    if (impl_->refreshSuppressed()) {
        impl_->watch_poll_pending = true;
        return;
    }
    impl_->queueWatch(Impl::RequestPurpose::WatchPoll);
}

bool InspectorModel::beginWidgetEdit(std::string_view field_key) {
    if (!canEdit() || impl_->findWidget(field_key) == nullptr) {
        return false;
    }
    const std::string key{field_key};
    const bool another_widget_is_editing =
        !impl_->editing_widgets.empty() &&
        !impl_->editing_widgets.contains(key);
    const bool another_preview_is_active =
        impl_->preview && impl_->preview->field_key != key;
    if (impl_->editPending() || another_widget_is_editing ||
        another_preview_is_active) {
        impl_->setNotice(
            InspectorNoticeKind::Error,
            "Finish the active inspector edit before editing another field.");
        return false;
    }
    impl_->editing_widgets.insert(key);
    return true;
}

bool InspectorModel::stageWidgetValue(std::string_view field_key, Json value) {
    const std::string key{field_key};
    const bool already_editing = impl_->editing_widgets.contains(key);
    if (!beginWidgetEdit(field_key)) {
        return false;
    }
    if (impl_->stageValue(field_key, std::move(value))) {
        return true;
    }
    if (!already_editing) {
        impl_->editing_widgets.erase(key);
        impl_->flushDeferredRefresh();
    }
    return false;
}

bool InspectorModel::previewWidgetValue(std::string_view field_key,
                                        Json value) {
    if (!stageWidgetValue(field_key, std::move(value))) {
        return false;
    }
    const auto staged = impl_->staged_operations.find(std::string{field_key});
    if (staged == impl_->staged_operations.end()) {
        return false;
    }
    if (impl_->preview_unavailable.contains(std::string{field_key})) {
        return true;
    }
    if (!impl_->preview) {
        InspectorWidgetDescriptor *widget = impl_->findWidget(field_key);
        impl_->preview = Impl::PreviewState{
            .field_key = std::string{field_key},
            .component_slot =
                widget != nullptr ? widget->component_slot : std::string{},
            .latest_operation = staged->second,
            .sent_operation = staged->second,
            .submit_in_flight = true,
        };
        impl_->queue(
            "open_preview",
            Json{{"actor_id", *impl_->actor_id},
                 {"operations", Json::array({staged->second})}},
            {.purpose = Impl::RequestPurpose::PreviewSubmit,
             .selection_generation = impl_->selection_generation,
             .preview_kind = Impl::PreviewRequestKind::Open,
             .field_key = std::string{field_key},
             .component_slot = impl_->preview->component_slot});
        impl_->setNotice(InspectorNoticeKind::Information,
                         "Live preview is opening...");
        return true;
    }
    if (impl_->preview->field_key != field_key) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "Finish the active preview before editing another field.");
        return false;
    }
    impl_->preview->latest_operation = staged->second;
    impl_->preview->dirty =
        impl_->preview->latest_operation != impl_->preview->sent_operation;
    impl_->drivePreview();
    return true;
}

bool InspectorModel::finishWidgetEdit(std::string_view field_key,
                                      bool commit_staged) {
    impl_->editing_widgets.erase(std::string{field_key});
    if (impl_->preview && impl_->preview->field_key == field_key) {
        impl_->preview->release_requested = commit_staged;
        impl_->preview->abort_requested = !commit_staged;
        impl_->drivePreview();
        return true;
    }

    const auto staged = impl_->staged_operations.find(std::string{field_key});
    if (staged != impl_->staged_operations.end()) {
        Json operation = std::move(staged->second);
        impl_->staged_operations.erase(staged);
        if (commit_staged) {
            InspectorWidgetDescriptor *widget = impl_->findWidget(field_key);
            impl_->enqueueEdit(
                std::move(operation),
                "Edit " +
                    (widget != nullptr
                         ? widget->component_slot + "." + widget->field_name
                         : std::string{field_key}),
                widget != nullptr ? widget->component_slot : std::string{});
        } else {
            impl_->requestRefresh(true);
        }
    }
    impl_->flushDeferredRefresh();
    return true;
}

bool InspectorModel::commitWidgetValue(std::string_view field_key,
                                       Json value) {
    if (impl_->preview || impl_->editPending() ||
        !impl_->editing_widgets.empty()) {
        impl_->setNotice(
            InspectorNoticeKind::Error,
            "Finish the active inspector edit before committing another field.");
        return false;
    }
    if (!impl_->stageValue(field_key, std::move(value))) {
        return false;
    }
    const auto staged = impl_->staged_operations.find(std::string{field_key});
    if (staged == impl_->staged_operations.end()) {
        return false;
    }
    InspectorWidgetDescriptor *widget = impl_->findWidget(field_key);
    Json operation = std::move(staged->second);
    impl_->staged_operations.erase(staged);
    impl_->enqueueEdit(
        std::move(operation),
        "Edit " +
            (widget != nullptr
                 ? widget->component_slot + "." + widget->field_name
                 : std::string{field_key}),
        widget != nullptr ? widget->component_slot : std::string{});
    return true;
}

bool InspectorModel::canInsertField(std::string_view field_key) const {
    if (!canEdit() || busy() || refreshBlocked() || impl_->refresh_in_flight) return false;
    const auto *widget = impl_->findWidget(field_key);
    if (!widget || widget->authored || !widget->default_available) return false;
    const auto *component = impl_->findComponent(*widget);
    return component && component->editable && !component->pending && !component->generated;
}

bool InspectorModel::insertField(std::string_view field_key) {
    if (!canInsertField(field_key)) return false;
    const auto &widget = *impl_->findWidget(field_key);
    auto operation = impl_->operationFor(widget, nullptr);
    operation["op"] = "insert_component_field";
    operation.erase("value");
    impl_->enqueueEdit(std::move(operation), "Add default " + widget.field_name,
                       widget.component_slot);
    return true;
}

bool InspectorModel::requestRayOccurrences() {
    if (!canEdit() || busy() || refreshBlocked() || impl_->refresh_in_flight ||
        std::any_of(impl_->requests.begin(), impl_->requests.end(), [](const auto &request) {
            return request.second.purpose == Impl::RequestPurpose::RayOccurrences;
        })) return false;
    impl_->invalidateRayOccurrences();
    impl_->queue("get_ray_occurrences",
        Json{{"scene_id", impl_->snapshot->scene_id}, {"authoring_object_id", impl_->snapshot->authoring_object_id}},
        {.purpose = Impl::RequestPurpose::RayOccurrences,
         .selection_generation = impl_->selection_generation,
         .snapshot_revision = impl_->snapshot->scene_revision,
         .ray_catalog_epoch = impl_->ray_catalog_epoch});
    impl_->setNotice(InspectorNoticeKind::Information, "Loading geometry occurrences...");
    return true;
}

const std::optional<Json> &InspectorModel::rayOccurrences() const noexcept {
    return impl_->ray_occurrences;
}

bool InspectorModel::addRayOccurrenceOverride(std::string_view list_key, std::string_view occurrence_id) {
    if (!canEditList(list_key) || !impl_->ray_occurrences || occurrence_id.empty()) return false;
    const auto [component, list] = impl_->findList(list_key);
    if (component->name != "pelican_ray_instance" || list->path != "/overrides") return false;
    const Json requested_id = std::string{occurrence_id};
    const auto &entries = impl_->ray_occurrences->at("entries");
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
        return entry.value("occurrence_id", Json{}) == requested_id &&
            entry.value("eligibility", Json{}) == "ok" &&
            entry.value("authoring_object_id", Json{}) == impl_->snapshot->authoring_object_id;
    });
    if (found == entries.end()) return false;
    if (std::any_of(list->rows.begin(), list->rows.end(), [&](const auto &row) {
        return row.value("occurrence_id", Json{}) == requested_id;
    })) return false;
    return insertListRow(list_key, {{"occurrence_id", std::string{occurrence_id}}});
}

bool InspectorModel::canEditList(std::string_view key) const {
    if (!canEdit() || busy() || refreshBlocked() || impl_->refresh_in_flight) return false;
    const auto [component, list] = impl_->findList(key);
    return component && list && component->editable && !component->pending &&
        !component->generated && component->schema_available && !containsNestedList(list->schema) &&
        list->schema.value("children", Json::array()).size() == 1;
}

bool InspectorModel::insertListRow(std::string_view key, Json row) {
    if (!canEditList(key)) return false;
    const auto [component, list] = impl_->findList(key);
    try {
        validateListRowKeys(list->schema.at("children")[0], row);
        impl_->enqueueListEdit(Json::array({{{"op", "insert_component_field"},
            {"address", impl_->listAddress(*component, *list)}, {"value", std::move(row)},
            {"position", {{"end", true}}}}}), *component);
        return true;
    } catch (const std::exception &error) {
        impl_->setNotice(InspectorNoticeKind::Error, error.what());
        return false;
    }
}

bool InspectorModel::removeListRow(std::string_view key, std::uint32_t row_id) {
    if (!canEditList(key)) return false;
    const auto [component, list] = impl_->findList(key);
    if (!findInspectorRow(*list, row_id)) return false;
    auto address = impl_->listAddress(*component, *list);
    address["row_id"] = row_id;
    impl_->enqueueListEdit(Json::array({{{"op", "remove_component_field"}, {"address", address}}}), *component);
    return true;
}

bool InspectorModel::moveListRow(std::string_view key, std::uint32_t row_id,
                                std::optional<std::uint32_t> before) {
    if (!canEditList(key)) return false;
    const auto [component, list] = impl_->findList(key);
    if (!findInspectorRow(*list, row_id) || (before && (*before == row_id || !findInspectorRow(*list, *before)))) return false;
    auto address = impl_->listAddress(*component, *list);
    address["row_id"] = row_id;
    impl_->enqueueListEdit(Json::array({{{"op", "move_component_field"}, {"address", address},
        {"position", before ? Json{{"before", *before}} : Json{{"end", true}}}}}), *component);
    return true;
}

bool InspectorModel::editListRow(std::string_view key, std::uint32_t row_id, Json row) {
    if (!canEditList(key)) return false;
    const auto [component, list] = impl_->findList(key);
    const auto *old_row = findInspectorRow(*list, row_id);
    if (!old_row) return false;
    try {
        validateListRowKeys(list->schema.at("children")[0], row);
        auto address = impl_->listAddress(*component, *list);
        address["row_id"] = row_id;
        auto operations = Json::array();
        listRowDiff(list->schema.at("children")[0], *old_row, row, "", address, operations);
        if (operations.empty()) return true;
        impl_->enqueueListEdit(std::move(operations), *component);
        return true;
    } catch (const std::exception &error) {
        impl_->setNotice(InspectorNoticeKind::Error, error.what());
        return false;
    }
}

bool InspectorModel::canRemoveComponent(std::size_t component_index) const noexcept {
    if (!canEdit() || busy() || refreshBlocked() || impl_->refresh_in_flight) {
        return false;
    }
    const auto &components = impl_->snapshot->components;
    const auto found = std::find_if(components.begin(), components.end(),
        [component_index](const auto &component) {
            return component.component_index == component_index;
        });
    return found != components.end() && found->editable && !found->pending &&
           !found->generated;
}

bool InspectorModel::removeComponent(std::size_t component_index) {
    if (!canRemoveComponent(component_index)) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "Component removal requires an editable authored component and an idle inspector.");
        return false;
    }
    const auto &components = impl_->snapshot->components;
    const auto &component = *std::find_if(components.begin(), components.end(),
        [component_index](const auto &candidate) {
            return candidate.component_index == component_index;
        });
    Json operation{{"op", "remove_component"},
                   {"object_id", impl_->snapshot->authoring_object_id},
                   {"component_slot", component.name}};
    if (component.name == "behavior") operation["attachment_index"] = component_index;
    impl_->enqueueEdit(std::move(operation), "Remove " + component.title, component.name);
    return true;
}

bool InspectorModel::canRequestComponentAddCandidates() const noexcept {
    return canEdit() && !busy() && !refreshBlocked() && !impl_->refresh_in_flight &&
           std::none_of(impl_->requests.begin(), impl_->requests.end(), [](const auto &request) {
               return request.second.purpose == Impl::RequestPurpose::ComponentAddCandidates;
           });
}

bool InspectorModel::requestComponentAddCandidates() {
    if (!canRequestComponentAddCandidates()) return false;
    impl_->component_add_candidates.reset();
    impl_->queue("get_component_add_candidates",
                 Json{{"scene_id", impl_->snapshot->scene_id},
                      {"authoring_object_id", impl_->snapshot->authoring_object_id}},
                 {.purpose = Impl::RequestPurpose::ComponentAddCandidates,
                  .selection_generation = impl_->selection_generation});
    impl_->setNotice(InspectorNoticeKind::Information, "Loading component types...");
    return true;
}

const std::optional<Json> &InspectorModel::componentAddCandidates() const noexcept {
    return impl_->component_add_candidates;
}

const std::optional<std::vector<std::string>> &InspectorModel::modelAssets() const noexcept {
    return impl_->model_assets;
}

std::uint64_t InspectorModel::modelAssetsRevision() const noexcept {
    return impl_->model_assets_revision;
}

bool InspectorModel::addComponent(std::string_view key) {
    if (!impl_->component_add_candidates) return false;
    const auto &catalog = *impl_->component_add_candidates;
    const auto &candidates = catalog.at("candidates");
    const auto found = std::find_if(candidates.begin(), candidates.end(), [key](const auto &candidate) {
        return candidate.at("key").template get_ref<const std::string &>() == key;
    });
    if (found == candidates.end() || !found->value("available", false)) return false;
    return addComponent(key, found->at("component"));
}

bool InspectorModel::addComponent(std::string_view key, const Json &declaration) {
    if (!canRequestComponentAddCandidates() || !impl_->component_add_candidates) return false;
    const auto &catalog = *impl_->component_add_candidates;
    if (catalog.at("object_id") != impl_->snapshot->authoring_object_id ||
        catalog.at("scene_revision") != impl_->snapshot->scene_revision) return false;
    const auto &candidates = catalog.at("candidates");
    const auto found = std::find_if(candidates.begin(), candidates.end(), [key](const auto &candidate) {
        return candidate.at("key").template get_ref<const std::string &>() == key;
    });
    if (found == candidates.end() || !declaration.is_object() ||
        !declaration.contains("name") || !declaration.at("name").is_string() ||
        declaration.at("name") != found->at("name")) return false;
    if (found->contains("behavior_type") &&
        (!declaration.contains("type") || declaration.at("type") != found->at("behavior_type"))) return false;
    const auto &component_name = found->at("name").get_ref<const std::string &>();
    Json operation{{"op", "add_component"},
                   {"object_id", impl_->snapshot->authoring_object_id},
                   {"component", declaration}};
    impl_->enqueueEdit(std::move(operation), "Add " + std::string{key}, component_name);
    impl_->component_add_candidates.reset();
    return true;
}

bool InspectorModel::spawnObject(const std::string &scene_id, Json object) {
    if (!impl_->connected || !impl_->actor_id || !impl_->current_scene_revision) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "The engine editor session is not ready.");
        return false;
    }
    if (busy() || refreshBlocked()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "Finish the active inspector edit before adding an object.");
        return false;
    }
    const std::string name = object.value("name", std::string{});
    impl_->invalidateRayOccurrences();
    // No coalesce key: a new object is its own undo step, never folded into
    // the property edit before it.
    impl_->queue("edit",
                 Json{{"actor_id", *impl_->actor_id},
                      {"base_revision", *impl_->current_scene_revision},
                      {"operations", Json::array({Json{{"op", "spawn"},
                                                       {"scene_id", scene_id},
                                                       {"object", std::move(object)}}})}},
                 {.purpose = Impl::RequestPurpose::EditSubmit,
                  .selection_generation = impl_->selection_generation,
                  .description = "Add " + name});
    return true;
}

void InspectorModel::abortActivePreview() {
    if (!impl_->preview) {
        return;
    }
    impl_->preview->abort_requested = true;
    impl_->drivePreview();
}

void InspectorModel::undo() {
    impl_->enqueueUndoRedo(false);
}

void InspectorModel::redo() {
    impl_->enqueueUndoRedo(true);
}

bool InspectorModel::saveScene() {
    if (!canSave()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "Save requires an idle Inspector editor session.");
        return false;
    }
    impl_->queue("save_scene", Json::object(),
                 {.purpose = Impl::RequestPurpose::Save,
                  .selection_generation = impl_->selection_generation});
    impl_->setNotice(InspectorNoticeKind::Information, "Saving scene...");
    return true;
}

void InspectorModel::observeSceneRevision(std::uint64_t revision) {
    impl_->observeSceneRevision(revision);
}

void InspectorModel::resetSceneSaveState() {
    impl_->current_scene_revision.reset();
    impl_->saved_scene_revision.reset();
    impl_->scene_revision_uncertain = false;
    impl_->first_session_revision = true;
}

bool InspectorModel::hasUnsavedChanges() const noexcept {
    return impl_->scene_revision_uncertain ||
           (impl_->current_scene_revision && impl_->saved_scene_revision &&
            *impl_->current_scene_revision != *impl_->saved_scene_revision);
}

bool InspectorModel::sceneSaveInFlight() const noexcept { return impl_->saveInFlight(); }
std::uint64_t InspectorModel::sceneSaveCompletion() const noexcept { return impl_->save_completion; }
bool InspectorModel::lastSceneSaveSucceeded() const noexcept { return impl_->last_save_succeeded; }

const std::optional<OutlinerObjectKey> &InspectorModel::selection() const noexcept {
    return impl_->selection;
}

const std::optional<InspectorObjectSnapshot> &InspectorModel::snapshot() const noexcept {
    return impl_->snapshot;
}

const InspectorNotice &InspectorModel::notice() const noexcept {
    return impl_->notice;
}

std::uint64_t InspectorModel::contentRevision() const noexcept {
    return impl_->content_revision;
}

bool InspectorModel::refreshBlocked() const noexcept {
    return impl_->refreshBlocked();
}

bool InspectorModel::busy() const noexcept {
    return impl_->preview.has_value() || impl_->editPending() ||
           impl_->saveInFlight();
}

bool InspectorModel::canEdit() const noexcept {
    return impl_->connected && impl_->actor_id && impl_->snapshot.has_value();
}

bool InspectorModel::canUndoRedo() const noexcept {
    return canEdit() && !busy() && !impl_->refresh_in_flight;
}

bool InspectorModel::canSave() const noexcept {
    return impl_->connected && impl_->actor_id && !busy() &&
           !refreshBlocked() &&
           !impl_->refresh_in_flight;
}

} // namespace PelicanStudio
