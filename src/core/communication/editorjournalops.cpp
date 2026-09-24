#include "editorjournalops.hpp"

#include "../loader/authoringsceneauthority.hpp"
#include "../loader/componentcodec.hpp"
#include "../loader/editorprojectiontransaction.hpp"
#include "../schemanodeadapter.hpp"
#include "../userpublic/details/behavior/registerer.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {
namespace EditorJournalInternal {

std::string addressedTarget(const AddressedComponent &address,
                            std::optional<std::uint32_t> row_id,
                            std::string_view leaf_path = {}) {
    auto result = "/authoring_objects/" +
                  std::to_string(address.object.object_id.value) +
                  "/attachments/" + std::to_string(address.component_index) +
                  address.list_path;
    if (row_id) result += "/rows/by-id/" + std::to_string(*row_id);
    if (!leaf_path.empty()) result += std::string{leaf_path};
    return result;
}

void copyAddressRuntimeIdentity(const Json &raw, OrderedJson &operation) {
    for (const auto field : {"attachment_handle", "attachment_seq"}) {
        if (const auto found = raw.find(field); found != raw.end()) {
            operation[field] = *found;
        }
    }
}

OrderedJson canonicalAddress(const AddressedComponent &address,
                             std::optional<std::uint32_t> row_id,
                             std::string_view leaf_path) {
    OrderedJson result{
        {"authoring_object_id", address.object.object_id.value},
        {"component",
         {{"type", address.attachment_identity.at("type")},
          {"occurrence", address.component_index},
          {"origin", address.attachment_identity.at("origin")}}},
        {"list_path", address.list_path},
    };
    if (row_id) result["row_id"] = *row_id;
    if (!leaf_path.empty()) result["leaf_path"] = leaf_path;
    return result;
}

PreparedOperation addressedPreparedOperation(
    const Json &raw, const AddressedComponent &address,
    std::string_view op, const Json &next_component,
    const Json &inverse_component, std::optional<std::uint32_t> row_id,
    std::string_view leaf_path, std::optional<Json> value = std::nullopt,
    std::optional<OrderedJson> position = std::nullopt) {
    const auto target = addressedTarget(address, row_id, leaf_path);
    OrderedJson forward{
        {"op", op},
        {"object_id", address.object.object_id.value},
        {"scene_id", address.object.scene_id},
        {"component_slot", address.component_name},
        {"attachment_index", address.component_index},
        {"attachment_identity", address.attachment_identity},
        {"address", canonicalAddress(address, row_id, leaf_path)},
        {"authored_component", next_component},
    };
    OrderedJson inverse{
        {"op", op},
        {"object_id", address.object.object_id.value},
        {"scene_id", address.object.scene_id},
        {"component_slot", address.component_name},
        {"attachment_index", address.component_index},
        {"attachment_identity", address.attachment_identity},
        {"address", canonicalAddress(address, row_id, leaf_path)},
        {"authored_component", inverse_component},
    };
    if (value) forward["value"] = *value;
    if (position) forward["position"] = *position;
    copyAddressRuntimeIdentity(raw, forward);
    copyAddressRuntimeIdentity(raw, inverse);
    return PreparedOperation{
        .commands = {makeReplaceComponentCommand(
            address.object, address.component_index, address.component_name,
            next_component)},
        .forward = std::move(forward),
        .inverse = std::move(inverse),
        .affected = {address.object.object_id},
        .stable_target = target,
        .read_set = {target},
        .write_set = {target},
        .structural_domain =
            {{"kind", "component_list_row"},
             {"object", address.object.object_id.value},
             {"slot", address.component_name},
             {"attachment", address.attachment_identity},
             {"list_path", address.list_path},
             {"row_id", row_id ? OrderedJson(*row_id) : OrderedJson(nullptr)},
             {"leaf_path", leaf_path.empty() ? OrderedJson(nullptr)
                                               : OrderedJson(leaf_path)}},
    };
}

PreparedOperation prepareAddressedSet(
    const Json &raw, const AuthoringSceneDocument &document) {
    constexpr auto context = "set_component_value";
    requireOnly(raw, {"op", "address", "value", "attachment_handle",
                      "attachment_seq"}, context);
    if (!raw.contains("value")) {
        throw std::invalid_argument("set_component_value value is required");
    }
    auto address = requireComponentAddress(raw, document, context, false);
    const auto &list_schema = requireListSchema(address);
    if (address.leaf_tokens.empty()) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.leaf_path},
                     {"detail", "row leaf_path must identify one leaf field"}},
                    "row leaf_path must identify a leaf field");
    }
    const auto &leaf_schema =
        requireRowLeafSchema(address, list_schema);
    if (leaf_schema.tag == Schema::NodeTag::Struct ||
        leaf_schema.tag == Schema::NodeTag::List) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.leaf_path},
                     {"detail", "leaf_path must identify a scalar field"}},
                    "leaf_path must identify a scalar field");
    }
    Json next = address.component;
    auto *list = findListValue(next, address);
    if (list == nullptr) {
        editFailure(EditorEditErrorCode::row_envelope_missing,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.list_path}},
                    "RowEnvelopeMissing: list is absent");
    }
    auto [row_index, row] = requireRow(*list, *address.row_id, address);
    (void)row_index;
    try {
        const Json::json_pointer pointer{address.leaf_path};
        (void)row.at(pointer);
        row[pointer] = raw.at("value");
    } catch (const std::exception &error) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.leaf_path},
                     {"row_id", *address.row_id},
                     {"detail", error.what()}},
                    error.what());
    }
    validateRawComponentTransition(address.component, next,
                                   address.object.object_id,
                                   address.component_name);
    return addressedPreparedOperation(
        raw, address, context, next, address.component, address.row_id,
        address.leaf_path, raw.at("value"));
}

std::pair<std::size_t, std::uint32_t> insertionPosition(
    Json &list, const Json &raw, const AddressedComponent &address) {
    const auto found = raw.find("position");
    if (found == raw.end()) {
        throw std::invalid_argument(
            "list row requires position for a list row");
    }
    requireObject(*found, "list row position");
    const bool has_before = found->contains("before");
    const bool has_end = found->contains("end");
    if (has_before == has_end) {
        throw std::invalid_argument(
            "list row position requires exactly one of before and end");
    }
    if (has_end) {
        requireOnly(*found, {"end"}, "list row position");
        if (!found->at("end").is_boolean() ||
            !found->at("end").get<bool>()) {
            throw std::invalid_argument(
                "list row position.end must be true");
        }
        return {list.at("rows").size(), 0};
    }
    requireOnly(*found, {"before"}, "list row position");
    const auto before = exactU32(found->at("before"),
                                 "list row position.before", true);
    auto [index, row] = requireRow(list, before, address);
    (void)row;
    return {index, before};
}

std::optional<Schema::Node> fieldSchemaAt(const Json &component,
                                          std::string_view component_name,
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
        if (node == nullptr) {
            // A v2 behavior keeps only the flat schema; lift it into the same
            // owning model so both paths below read one representation.
            owned = internal::hostSchemaNode(
                SchemaNode::fromFlat(registration->params_schema));
            node = &*owned;
        }
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
    }
    if (node == nullptr) return std::nullopt;
    return *node;
}

PreparedOperation prepareLegacyFieldMutation(
    const Json &raw, const AuthoringSceneDocument &document, bool insert) {
    const auto context = insert ? std::string_view{"insert_component_field"}
                                : std::string_view{"remove_component_field"};
    requireOnly(raw, {"op", "object_id", "component_slot", "field_path",
                      "attachment_index", "attachment_handle",
                      "attachment_seq", "attachment_identity"}, context);
    const auto object_id = AuthoringObjectId{exactUnsigned(
        raw.at("object_id"), std::string{context} + " object_id", true)};
    const auto component_name = requireString(raw, "component_slot", context);
    const auto field_path = requireString(raw, "field_path", context);
    const auto tokens = pointerTokens(field_path,
                                      std::string{context} + " field_path");
    if (field_path == "/name" || field_path.starts_with("/name/")) {
        throw std::invalid_argument(std::string{context} +
                                    " cannot change component name");
    }
    const auto object = requireObjectLocation(document, object_id);
    const bool behavior = component_name == "behavior";
    std::pair<std::size_t, Json> located;
    if (behavior) {
        const auto found = raw.find("attachment_index");
        if (found == raw.end()) {
            throw std::invalid_argument(std::string{context} +
                                        " behavior target requires attachment_index");
        }
        located = requireComponentAt(
            object,
            static_cast<std::size_t>(exactUnsigned(
                *found, std::string{context} + " attachment_index")),
            component_name);
    } else {
        located = requireComponent(object, component_name);
    }
    const auto &[component_index, old_component] = located;
    rejectLegacyListPointer(old_component, component_name, tokens, object_id,
                            field_path);
    const auto field = fieldSchemaAt(old_component, component_name, tokens);
    if (!field || field->tag == Schema::NodeTag::Struct ||
        field->tag == Schema::NodeTag::List) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", object_id.value},
                     {"slot", component_name},
                     {"field_path", field_path},
                     {"detail", "field_path does not identify an editable field"}},
                    "field_path does not identify an editable field");
    }

    Json next = old_component;
    const Json::json_pointer pointer{field_path};
    if (insert) {
        try {
            (void)next.at(pointer);
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", field_path},
                         {"detail", "field already exists"}},
                        "field already exists");
        } catch (const EditFailure &) {
            throw;
        } catch (const nlohmann::json::out_of_range &) {
        }
        Json default_value;
        if (field->default_value) {
            default_value = *field->default_value;
        } else {
            // Legacy flat schemas do not retain default literals.  Decode to a
            // temporary typed value solely to discover the selected default;
            // never publish the resulting canonical component.
            const auto canonical =
                behavior ? canonicalBehaviorComponent(old_component, object_id)
                         : canonicalComponent(old_component, object_id,
                                              component_name);
            try {
                default_value = canonical.at(pointer);
            } catch (const nlohmann::json::out_of_range &) {
                editFailure(EditorEditErrorCode::schema_violation,
                            {{"object", object_id.value},
                             {"slot", component_name},
                             {"field_path", field_path},
                             {"detail", "field has no authored default"}},
                            "field has no authored default");
            }
        }
        if (behavior && !next.contains("params")) {
            next["params"] = Json::object();
        }
        next[pointer] = std::move(default_value);
    } else {
        try {
            (void)next.at(pointer);
        } catch (const nlohmann::json::out_of_range &) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value},
                         {"slot", component_name},
                         {"field_path", field_path},
                         {"detail", "field does not exist"}},
                        "field does not exist");
        }
        Json *parent = &next;
        for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
            parent = &parent->at(tokens[index]);
        }
        parent->erase(tokens.back());
    }
    validateRawComponentTransition(old_component, next, object_id,
                                   component_name);

    const auto attachment_handle = internalIdentityValue(
        raw, "attachment_handle", std::string{context} + " attachment_handle",
        true);
    const auto attachment_seq = internalIdentityValue(
        raw, "attachment_seq", std::string{context} + " attachment_seq");
    const auto target = behavior
                            ? stableBehaviorTarget(object_id, component_index,
                                                   attachment_handle, field_path)
                            : stableComponentTarget(object_id, component_name,
                                                    field_path);
    OrderedJson forward{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"field_path", field_path},
                        {"authored_component", next}};
    OrderedJson inverse{{"op", context},
                        {"object_id", object_id.value},
                        {"scene_id", object.scene_id},
                        {"component_slot", component_name},
                        {"field_path", field_path},
                        {"authored_component", old_component}};
    if (behavior) {
        forward["attachment_index"] = component_index;
        inverse["attachment_index"] = component_index;
        forward["attachment_handle"] = attachment_handle;
        inverse["attachment_handle"] = attachment_handle;
        forward["attachment_seq"] = attachment_seq;
        inverse["attachment_seq"] = attachment_seq;
        copyBehaviorAttachmentWitness(raw, forward, inverse);
    }
    return PreparedOperation{
        .commands = {makeReplaceComponentCommand(
            object, component_index, component_name, next)},
        .forward = std::move(forward),
        .inverse = std::move(inverse),
        .affected = {object_id},
        .stable_target = target,
        .read_set = {target},
        .write_set = {target},
        .structural_domain =
            {{"kind", "value_field"},
             {"object", object_id.value},
             {"slot", component_name},
             {"field_path", field_path},
             {"attachment_index", behavior ? OrderedJson(component_index)
                                             : OrderedJson(nullptr)}},
    };
}

PreparedOperation prepareInsertField(
    const Json &raw, const AuthoringSceneDocument &document) {
    constexpr auto context = "insert_component_field";
    if (!raw.contains("address")) {
        return prepareLegacyFieldMutation(raw, document, true);
    }
    requireOnly(raw, {"op", "address", "value", "position",
                      "attachment_handle", "attachment_seq"}, context);
    auto address = requireComponentAddress(raw, document, context, true);
    const auto &list_schema = requireListSchema(address);
    Json next = address.component;

    if (!address.row_id) {
        if (!raw.contains("value") || !raw.at("value").is_object()) {
            throw std::invalid_argument(
                "insert_component_field list row value must be an object");
        }
        Json *list = findListValue(next, address);
        const bool list_was_absent = list == nullptr;
        if (list_was_absent) {
            if (list_schema.presence != Schema::FieldPresence::Defaulted) {
                editFailure(EditorEditErrorCode::schema_violation,
                            {{"object", address.object.object_id.value},
                             {"slot", address.component_name},
                             {"field_path", address.list_path},
                             {"detail", "missing list is not defaulted"}},
                            "missing list is not defaulted");
            }
            list = &createMissingListValue(next, address);
            *list = Json{{"$list", {{"v", 1}, {"next_id", 1}}},
                         {"rows", Json::array()}};
        }
        validateListContainerShape(*list, address);
        const auto next_id = exactU32(list->at("$list").at("next_id"),
                                      "$list.next_id", true);
        if (next_id == std::numeric_limits<std::uint32_t>::max()) {
            editFailure(EditorEditErrorCode::row_id_exhausted,
                        {{"object", address.object.object_id.value},
                         {"slot", address.component_name},
                         {"field_path", address.list_path}},
                        "RowIdExhausted: list row id space is exhausted");
        }
        const auto [insert_index, before] =
            insertionPosition(*list, raw, address);
        Json authored_row = raw.at("value");
        Json envelope{{"id", next_id}, {"v", 1}};
        if (const auto supplied = authored_row.find("$row");
            supplied != authored_row.end()) {
            if (!supplied->is_object() || !supplied->contains("id") ||
                !supplied->contains("v")) {
                editFailure(EditorEditErrorCode::row_envelope_missing,
                            {{"object", address.object.object_id.value},
                             {"slot", address.component_name},
                             {"field_path", address.list_path}},
                            "RowEnvelopeMissing: copied row has an invalid $row envelope");
            }
            for (auto member = supplied->begin(); member != supplied->end();
                 ++member) {
                if (member.key() != "id" && member.key() != "v" &&
                    member.key() != "hint") {
                    editFailure(
                        EditorEditErrorCode::schema_violation,
                        {{"object", address.object.object_id.value},
                         {"slot", address.component_name},
                         {"field_path", address.list_path},
                         {"detail", "copied $row envelope has an unknown member"}},
                        "copied $row envelope has an unknown member");
                }
            }
            if (exactU32(supplied->at("v"), "$row.v") != 1U) {
                editFailure(EditorEditErrorCode::row_envelope_unsupported,
                            {{"object", address.object.object_id.value},
                             {"slot", address.component_name},
                             {"field_path", address.list_path}},
                            "RowEnvelopeUnsupported: copied row has an unknown $row version");
            }
            (void)exactU32(supplied->at("id"), "$row.id", true);
            if (supplied->contains("hint")) {
                envelope["hint"] = supplied->at("hint");
            }
            authored_row.erase(supplied);
        }
        Json row{{"$row", std::move(envelope)}};
        for (auto field = authored_row.begin(); field != authored_row.end();
             ++field) {
            row[field.key()] = field.value();
        }
        auto &rows = list->at("rows");
        rows.insert(rows.begin() + static_cast<Json::difference_type>(insert_index),
                    std::move(row));
        (*list)["$list"]["next_id"] = next_id + 1U;
        validateRawComponentTransition(address.component, next,
                                       address.object.object_id,
                                       address.component_name);

        Json inverse = address.component;
        Json *inverse_list = findListValue(inverse, address);
        if (inverse_list == nullptr) {
            inverse_list = &createMissingListValue(inverse, address);
            *inverse_list = Json{{"$list", {{"v", 1},
                                             {"next_id", next_id + 1U}}},
                                 {"rows", Json::array()}};
        } else {
            (*inverse_list)["$list"]["next_id"] = next_id + 1U;
        }
        address.row_id = next_id;
        const OrderedJson position = before == 0
                                         ? OrderedJson{{"end", true}}
                                         : OrderedJson{{"before", before}};
        return addressedPreparedOperation(
            raw, address, context, next, inverse, next_id, {},
            raw.at("value"), position);
    }

    if (raw.contains("position")) {
        throw std::invalid_argument(
            "insert_component_field leaf insertion does not accept position");
    }
    if (address.leaf_tokens.empty()) {
        throw std::invalid_argument(
            "insert_component_field with row_id requires leaf_path");
    }
    Json *list = findListValue(next, address);
    if (list == nullptr) {
        editFailure(EditorEditErrorCode::row_envelope_missing,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.list_path}},
                    "RowEnvelopeMissing: list is absent");
    }
    auto [row_index, row] = requireRow(*list, *address.row_id, address);
    (void)row_index;
    const auto &field = requireRowLeafSchema(address, list_schema);
    if (field.tag == Schema::NodeTag::Struct ||
        field.tag == Schema::NodeTag::List || !field.default_value) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.leaf_path},
                     {"detail", "field has no authored default"}},
                    "field has no authored default");
    }
    const Json::json_pointer pointer{address.leaf_path};
    try {
        (void)row.at(pointer);
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.leaf_path},
                     {"detail", "field already exists"}},
                    "field already exists");
    } catch (const EditFailure &) {
        throw;
    } catch (const nlohmann::json::out_of_range &) {
    }
    row[pointer] = *field.default_value;
    validateRawComponentTransition(address.component, next,
                                   address.object.object_id,
                                   address.component_name);
    return addressedPreparedOperation(
        raw, address, context, next, address.component, address.row_id,
        address.leaf_path);
}

PreparedOperation prepareMoveField(
    const Json &raw, const AuthoringSceneDocument &document) {
    constexpr auto context = "move_component_field";
    requireOnly(raw, {"op", "address", "position", "attachment_handle", "attachment_seq"}, context);
    const auto address = requireComponentAddress(raw, document, context, false);
    (void)requireListSchema(address);
    if (!address.row_id || !address.leaf_tokens.empty()) {
        throw std::invalid_argument("move_component_field requires row_id and no leaf_path");
    }
    Json next = address.component;
    auto *list = findListValue(next, address);
    if (list == nullptr) {
        editFailure(EditorEditErrorCode::row_envelope_missing,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name}, {"field_path", address.list_path}},
                    "RowEnvelopeMissing: list is absent");
    }
    const auto [source, row] = requireRow(*list, *address.row_id, address);
    const auto [destination, before] = insertionPosition(*list, raw, address);
    auto &rows = list->at("rows");
    if (before != *address.row_id) {
        Json moved = row;
        rows.erase(rows.begin() + static_cast<Json::difference_type>(source));
        const auto index = destination > source ? destination - 1 : destination;
        rows.insert(rows.begin() + static_cast<Json::difference_type>(index), std::move(moved));
    }
    return addressedPreparedOperation(raw, address, context, next, address.component,
                                      address.row_id, {}, std::nullopt, raw.at("position"));
}

PreparedOperation prepareRemoveField(
    const Json &raw, const AuthoringSceneDocument &document) {
    constexpr auto context = "remove_component_field";
    if (!raw.contains("address")) {
        return prepareLegacyFieldMutation(raw, document, false);
    }
    requireOnly(raw, {"op", "address", "attachment_handle",
                      "attachment_seq"}, context);
    auto address = requireComponentAddress(raw, document, context, false);
    (void)requireListSchema(address);
    Json next = address.component;
    auto *list = findListValue(next, address);
    if (list == nullptr) {
        editFailure(EditorEditErrorCode::row_envelope_missing,
                    {{"object", address.object.object_id.value},
                     {"slot", address.component_name},
                     {"field_path", address.list_path}},
                    "RowEnvelopeMissing: list is absent");
    }
    auto [row_index, row] = requireRow(*list, *address.row_id, address);
    if (address.leaf_tokens.empty()) {
        auto &rows = list->at("rows");
        rows.erase(rows.begin() + static_cast<Json::difference_type>(row_index));
    } else {
        const auto row_list_schema = requireListSchema(address);
        (void)requireRowLeafSchema(address, row_list_schema);
        const Json::json_pointer pointer{address.leaf_path};
        try {
            (void)row.at(pointer);
        } catch (const nlohmann::json::out_of_range &) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", address.object.object_id.value},
                         {"slot", address.component_name},
                         {"field_path", address.leaf_path},
                         {"row_id", *address.row_id},
                         {"detail", "field does not exist"}},
                        "field does not exist");
        }
        Json *parent = &row;
        for (std::size_t index = 0;
             index + 1U < address.leaf_tokens.size(); ++index) {
            parent = &parent->at(address.leaf_tokens[index]);
        }
        parent->erase(address.leaf_tokens.back());
    }
    validateRawComponentTransition(address.component, next,
                                   address.object.object_id,
                                   address.component_name);
    return addressedPreparedOperation(
        raw, address, context, next, address.component, address.row_id,
        address.leaf_path);
}

PreparedOperation prepareSet(const Json &raw,
                             const AuthoringSceneDocument &document) {
    constexpr auto context = "set_component_value";
    if (raw.contains("address")) return prepareAddressedSet(raw, document);
    requireOnly(raw, {"op", "object_id", "component_slot", "field_path",
                      "value", "attachment_index", "attachment_handle",
                      "attachment_seq", "attachment_identity"}, context);
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
    Json next_component = old_component;
    const auto tokens = pointerTokens(field_path,
                                      "set_component_value field_path");
    rejectLegacyListPointer(old_component, component_name, tokens, object_id,
                            field_path);
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
    validateRawComponentTransition(old_component, next_component, object_id,
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
        copyBehaviorAttachmentWitness(raw, forward, inverse);
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
                             const AuthoringSceneDocument &document,
                             const std::function<bool(std::string_view)> &
                                 component_exists) {
    constexpr auto context = "add_component";
    requireOnly(raw, {"op", "object_id", "component", "component_index",
                      "attachment_handle", "attachment_seq",
                      "attachment_identity"}, context);
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
    // Decode/canonicalize only to validate the typed projection.  The authored
    // component itself is the command and journal payload so omitted defaults,
    // list envelopes, and member order survive remove/undo and replay.
    if (behavior) {
        (void)canonicalBehaviorComponent(component, object_id);
    } else {
        (void)canonicalComponent(component, object_id, component_name,
                                 component_exists);
    }
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
    if (behavior && attachment_handle == 0) {
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
                        {"component", component}};
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
        copyBehaviorAttachmentWitness(raw, forward, inverse);
    }
    return PreparedOperation{
        .commands = {makeInsertComponentCommand(object, component_index, component)},
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
                      "attachment_handle", "attachment_seq",
                      "attachment_identity"}, context);
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
        copyBehaviorAttachmentWitness(raw, forward, inverse);
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
                          const AuthoringSceneDocument &document,
                          const std::function<bool(std::string_view)> &
                              component_exists) {
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
        // Validation produces the typed projection later; spawn must retain
        // the exact authored component instead of publishing its defaults.
        (void)canonicalComponent(component, AuthoringObjectId{}, component_name,
                                 component_exists);
    }
    return result;
}

PreparedOperation prepareSpawn(const Json &raw,
                               const AuthoringSceneDocument &document,
                               std::string_view default_scene,
                               const std::function<bool(std::string_view)> &
                                   component_exists) {
    constexpr auto context = "spawn";
    requireOnly(raw, {"op", "scene_id", "declaration_index", "object",
                      "new_instance_id"},
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
    auto object = canonicalSpawnObject(raw.at("object"), scene_id, document,
                                       component_exists);
    std::optional<std::string> new_instance_id;
    if (object.contains("prefab")) {
        const auto found = raw.find("new_instance_id");
        if (found == raw.end()) {
            editFailure(
                EditorEditErrorCode::prefab_instance_id_collision,
                {{"object", nullptr}, {"slot", nullptr},
                 {"field_path", "/new_instance_id"},
                 {"detail", "prefab spawn requires an authoritative new_instance_id"}},
                "prefab spawn requires an authoritative new_instance_id");
        }
        if (!found->is_string() ||
            !isPrefabIdentifier(found->get_ref<const std::string &>())) {
            editFailure(
                EditorEditErrorCode::schema_violation,
                {{"object", nullptr}, {"slot", nullptr},
                 {"field_path", "/new_instance_id"},
                 {"detail", "prefab spawn requires a valid new_instance_id"}},
                "prefab spawn requires a valid new_instance_id");
        }
        if (!object.at("prefab").is_object()) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", nullptr}, {"slot", nullptr},
                         {"field_path", "/object/prefab"},
                         {"detail", "prefab spawn block must be an object"}},
                        "prefab spawn block must be an object");
        }
        new_instance_id = found->get<std::string>();
        object["prefab"]["instance_id"] = *new_instance_id;
    } else if (raw.contains("new_instance_id")) {
        editFailure(EditorEditErrorCode::schema_violation,
                    {{"object", nullptr}, {"slot", nullptr},
                     {"field_path", "/new_instance_id"},
                     {"detail", "new_instance_id is valid only for prefab spawn"}},
                    "new_instance_id is valid only for prefab spawn");
    }
    const auto name = object.at("name").get<std::string>();
    OrderedJson forward{{"op", context},
                        {"scene_id", scene_id},
                        {"declaration_index", declaration_index},
                        {"object", object}};
    if (new_instance_id) forward["new_instance_id"] = *new_instance_id;
    return PreparedOperation{
        .commands = {makeInsertObjectCommand(scene_id, declaration_index, object)},
        .forward = std::move(forward),
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

namespace {

// `<base>_copy<N>` with the smallest N the scene does not use yet.
std::string copyName(std::string_view base,
                     const std::unordered_set<std::string> &taken) {
    for (std::uint64_t suffix = 1;; ++suffix) {
        auto candidate = std::string{base} + "_copy" + std::to_string(suffix);
        if (!taken.contains(candidate)) return candidate;
    }
}

} // namespace

// OL-1: copy an object and its descendants. The copies keep their relative
// declaration order as one contiguous block, by default right after the source
// subtree. Each copy is renamed `<name>_copy<N>` and parent links inside the
// subtree follow the renames; the root copy keeps the source's parent. A
// prefab instance is copied as an instance under a fresh instance_id, which the
// journal records. graph.instance owners are not copied: they name the source's
// AuthoringObjectId, which the copy does not take.
PreparedOperation prepareDuplicate(const Json &raw,
                                   const AuthoringSceneDocument &document) {
    constexpr auto context = "duplicate";
    requireOnly(raw, {"op", "object_id", "declaration_index"}, context);
    const auto object_id = AuthoringObjectId{
        exactUnsigned(raw.at("object_id"), "duplicate object_id", true)};
    const auto root = requireObjectLocation(document, object_id);
    auto subtree = subtreeObjects(document, root);
    std::sort(subtree.begin(), subtree.end(),
              [](const auto &left, const auto &right) {
                  return left.object_index < right.object_index;
              });

    std::unordered_set<std::string> names;
    std::unordered_set<std::string> instance_ids;
    std::size_t scene_size = 0;
    for (const auto &object : objectLocations(document)) {
        if (object.scene_id != root.scene_id) continue;
        ++scene_size;
        if (object.name) names.insert(*object.name);
        const auto prefab = object.authored.find("prefab");
        if (prefab == object.authored.end() || !prefab->is_object()) continue;
        const auto instance = prefab->find("instance_id");
        if (instance != prefab->end() && instance->is_string()) {
            instance_ids.insert(instance->get<std::string>());
        }
    }

    auto declaration_index = subtree.back().object_index + 1U;
    if (const auto found = raw.find("declaration_index"); found != raw.end()) {
        declaration_index = static_cast<std::size_t>(
            exactUnsigned(*found, "duplicate declaration_index"));
        if (declaration_index > scene_size) {
            editFailure(EditorEditErrorCode::schema_violation,
                        {{"object", object_id.value}, {"slot", nullptr},
                         {"field_path", "/declaration_index"},
                         {"detail", "declaration index is outside the scene"}},
                        "duplicate declaration index is outside the scene");
        }
    }

    std::unordered_map<std::string, std::string> renamed;
    for (const auto &object : subtree) {
        if (!object.name) continue;
        auto copy = copyName(*object.name, names);
        names.insert(copy);
        renamed.emplace(*object.name, std::move(copy));
    }

    PreparedOperation result;
    OrderedJson objects = OrderedJson::array();
    OrderedJson new_instance_ids = OrderedJson::array();
    OrderedJson name_reservations = OrderedJson::array();
    OrderedJson declaration_indices = OrderedJson::array();
    for (std::size_t index = 0; index < subtree.size(); ++index) {
        const auto &object = subtree[index];
        Json copy = object.authored;
        if (object.name) {
            copy["name"] = renamed.at(*object.name);
            name_reservations.push_back(renamed.at(*object.name));
            result.read_set.push_back("/scenes/" + root.scene_id +
                                      "/name_reservations/" +
                                      renamed.at(*object.name));
        }
        if (object.parent) {
            if (const auto parent = renamed.find(*object.parent);
                parent != renamed.end()) {
                copy["parent"] = parent->second;
            }
        }
        if (const auto prefab = copy.find("prefab");
            prefab != copy.end() && prefab->is_object()) {
            const auto instance = prefab->find("instance_id");
            if (instance != prefab->end() && instance->is_string()) {
                auto fresh = copyName(instance->get<std::string>(), instance_ids);
                if (!isPrefabIdentifier(fresh)) {
                    editFailure(
                        EditorEditErrorCode::schema_violation,
                        {{"object", object.object_id.value}, {"slot", nullptr},
                         {"field_path", "/prefab/instance_id"},
                         {"detail", "the copy's instance_id would not be a valid "
                                    "prefab identifier: " + fresh}},
                        "duplicate cannot name the copied prefab instance");
                }
                instance_ids.insert(fresh);
                (*prefab)["instance_id"] = fresh;
                new_instance_ids.push_back(
                    {{"source_object_id", object.object_id.value},
                     {"instance_id", std::move(fresh)}});
            }
        }
        const auto target_index = declaration_index + index;
        declaration_indices.push_back(target_index);
        result.read_set.push_back("/authoring_objects/" +
                                  std::to_string(object.object_id.value));
        result.commands.push_back(
            makeInsertObjectCommand(root.scene_id, target_index, copy));
        objects.push_back({{"source_object_id", object.object_id.value},
                           {"object", std::move(copy)}});
    }

    result.forward = {{"op", context},
                      {"source_object_id", object_id.value},
                      {"scene_id", root.scene_id},
                      {"declaration_index", declaration_index},
                      {"objects", std::move(objects)}};
    if (!new_instance_ids.empty()) {
        result.forward["new_instance_ids"] = std::move(new_instance_ids);
    }
    result.inverse = {{"op", "remove_objects"},
                      {"object_ids", OrderedJson::array()}};
    result.stable_target = "/scenes/" + root.scene_id + "/objects";
    result.write_set = {"/scenes/" + root.scene_id + "/objects"};
    result.structural_domain = {{"kind", "object_existence"},
                                {"scene_id", root.scene_id},
                                {"source", object_id.value},
                                {"objects", OrderedJson::array()},
                                {"name_reservations", std::move(name_reservations)},
                                {"declaration_indices", std::move(declaration_indices)}};
    return result;
}

} // namespace EditorJournalInternal
} // namespace Pelican
