#include "inspectormodel.hpp"
#include "frameplanmodel.hpp"
#include "playstatemodel.hpp"
#include "renderdocumentreload.hpp"

#include <schemawire.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <concepts>
#include <fstream>
#include <sstream>
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
    // MI-1s, decision 5 (engine MI-1): a model the project has not registered.
    if (auto unknown = modelUnknownEditDescription(*error); !unknown.empty()) return unknown;
    if (error->value("code", std::string{}) == "animation.component_invalid@1") {
        auto message = error->value("message", std::string{});
        const auto payload = error->find("payload");
        if (payload != error->end() && payload->is_object()) {
            const auto hint = payload->value("hint", std::string{});
            if (!hint.empty()) message += "\n" + hint;
        }
        return message;
    }
    const auto message = error->find("message");
    return message != error->end() && message->is_string()
               ? message->get<std::string>()
               : std::string{"editor command failed"};
}

std::string describeFailure(const Json &result) {
    const auto code = resultErrorCode(result);
    const auto message = resultErrorMessage(result);
    // MI-1s: said in words, without method_unavailable in front of it.
    if (const auto error = result.find("error");
        error != result.end() && !modelUnknownEditDescription(*error).empty())
        return message;
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
        // MT-2s: a list (simplemodelview.materials since engine MT-2) is no
        // field of its own; its rows are drawn from the schema node
        // (collectInspectorLists).
        if (field.is_object() && field.value("type", std::string{}) == "list") continue;
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
        // UX-10, decision 6: what SC-1 adds beside a declaration.
        const auto strings = [&field](const char *key) {
            std::vector<std::string> values;
            if (const auto found = field.find(key); found != field.end() && found->is_array())
                for (const auto &value : *found)
                    if (value.is_string()) values.push_back(value.get<std::string>());
            return values;
        };
        if (const auto found = field.find("discriminant");
            found != field.end() && found->is_string())
            descriptor.discriminant = found->get<std::string>();
        descriptor.applies_to = strings("applies_to");
        descriptor.required_for = strings("required_for");
        if (const auto found = field.find("default"); found != field.end())
            descriptor.schema_default = *found;

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
    case AddObjectKind::Floor: return "Floor";
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
    const bool floor = kind == AddObjectKind::Floor;
    Json components = Json::array({
        {{"name", "transform"},
         {"pos", Json::array({0.0, floor ? -0.5 : 0.0, 0.0})},
         {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
         {"scale", floor ? Json::array({8.0, 1.0, 8.0}) : Json::array({1.0, 1.0, 1.0})}},
    });
    if (kind != AddObjectKind::Empty) {
        const auto shape = floor ? AddObjectKind::Plane : kind;
        const auto index = static_cast<std::size_t>(shape) - 1;
        components.push_back({{"name", "simplemodelview"},
                              {"model", enginePrimitiveModelReferences().at(index)}});
    }
    return Json{{"name", name}, {"components", std::move(components)}};
}

nlohmann::json modelObjectDeclaration(const std::string &name, const std::string &model) {
    auto declaration = addObjectDeclaration(AddObjectKind::Cube, name);
    declaration["components"][1]["model"] = model;
    return declaration;
}

std::string modelObjectBaseName(std::string_view model) {
    std::string name;
    for (const char c : model)
        name.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_');
    if (name.find_first_not_of('_') == std::string::npos) return "Model";
    return name;
}

namespace {

constexpr double DegreesPerRadian = 57.295779513082320876798154814105;

// (-180, 180]: -180 and 180 are one angle, and the field shows it as 180.
double wrapDegrees(double degrees) {
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped <= -180.0) wrapped += 360.0;
    else if (wrapped > 180.0) wrapped -= 360.0;
    return wrapped;
}

} // namespace

std::array<double, 3>
eulerDegreesFromQuaternion(const std::array<double, 4> &quaternion) {
    const double length = std::sqrt(quaternion[0] * quaternion[0] +
                                    quaternion[1] * quaternion[1] +
                                    quaternion[2] * quaternion[2] +
                                    quaternion[3] * quaternion[3]);
    if (!(length > 0.0) || !std::isfinite(length)) return {0.0, 0.0, 0.0};
    const double x = quaternion[0] / length;
    const double y = quaternion[1] / length;
    const double z = quaternion[2] / length;
    const double w = quaternion[3] / length;
    const double sin_pitch = 2.0 * (w * y - z * x);
    // Gimbal lock: only Z - X (at +90) or Z + X (at -90) is defined. X is 0.
    if (std::abs(sin_pitch) >= 1.0 - 1.0e-12) {
        const double yaw = 2.0 * std::atan2(z, w) * DegreesPerRadian;
        return {0.0, sin_pitch > 0.0 ? 90.0 : -90.0, wrapDegrees(yaw)};
    }
    const double roll = std::atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y));
    const double pitch = std::asin(sin_pitch);
    const double yaw = std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
    return {wrapDegrees(roll * DegreesPerRadian), pitch * DegreesPerRadian,
            wrapDegrees(yaw * DegreesPerRadian)};
}

std::array<double, 4>
quaternionFromEulerDegrees(const std::array<double, 3> &degrees) {
    const double half = 0.5 / DegreesPerRadian;
    const double cx = std::cos(degrees[0] * half), sx = std::sin(degrees[0] * half);
    const double cy = std::cos(degrees[1] * half), sy = std::sin(degrees[1] * half);
    const double cz = std::cos(degrees[2] * half), sz = std::sin(degrees[2] * half);
    // q = qz * qy * qx: X is applied first.
    std::array<double, 4> result{
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz,
    };
    if (result[3] < 0.0)
        for (double &component : result) component = -component;
    // What the old four fields showed for no rotation, exactly.
    for (double &component : result)
        if (std::abs(component) < 1.0e-15) component = 0.0;
    return result;
}

bool isEulerRotationField(const InspectorWidgetDescriptor &descriptor) {
    return descriptor.component_slot == "transform" &&
           descriptor.field_name == "rotation" && descriptor.columns == 4 &&
           descriptor.kind == InspectorWidgetKind::QuaternionDrag;
}

namespace {
// The value the discriminant a field names holds now: authored, else its
// schema default; empty when there is none.
std::optional<std::string> discriminantValue(const InspectorWidgetDescriptor &field,
                                             const std::vector<InspectorWidgetDescriptor> &fields) {
    if (field.discriminant.empty()) return std::nullopt;
    for (const auto &candidate : fields) {
        if (candidate.field_name != field.discriminant) continue;
        if (candidate.authored && candidate.value.is_string())
            return candidate.value.get<std::string>();
        if (candidate.schema_default.is_string()) return candidate.schema_default.get<std::string>();
        return std::string{};
    }
    return std::string{};
}

bool listed(const std::vector<std::string> &values, const std::string &value) {
    return std::ranges::find(values, value) != values.end();
}
} // namespace

bool inspectorFieldShown(const InspectorWidgetDescriptor &field,
                         const std::vector<InspectorWidgetDescriptor> &fields) {
    const auto value = discriminantValue(field, fields);
    if (!value || field.applies_to.empty()) return true;
    return listed(field.applies_to, *value);
}

bool inspectorFieldRequiredMissing(const InspectorWidgetDescriptor &field,
                                   const std::vector<InspectorWidgetDescriptor> &fields) {
    const auto value = discriminantValue(field, fields);
    if (!value || !listed(field.required_for, *value) || !inspectorFieldShown(field, fields))
        return false;
    return !field.authored || field.value.is_null() ||
           (field.value.is_string() && field.value.get<std::string>().empty());
}

double inspectorDisplayScale(const InspectorWidgetDescriptor &field) {
    const bool radians = field.unit == "rad" || field.unit == "radian" || field.unit == "radians" ||
                         (field.component_slot == "camera" &&
                          (field.field_name == "controller.yaw" ||
                           field.field_name == "controller.pitch"));
    const bool scalar = field.kind == InspectorWidgetKind::FloatingPointDrag;
    return radians && scalar ? DegreesPerRadian : 1.0;
}

std::string inspectorAngleTip(const InspectorWidgetDescriptor &field) {
    if (field.component_slot == "camera" && field.field_name == "controller.yaw")
        return "Yaw: the turn about +Y from +Z towards -X, in degrees (a light's azimuth turns the same way)";
    if (field.component_slot == "camera" && field.field_name == "controller.pitch")
        return "Pitch: degrees from the horizon towards +Y (negative looks down)";
    return {};
}

std::string inspectorDirectionAngleTip(std::size_t index) {
    return index == 0 ? "Azimuth: the turn about +Y from +Z towards -X, in degrees (as a camera's yaw)"
                      : "Elevation: degrees above the horizon (negative points down)";
}

bool isDirectionField(const InspectorWidgetDescriptor &descriptor) {
    return descriptor.field_name == "direction" && descriptor.columns == 3 &&
           descriptor.kind == InspectorWidgetKind::VectorDrag;
}

std::array<double, 2> directionAnglesDegrees(const std::array<double, 3> &direction) {
    const double length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                    direction[2] * direction[2]);
    if (!(length > 0.0) || !std::isfinite(length)) return {0.0, 0.0};
    const double x = direction[0] / length;
    const double y = std::clamp(direction[1] / length, -1.0, 1.0);
    const double z = direction[2] / length;
    const double elevation = std::asin(y) * DegreesPerRadian;
    if (std::abs(x) < 1.0e-12 && std::abs(z) < 1.0e-12) return {0.0, elevation};
    // UX-14, decision 3: the azimuth turns as a camera.controller's yaw does
    // (engine CC-1): 0 is +Z, and it grows towards -X.
    return {wrapDegrees(std::atan2(-x, z) * DegreesPerRadian), elevation};
}

std::array<double, 3> directionFromAnglesDegrees(const std::array<double, 2> &angles) {
    const double azimuth = angles[0] / DegreesPerRadian;
    const double elevation = angles[1] / DegreesPerRadian;
    std::array<double, 3> result{-std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                                 std::cos(elevation) * std::cos(azimuth)};
    // Straight down is (0, -1, 0), not a rounding away from it.
    for (double &component : result)
        if (std::abs(component) < 1.0e-12) component = 0.0;
    return result;
}

bool isColorField(const InspectorWidgetDescriptor &descriptor) {
    return descriptor.field_name == "color" &&
           (descriptor.columns == 3 || descriptor.columns == 4) &&
           descriptor.kind == InspectorWidgetKind::VectorDrag;
}

bool isTextureReferenceField(const InspectorWidgetDescriptor &descriptor) {
    return descriptor.component_slot == "sprite_view" && descriptor.json_pointer == "/texture" &&
           descriptor.kind == InspectorWidgetKind::StringInput;
}

std::vector<std::string> textureReferenceChoices(std::string_view current,
                                                 const std::vector<std::string> &engine_textures) {
    std::vector<std::string> choices;
    for (const auto &texture : engine_textures)
        if (!texture.empty() && std::ranges::find(choices, texture) == choices.end())
            choices.push_back(texture);
    if (!current.empty() && std::ranges::find(choices, current) == choices.end())
        choices.emplace_back(current);
    return choices;
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

namespace {

struct UnavailableComponentText {
    std::string_view code;
    std::string_view sentence;
    std::string_view tag;
};

// Q-6: the codes PG-1 answers with. A new code reads as itself until a row
// is added here.
constexpr std::string_view NotInThisBuild = "not in this build";
constexpr std::array<UnavailableComponentText, 8> UnavailableComponentTexts{{
    {"animation.module_unavailable@1", "This build has no animation module", NotInThisBuild},
    {"audio.module_unavailable@1", "This build has no audio module", NotInThisBuild},
    {"physics.module_unavailable@1", "This build has no physics module", NotInThisBuild},
    {"xr.module_unavailable@1", "This build has no XR module", NotInThisBuild},
    {"vat.module_unavailable@1", "This build has no VAT module", NotInThisBuild},
    {"seqplayer.module_unavailable@1", "This build has no sequence player module", NotInThisBuild},
    // MG-2a: ray group layouts are v3 only. The code keeps its old name; what
    // it means now is that the project declares no ray group layout at all.
    // RTL-2s b, decision 5: the way to one is Enable hit groups.
    {"pelican.ray_instance_requires_layout_v2@1",
     "No ray group layout: enable hit groups first",
     "needs hit groups"},
    // AU-2: a sound plays only when its extension and its first bytes both
    // say WAV, or both say Ogg Vorbis.
    {"audio.format_unsupported@1", "Sound extension and contents disagree (WAV or Ogg Vorbis)",
     "unsupported sound"},
}};

const UnavailableComponentText *findUnavailableComponentText(std::string_view code) {
    const auto found = std::find_if(UnavailableComponentTexts.begin(), UnavailableComponentTexts.end(),
                                    [code](const auto &entry) { return entry.code == code; });
    return found == UnavailableComponentTexts.end() ? nullptr : &*found;
}

} // namespace

bool isComponentUnavailableCode(std::string_view reason) {
    // "<word>.<word>...@<version>": lowercase words, no spaces. A sentence
    // has spaces and never an '@' followed only by digits.
    const auto at = reason.rfind('@');
    if (at == std::string_view::npos || at == 0 || at + 1 == reason.size()) return false;
    const auto name = reason.substr(0, at);
    const auto version = reason.substr(at + 1);
    if (!std::all_of(version.begin(), version.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    if (name.find('.') == std::string_view::npos || name.front() == '.' || name.back() == '.' ||
        name.find("..") != std::string_view::npos)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

std::string componentUnavailableSentence(std::string_view code) {
    const auto *entry = findUnavailableComponentText(code);
    return entry ? std::string{entry->sentence} : std::string{};
}

std::string componentUnavailableDescription(std::string_view code) {
    const auto sentence = componentUnavailableSentence(code);
    if (sentence.empty()) return std::string{code};
    return sentence + " (" + std::string{code} + ")";
}

namespace {

// CT-6s, decision 5. One sentence per code of the ray tracing task parser
// (computetaskdefinitionjson.cpp); {task} is the task the message names
// ("compute task '<name>'"), {name} what it quotes after `quoted`. A new code
// reads as the engine's message until a row is added here.
struct RayTracingRefusalText {
    std::string_view code;
    std::string_view sentence;
    std::string_view quoted;
};
// RTL-2s, decision 3 (engine RTL-1a / RTL-1b): the rest of the task
// parser's codes, and the layout's and the assignments' parsers'.
constexpr RayTracingRefusalText RayLayoutRefusalTexts[] = {
    {"pelican.ray_tracing.generation_rolled_back@1",
     "The engine could not draw the first frame with the new ray tracing setup and kept the one before", ""},
    {"pelican.ray_tracing.shader_record_abi_mismatch@1",
     "Shader {name}'s shader record does not match the ray group layout's: every hit shader of a traced ray "
     "type declares the engine's fields (engine_record), then the layout's record, in order",
     "shader '"},
    {"pelican.ray_tracing.payload_mismatch@1",
     "A ray's payload is not the payload its miss or its hit group's shaders take (a ray type's shaders share "
     "one payload type)",
     ""},
    {"pelican.ray_tracing.ray_hit_group_invalid@1",
     "{task}'s hit shaders do not fit the hit group's type: a triangles group takes a closest hit and an any "
     "hit, a procedural one an intersection too",
     ""},
    {"pelican.ray_group_type_declared_in_task@1",
     "A hit group's type (triangles or procedural) is the ray group layout's: a task names only its shaders",
     ""},
    {"pelican.procedural_group_requires_intersection@1",
     "A procedural hit group needs an intersection shader and a closest-hit shader", ""},
    // RTL-3s: met by a hit shader without the intersection's hitAttributeEXT.
    {"pelican.procedural_hit_attribute_mismatch@1",
     "Shader {name}'s hit attribute is not its procedural hit group's intersection's: the group's hit shaders "
     "declare one hitAttributeEXT of the intersection's type",
     "shader '"},
    {"pelican.ray_profile_authored_anyhit@1",
     "The layout's material alpha profile makes the any-hit shader itself: {task} cannot name one", ""},
    {"pelican.ray_group_name_unknown@1", "The ray group layout has no such name", ""},
    {"pelican.ray_tracing.layout_shader_invalid@1", "{task}'s ray tracing places must each name a shader", ""},
    {"pelican.ray_tracing.layout_source_only@1",
     "With a ray group layout, {task}'s shaders are GLSL sources the engine compiles, not SPIR-V", ""},
    {"pelican.ray_tracing.callable_records_invalid@1",
     "{task} needs callable, callable_record and callable_records together, one record per callable", ""},
    {"pelican.ray_tracing.invalid_pipeline_declaration@1",
     "{task}'s ray_tracing is not a ray tracing pipeline the engine reads", ""},
    {"pelican.ray_tracing.ambiguous_pipeline_declaration@1",
     "{task} names a shader and a ray tracing pipeline both: it runs one of them", ""},
    {"pelican.ray_tracing.layout_requires_runtime_compiler@1",
     "A ray group layout needs the engine's runtime shader compiler, which this build does not have", ""},
    {"pelican.ray_tracing.bindings_without_layout@1",
     "Hit group assignments need a ray group layout: the root names assignments and no layout", ""},
    {"pelican.ray_group_layout.version_unsupported@1",
     "The ray group layout's version is not one this engine reads", ""},
    {"pelican.ray_group_layout_invalid@1", "The ray group layout is not one the engine reads", ""},
    {"pelican.ray_record_name_reserved@1", "A record field of the ray group layout has a name the engine keeps", ""},
    {"pelican.ray_record_type_unsupported@1",
     "A record field of the ray group layout has a type other than i32, u32, f32, vec2, vec3 or vec4", ""},
    {"pelican.ray_engine_record_unknown@1",
     "The layout's engine_record names a field the engine does not have (material_index, geometry_index, "
     "geometry_kind, procedural_transform)",
     ""},
    {"pelican.ray_engine_record_duplicate@1", "The layout's engine_record names a field twice", ""},
    {"pelican.ray_layout_destination_exists@1",
     "A file is already where the ray layout's document would be written, and the root does not name it", ""},
    {"pelican.ray_bindings.version_unsupported@1",
     "The hit group assignments' version is not one this engine reads", ""},
    {"pelican.ray_binding_record_invalid@1",
     "A hit group assignment's record (or a procedural asset's) does not have exactly the layout's record fields",
     ""},
    {"pelican.ray_binding_selector_exactly_one@1",
     "A hit group assignment selects a material or a procedural asset, exactly one", ""},
    {"pelican.ray_binding_selector_duplicate@1", "Two hit group assignments select the same material", ""},
    {"pelican.ray_row_binding_ambiguous@1",
     "Objects of one material are assigned to different hit groups or records", ""},
    {"pelican.procedural_selector_duplicate@1", "A procedural asset is assigned to a hit group twice", ""},
    {"pelican.procedural_binding_record_forbidden@1",
     "A procedural asset's record is the asset's own, not its assignment's", ""},
    {"pelican.procedural_group_unresolved@1",
     "A procedural object has no procedural hit group: assign its asset to one", ""},
    {"pelican.procedural_group_kind_conflict@1",
     "A procedural asset is assigned to a hit group that is not procedural", ""},
    {"pelican.ray_bindings_invalid@1", "The hit group assignments are not ones the engine reads", ""},
};

constexpr std::array<RayTracingRefusalText, 16> RayTracingRefusalTexts{{
    {"pelican.ray_layout_mode_change_requires_restart@1",
     "Whether the project has a ray group layout changes only when the engine restarts: restart "
     "it to apply this",
     ""},
    {"pelican.ray_tracing.legacy_syntax_with_layout@1",
     "This project has a ray group layout: {task} names its shaders in traced_ray_types, misses "
     "and hit_shaders, not miss and closesthit",
     ""},
    {"pelican.ray_tracing.unknown_shader_stage@1",
     "{task}'s ray_tracing has {name}, which the engine does not read", "field '"},
    {"pelican.ray_tracing.unsupported_shader_stage@1",
     "{task} cannot name a {name} shader here: any-hit and intersection shaders belong to a hit "
     "group of a ray group layout",
     "stage '"},
    {"pelican.ray_tracing.shader_stage_required@1",
     "{task} needs a raygen, a miss and a closesthit shader", ""},
    {"pelican.ray_tracing.invalid_shader_reference@1",
     "{task}'s ray tracing stages must each name a shader", ""},
    {"pelican.ray_tracing.traced_ray_types_invalid@1",
     "{task}'s traced_ray_types must be ray types of the ray group layout, each once", ""},
    {"pelican.ray_tracing.miss_shader_missing@1",
     "{task} needs a miss shader for every miss of the ray group layout", ""},
    {"pelican.ray_tracing.miss_shader_unknown@1", "The ray group layout has no miss {name}", "miss='"},
    {"pelican.ray_tracing.hit_shader_missing@1",
     "{task} needs a closest-hit shader for every ray type it traces and every hit group", ""},
    {"pelican.ray_tracing.hit_shader_untraced@1",
     "{task} gives hit shaders for {name}, a ray type it does not trace", "ray type '"},
    {"pelican.ray_tracing.hit_shader_unknown_group@1",
     "The ray group layout has no hit group {name}", "group='"},
    {"pelican.ray_tracing.invalid_recursion@1",
     "{task}'s max_recursion must be a whole number from 1", ""},
    {"pelican.ray_tracing.invalid_trace_dispatch@1",
     "A ray tracing task launches its rays with exactly one of rays [x, y, z], rays_from or "
     "indirect",
     ""},
    {"pelican.ray_tracing.rays_from_requires_pipeline@1",
     "Only a ray tracing task launches rays_from an image: a compute task counts groups", ""},
    {"pelican.ray_tracing.trace_extent_required@1",
     "{task} needs to say how many rays it launches: rays, rays_from or indirect", ""},
}};

// The text after `marker` in `message`, up to the next quote.
std::string quotedAfter(std::string_view message, std::string_view marker) {
    if (marker.empty()) return {};
    const auto at = message.find(marker);
    if (at == std::string_view::npos) return {};
    const auto begin = at + marker.size();
    const auto end = message.find('\'', begin);
    return std::string{message.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin)};
}

} // namespace

namespace {
std::string rayTracingRefusalSentence(const RayTracingRefusalText &found, std::string_view message,
                                      const nlohmann::json &error);
} // namespace

std::string rayTracingRefusalDescription(std::string_view code, std::string_view message) {
    return rayTracingRefusalDescription(nlohmann::json{{"code", std::string{code}}, {"message", std::string{message}}});
}

std::string rayTracingRefusalDescription(const nlohmann::json &relayed) {
    // A relayed refusal may carry its fields as error_fields (an edit's
    // reply); the engine's own puts them beside its code.
    nlohmann::json error = relayed;
    if (relayed.is_object())
        if (const auto inner = relayed.find("error_fields"); inner != relayed.end() && inner->is_object())
            for (const auto &[key, value] : inner->items())
                if (!error.contains(key)) error[key] = value;
    const auto code_text = error.is_object() ? error.value("code", std::string{}) : std::string{};
    const auto message_text = error.is_object() ? error.value("message", std::string{}) : std::string{};
    const std::string_view code = code_text;
    const std::string_view message = message_text;
    const RayTracingRefusalText *entry = nullptr;
    // RTL-2s: the refusal's own code first (a rolled-back generation quotes
    // the code that rolled it back), then a code its message carries.
    const auto find = [&](auto matches) {
        for (const auto &candidate : RayLayoutRefusalTexts)
            if (matches(candidate)) return &candidate;
        for (const auto &candidate : RayTracingRefusalTexts)
            if (matches(candidate)) return &candidate;
        return static_cast<const RayTracingRefusalText *>(nullptr);
    };
    entry = find([&](const RayTracingRefusalText &candidate) { return code == candidate.code; });
    if (entry == nullptr)
        entry = find([&](const RayTracingRefusalText &candidate) {
            return message.find(candidate.code) != std::string_view::npos;
        });
    if (entry == nullptr) return {};
    // RTL-2s, decision 3 (engine RTL-1b): where, from the refusal's fields
    // (beside its code), else from what the parser's message quotes.
    const auto field = [&](const char *key, std::string_view marker) {
        auto value = error.is_object() && error.contains(key) && error.at(key).is_string()
                         ? error.at(key).get<std::string>()
                         : std::string{};
        if (value.empty() && !marker.empty()) {
            const auto own = message.substr(std::min(message.size(), message.find(entry->code)));
            value = quotedAfter(own, marker);
        }
        return value == "<all>" ? std::string{} : value;
    };
    std::vector<std::string> where;
    if (const auto ray_type = field("ray_type", "ray_type='"); !ray_type.empty())
        where.push_back("ray type " + ray_type);
    if (const auto group = field("group", "group='"); !group.empty()) where.push_back("hit group " + group);
    if (const auto miss = field("miss", "miss='"); !miss.empty()) where.push_back("miss " + miss);
    if (const auto stage = field("stage", {}); !stage.empty()) where.push_back("stage " + stage);
    if (entry->code == "pelican.ray_group_name_unknown@1")
        if (const auto name = field("name", {}); !name.empty()) {
            const auto kind = field("kind", {});
            where.push_back((kind == "ray_type" ? std::string{"ray type"} : kind == "group" ? "hit group" : kind) +
                            " " + name);
        }
    auto described = rayTracingRefusalSentence(*entry, message, error);
    std::string at;
    for (const auto &part : where) {
        // A name the sentence already says is not said again.
        const auto value = part.substr(part.rfind(' ') + 1);
        if (described.find(value) != std::string::npos) continue;
        at += (at.empty() ? "" : ", ") + part;
    }
    if (!at.empty()) described += " (" + at + ")";
    return described + " (" + std::string{entry->code} + ").";
}

namespace {

// RTL-3s, decision 6 (engine EB-3): a shader's file as a sentence names it --
// project://shaders/x.rchit is shaders/x.rchit.
std::string refusalShaderName(const nlohmann::json &shader) {
    if (!shader.is_string()) return {};
    auto name = shader.get<std::string>();
    if (name.starts_with("project://")) name.erase(0, std::string_view{"project://"}.size());
    return name;
}

// The record fields a refusal's expected lists, "material_index u32, ...".
std::string refusalRecordFields(const nlohmann::json &fields) {
    std::string listed;
    for (const auto &field : fields) {
        if (!field.is_object()) continue;
        const auto name = field.value("name", std::string{});
        const auto type = field.value("type", std::string{});
        listed += (listed.empty() ? "" : ", ") + (name.empty() ? "?" : name) + (type.empty() ? "" : " " + type);
    }
    return listed;
}

// shader_record_abi_mismatch and payload_mismatch from their fields
// (shader, stage, expected, actual): what to do to which shader. Empty when
// the refusal has no fields (an engine before EB-3): the table's sentence.
std::string rayShaderContractSentence(std::string_view code, const nlohmann::json &error) {
    if (!error.is_object() || !error.contains("expected")) return {};
    const auto &expected = error.at("expected");
    const auto shader = refusalShaderName(error.value("shader", nlohmann::json{}));
    const auto stage = error.value("stage", std::string{});
    if (code == "pelican.ray_tracing.shader_record_abi_mismatch@1") {
        const auto named = shader.empty() ? std::string{"The shader"} : "Shader " + shader;
        if (expected.is_array() && !expected.empty())
            return named + " declares a shader record that is not the ray group layout's: declare its fields in "
                           "order (" + refusalRecordFields(expected) + "), or take the shaderRecordEXT declaration out";
        if (expected.is_array())
            return named + " declares a shader record and the ray group layout has none: take its shaderRecordEXT "
                           "declaration out";
        return named + " declares a shader record where none is written (" +
               (stage.empty() ? std::string{"a raygen, a miss or an untraced ray type"} : "its stage " + stage) +
               "): take its shaderRecordEXT declaration out";
    }
    if (code == "pelican.ray_tracing.payload_mismatch@1") {
        const auto wanted = expected.is_string() ? expected.get<std::string>() : std::string{"?"};
        const auto &actual = error.contains("actual") ? error.at("actual") : nlohmann::json{};
        if (shader.empty())
            return "No " + (stage.empty() ? std::string{"hit"} : stage) + " shader takes the ray's payload (" +
                   wanted + ") here: pick one";
        if (stage == "anyhit" && error.contains("closesthit"))
            return "Any hit " + shader + " takes the payload as " +
                   (actual.is_string() ? actual.get<std::string>() : std::string{"nothing"}) +
                   " and the closest hit beside it (" +
                   refusalShaderName(error.at("closesthit")) + ") as " + wanted + ": make them one type";
        std::string caller;
        if (const auto found = error.find("caller"); found != error.end() && found->is_object())
            caller = refusalShaderName(found->value("shader", nlohmann::json{}));
        const auto by = caller.empty() ? std::string{} : " (traced by " + caller + ")";
        if (!actual.is_string())
            return "Shader " + shader + " declares no incoming payload, and the ray it answers carries " + wanted +
                   by + ": declare rayPayloadInEXT " + wanted;
        return "Shader " + shader + " takes the payload as " + actual.get<std::string>() +
               ", and the ray it answers carries " + wanted + by + ": make them one type";
    }
    return {};
}

std::string rayTracingRefusalSentence(const RayTracingRefusalText &found, std::string_view message,
                                      const nlohmann::json &error) {
    const auto *entry = &found;
    if (auto contract = rayShaderContractSentence(entry->code, error); !contract.empty()) return contract;
    // The parser's own message, from its code on: the task and what it quotes
    // (RTL-2s: the task the refusal's fields name, when it has them).
    const auto own = message.substr(std::min(message.size(), message.find(entry->code)));
    auto task = error.is_object() && error.contains("task") && error.at("task").is_string()
                    ? error.at("task").get<std::string>()
                    : std::string{};
    if (task.empty()) task = quotedAfter(own, "task '");
    const auto name = quotedAfter(own, entry->quoted);
    std::string pattern{entry->sentence};
    // A layout's fields where the project has none: the other form.
    if (entry->code == "pelican.ray_tracing.unknown_shader_stage@1" &&
        (name == "traced_ray_types" || name == "misses" || name == "hit_shaders"))
        pattern = "This project has no ray group layout: {task} names its shaders in miss and "
                  "closesthit, not {name}";
    std::string sentence;
    for (std::size_t at = 0; at < pattern.size();) {
        const auto open = pattern.find('{', at);
        if (open == std::string::npos) {
            sentence += pattern.substr(at);
            break;
        }
        const auto close = pattern.find('}', open);
        sentence += pattern.substr(at, open - at);
        const auto key = pattern.substr(open + 1, close - open - 1);
        if (key == "task")
            sentence += task.empty() ? (open == 0 ? "The task" : "the task") : "Task " + task;
        else
            sentence += name.empty() ? "?" : name;
        at = close + 1;
    }
    return sentence;
}

} // namespace

std::string knownRefusalDescription(std::string_view code, std::string_view message,
                                    bool name_document) {
    // RL-1s, decision 5: the render document on disk was rewritten and its
    // reload refused, so every edit meets a file the engine did not load.
    // The engine's own words follow, for the tooltip and Copy message.
    if (code == "external_modification" && renderDocumentsRefusedOnDisk())
        return std::string{renderDiskRefusalSentence} + "\n" + std::string{code} + ": " +
               std::string{message};
    // BS-1s, decision 7 (engine BS-1b): a render document that is not JSON.
    if (code == "render_document_syntax@1" ||
        message.find("render_document_syntax@1") != std::string_view::npos)
        if (auto syntax = renderDocumentSyntaxDescription(
                nlohmann::json{{"code", "render_document_syntax@1"}, {"message", std::string{message}}});
            !syntax.empty())
            return syntax;
    if (findUnavailableComponentText(code) != nullptr)
        return componentUnavailableDescription(code);
    for (const auto &entry : UnavailableComponentTexts)
        if (message.find(entry.code) != std::string_view::npos)
            return componentUnavailableDescription(entry.code);
    // CT-6s, decision 5: a render.compute's ray tracing.
    if (auto ray = rayTracingRefusalDescription(code, message); !ray.empty()) return ray;
    // LP-4a: a view_select refusal carries its fields in the message.
    const nlohmann::json error{{"code", std::string{code}}, {"message", std::string{message}}};
    if (auto view_select = viewSelectRefusalDescription(error); !view_select.empty())
        return view_select;
    // PM-3a: the play refusals (set_play_state, advance_frames, step_frame).
    if (auto play = playRefusalDescription(error); !play.empty()) return play;
    // MI-1s, decision 5: register_model / unregister_model (engine MI-1).
    if (code.starts_with("model."))
        if (auto model = modelRefusalDescription(error); !model.empty()) return model;
    // LP-4c: and so does a loop's or a definition's.
    return graphLoopRefusalDescription(error, name_document);
}

namespace {

constexpr std::string_view ViewSelectAxisUnknownCode = "graph.view_select_axis_unknown@1";
constexpr std::string_view ViewSelectRuntimeAxisCode = "graph.view_select_runtime_axis@1";
constexpr std::string_view ViewSelectPrimaryFamilyCode = "graph.view_select_primary_family@1";
constexpr std::string_view ViewSelectNeedsTargetPlanCode =
    "graph.view_select_needs_target_plan@1";
constexpr std::array<std::string_view, 4> ViewSelectRefusalCodes{
    ViewSelectAxisUnknownCode, ViewSelectRuntimeAxisCode, ViewSelectPrimaryFamilyCode,
    ViewSelectNeedsTargetPlanCode};

// The fields object right after "<code>: " in a message, when there is one:
// the engine writes a named refusal as "<code>: <fields JSON>", and a broader
// refusal may quote it after words of its own.
std::optional<nlohmann::json> fieldsAfterCode(std::string_view message, std::string_view code) {
    for (auto at = message.find(code); at != std::string_view::npos;
         at = message.find(code, at + 1)) {
        auto rest = message.substr(at + code.size());
        if (!rest.starts_with(": {")) continue;
        rest.remove_prefix(2);
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (std::size_t index = 0; index < rest.size(); ++index) {
            const char c = rest[index];
            if (in_string) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                auto fields = nlohmann::json::parse(rest.substr(0, index + 1), nullptr, false);
                if (!fields.is_discarded() && fields.is_object()) return fields;
                break;
            }
        }
    }
    return std::nullopt;
}

// A field as the sentence shows it: a string as written, a number as a
// number; empty when the refusal does not carry it.
std::string refusalField(const nlohmann::json &fields, const char *key) {
    const auto found = fields.find(key);
    if (found == fields.end()) return {};
    if (found->is_string()) return found->get<std::string>();
    if (found->is_number_integer()) return found->dump();
    return {};
}

} // namespace

std::string viewFamilyExtentText(const nlohmann::json &extent) {
    const auto number = [](const nlohmann::json &value) {
        if (value.is_number_integer() || value.is_number_unsigned()) return value.dump();
        if (!value.is_number()) return std::string{"?"};
        std::ostringstream out;
        out.setf(std::ios::fixed);
        out.precision(1);
        const auto scalar = value.get<double>();
        // One decimal unless the number needs more ("×0.25").
        if (std::abs(scalar * 10.0 - std::round(scalar * 10.0)) > 1e-9) {
            out.unsetf(std::ios::fixed);
            out.precision(6);
        }
        out << scalar;
        return out.str();
    };
    if (extent.is_array() && extent.size() == 2)
        return number(extent[0]) + "×" + number(extent[1]);
    if (extent.is_object())
        if (const auto scale = extent.find("output_scale"); scale != extent.end())
            return "screen ×" + number(*scale);
    return extent.is_null() ? std::string{"?"} : extent.dump();
}

std::string viewFamilyExtentRefusalDescription(const nlohmann::json &error) {
    constexpr std::string_view code = "render.view_family_extent_mismatch@1";
    if (!error.is_object()) return {};
    const auto own_code = error.value("code", std::string{});
    const auto message = error.value("message", std::string{});
    nlohmann::json fields;
    if (own_code == code) {
        if (const auto relayed = error.find("error_fields");
            relayed != error.end() && relayed->is_object())
            fields = *relayed;
        else if (error.contains("family") || error.contains("pass"))
            fields = error;
    }
    if (!fields.is_object()) {
        auto quoted = fieldsAfterCode(message, code);
        if (!quoted) return {};
        fields = std::move(*quoted);
    }
    // A pass a loop copied: named with the loop and the body node it copies.
    const auto passName = [&](const char *pass_key, const char *origin_key) {
        auto name = refusalField(fields, pass_key);
        if (name.empty()) name = "a pass";
        const auto origin = fields.find(origin_key);
        if (origin != fields.end() && origin->is_object()) {
            const auto loop = origin->value("loop", std::string{});
            const auto node = origin->value("node", std::string{});
            if (!loop.empty() && !node.empty())
                name += " (loop " + loop + ", body node " + node + ")";
        }
        return name;
    };
    const auto family = refusalField(fields, "family");
    return passName("pass", "loop_origin") + " writes " + refusalField(fields, "target") +
           " at " + viewFamilyExtentText(fields.value("extent", nlohmann::json{})) + ", but " +
           (family.empty() ? std::string{"its view family"} : family) + " draws at " +
           viewFamilyExtentText(fields.value("expected_extent", nlohmann::json{})) +
           " (its first pass " + passName("expected_pass", "expected_loop_origin") +
           " writes " + refusalField(fields, "expected_target") +
           "): the passes of one view family draw at one size (" + std::string{code} + ").";
}

std::string viewSelectRefusalDescription(const nlohmann::json &error) {
    if (!error.is_object()) return {};
    // LP-4f2 (VF-1): a view family's passes at two sizes.
    if (auto extent = viewFamilyExtentRefusalDescription(error); !extent.empty()) return extent;
    const auto code_value = error.find("code");
    const std::string own_code = code_value != error.end() && code_value->is_string()
                                     ? code_value->get<std::string>()
                                     : std::string{};
    const auto message_value = error.find("message");
    const std::string message = message_value != error.end() && message_value->is_string()
                                    ? message_value->get<std::string>()
                                    : std::string{};
    std::string_view code;
    nlohmann::json fields;
    for (const auto candidate : ViewSelectRefusalCodes) {
        if (own_code != candidate) continue;
        code = candidate;
        // The edit service lays the fields beside the code; anything else
        // still has them in its message.
        fields = error.contains("pass") ? error
                                        : fieldsAfterCode(message, candidate)
                                              .value_or(nlohmann::json::object());
    }
    if (code.empty()) {
        for (const auto candidate : ViewSelectRefusalCodes) {
            if (auto quoted = fieldsAfterCode(message, candidate)) {
                code = candidate;
                fields = std::move(*quoted);
                break;
            }
        }
    }
    if (code.empty()) return {};
    auto pass = refusalField(fields, "pass");
    if (pass.empty()) pass = "A pass";
    const auto family = refusalField(fields, "family");
    const auto axis = refusalField(fields, "axis");
    std::string sentence;
    if (code == ViewSelectAxisUnknownCode) {
        const auto value = refusalField(fields, "value");
        const auto length = fields.find("length");
        if (!value.empty() && length != fields.end() && length->is_number_integer() &&
            length->get<std::int64_t>() >= 0) {
            const auto count = length->get<std::uint64_t>();
            sentence = pass + ": " + axis + " " + value + " is outside " + family + "'s " +
                       std::to_string(count) + " views " +
                       (count == 0 ? std::string{"(it has none)"}
                                   : "(0.." + std::to_string(count - 1) + ")");
        } else {
            sentence = pass + ": " + family + " has no view axis named " + axis;
        }
    } else if (code == ViewSelectRuntimeAxisCode) {
        sentence = pass + ": " + axis + " of " + family +
                   " is decided each frame and cannot be selected";
    } else if (code == ViewSelectPrimaryFamilyCode) {
        sentence = pass + " draws the main view (" + family + "), which has no axes to select";
    } else {
        sentence = pass + ": view_select needs the physical target plan, which this "
                          "configuration does not build";
    }
    return sentence + " (" + std::string{code} + ").";
}

namespace {

// LP-4c, decision 9. One sentence per code; `{field}` is the refusal's field
// of that name (a list is joined with ", "), and a field the refusal does not
// carry reads as "?". A new code reads as the engine's message until a row is
// added here.
struct GraphLoopRefusalText {
    std::string_view code;
    std::string_view sentence;
};
constexpr std::array<GraphLoopRefusalText, 53> GraphLoopRefusalTexts{{
    {"graph.loop_branch_else_missing@1",
     "Loop {node} has no else: its last branch must have no condition"},
    {"graph.loop_branch_interface@1",
     "Branch {branch} of loop {node} runs {definition}, whose inputs and outputs "
     "differ from the loop's at {port}"},
    {"graph.loop_nested@1",
     "A loop stands in the root document only, and its body cannot hold a loop ({node})"},
    {"graph.loop_feature_pending@1", "Loop {node} uses {feature}, which this engine does not run"},
    {"graph.loop_tier_unsupported@1", "Loop {node} has no stage it can run in ({reason})"},
    {"graph.loop_tier_mismatch@1", "Loop {node} is fixed to {tier}, but it can only run as {actual}"},
    {"graph.loop_order_unsupported@1", "Loop {node} cannot be ordered that way in its stage"},
    {"graph.loop_expression_invalid@1",
     "An expression of loop {node} is not valid: {reason} at {pointer}"},
    {"graph.loop_carry_type_mismatch@1",
     "Loop {node} carries {from} into {to}, which takes another type"},
    {"graph.loop_element_unknown@1",
     "Input {port} of {node} reads element {element}, which the loop it reads does not have"},
    {"graph.loop_output_empty@1", "Output {port} of loop {node} is empty: the loop ran no element"},
    {"graph.loop_expansion_limit@1", "Loop {node} would make {count} copies; the limit is {limit}"},
    {"graph.loop_expansion_referenced@1",
     "{name} is a name a loop makes, which cannot be referred to; name the loop ({pointer})"},
    {"graph.loop_contract_unsupported@1", "Loop {node} draws with {contract}, which its stage does not take"},
    // LP-4d2, decision 8 (engine 5e246063): the material's variant is
    // `material_variant`; `variant` is only ever a graph variant.
    {"graph.loop_variant_missing@1",
     "Loop {node}: material {material} has no {material_variant} variant"},
    {"graph.loop_phase_crossing@1", "Loop {node}: material {material} draws in another phase"},
    {"graph.loop_override_interface@1", "Loop {node}: material {material} has no binding {binding}"},
    {"graph.loop_override_needs_compiler@1",
     "Loop {node} changes shader defines, which needs the runtime shader compiler"},
    {"graph.loop_override_compile_limit@1",
     "Loop {node} needs {count} shader compiles; the limit is {limit}"},
    // LP-4e: node is the graph.instance that tried to run a draw body.
    {"graph.definition_role@1",
     "Definition {definition} holds draw steps, so only a draw_loop runs it; "
     "the instance {node} cannot"},
    {"graph.loop_draws_unconnected@1",
     "The draws of loop {node} reach {count} inputs: they must feed the draws of exactly "
     "one render.material_pass"},
    {"graph.definition_referenced@1", "Definition {definition} is still used by {referenced_by}"},
    {"graph.definition_duplicate@1", "A definition named {definition} already exists"},
    {"graph.definition_missing@1", "There is no definition named {definition}"},
    {"graph.definition_immutable@1",
     "{node} is made by an instance or a loop; edit it in its definition"},
    // Q-19 (LP-1d): a render node's numbered ports run from 0 without a gap;
    // `reason` says which way they do not (GraphInputIndexReasonTexts).
    {"graph.input_index_invalid@1",
     "The numbered inputs of {node} are not 0, 1, 2...: input {input_index} is {reason}"},
    // UX-5c (RS-13): a pass of the rendering section names a loop's output
    // as <loop>.<output>; `reason` says what is wrong with the name
    // (GraphLoopOutputReferenceReasonTexts).
    {"graph.loop_output_reference_unknown@1", "{pass} reads {reference}, which is no loop output"},
    // UX-5c (RS-9): a render node lacks a parameter its op needs.
    {"graph.render_node_parameters_missing@1", "The {op} node {node} needs {missing}"},
    // UX-12a, decision 4 (CT-3): a value of the wrong kind, outside the
    // enum or the range; {why} is said from `reason` (see below).
    {"graph.render_node_parameter_invalid@1", "{node}.{parameter} must be {expected} ({why})"},
    // UX-12e, decision 5 (CT-4): a pose (or a morph weight) reads the model
    // of the object whose instance runs it; the root has none.
    {"graph.op_requires_owned_instance@1",
     "{op} runs inside a definition attached to an object \xE2\x80\x94 make a definition and "
     "place it there"},
    // UX-8, decision 12 (c) (RS-15): the render.scalar sinks on one buffer
    // tile it from byte 0; `reason` says which way they do not.
    {"graph.render_scalar_layout_invalid@1",
     "The render.scalar {node} writes {target} at byte {offset}, where {expected_offset} was "
     "next ({reason})"},
    {"graph.render_scalar_type_mismatch@1",
     "The render.scalar {node} writes {components} float(s) into {target}, whose elements are "
     "{element}"},
    // UX-13b, decision 3 (engine CT-5): the buffer a render.scalar writes is
    // not a runtime_f32 one as large as its sinks; `expected` / `actual` are
    // {host_source, size?}, read as "<host_source>, <size> bytes". One the
    // graph declares names the sink (`node`); one declared outside it does
    // not (the sentence then says so).
    {"graph.render_scalar_target_incompatible@1",
     "The render.scalar {node} writes {target} ({actual}), which must be {expected} \xE2\x80\x94 {fix}"},
    // UX-8, decision 13 (b) (CT-1): a port declaration lacks a field.
    {"graph.port_field_missing@1", "Port {port} of {node} needs {field}"},
    // GL-3a (FG-1): a fold group's decisions. `subject` is {node, element?,
    // port?}, read as "node [element].port".
    {"graph.fold_decision_conflict@1",
     "{group} asks {field} for {name}, but the graph-wide setting says otherwise"},
    {"graph.fold_subject_outside@1",
     "Fold group {group} names {subject}, which is not one of its nodes"},
    {"graph.fold_kind_unsupported@1",
     "Fold group {group} is folded as {kind}; only decisions can be declared for now"},
    // GL-4a (FG-2): verify_fold could not judge.
    {"fold_verify_failed",
     "{group} could not be verified: its open form could not be drawn ({run} run)"},
    {"fold_verify_unavailable", "This engine cannot verify a fold group now ({reason})"},
    {"fold_verify_busy", "{group} is being verified already; wait for it to finish"},
    // LC-1s, decision 3 (engine LC-1): a stage's push-constant range covers
    // part of the loop element and count ([112, 120)). `range` is
    // {offset, size}, read as "offset..offset+size".
    {"shader.loop_push_overlap@1",
     "{shader} declares a push constant that overlaps the loop element at {range} in the "
     "{stage} stage \xE2\x80\x94 use PELICAN_LOOP_CUSTOM_PUSH and PELICAN_LOOP_PUSH_FIELDS"},
    // PS-1s, decision 5 (engine PS-1): a pass that runs once and what it
    // writes or reads; and rerun_once's node the plan does not have.
    {"graph.once_writes_transient@1",
     "{node} runs once but writes {resource}, which is not kept between frames \xE2\x80\x94 "
     "make it Persistent"},
    {"graph.once_reads_per_frame@1",
     "{node} runs once but reads {input}, which changes every frame \xE2\x80\x94 it would "
     "freeze at the first value"},
    {"node_unknown", "{node} is not in the frame plan: there is nothing of it to recompute"},
    // IA-1s, decision 2 (engine IA-1): an image that starts from an asset.
    {"graph.image_initial_requires_persistent@1",
     "{node} starts from an image but is not kept between frames \xE2\x80\x94 make it "
     "Persistent"},
    {"graph.image_initial_extent_conflict@1",
     "{node} takes its size from its image and from the screen at once \xE2\x80\x94 check From "
     "the image, or give it a fixed size"},
    // `what` picks the sentence (GraphImageInitialMismatchTexts); `actual`
    // and `expected` read as that sentence's words.
    {"graph.image_initial_mismatch@1",
     "{asset} is {actual} but {node} is {expected} \xE2\x80\x94 bake the image to match "
     "(pelican_cli image ktx2) or change the image's format"},
    // AB-1s, decision 2 (engine AB-1): value.sample_table reads a baked
    // table when the graph is built.
    {"value.sample_table_unreadable@1", "{node} cannot read the table {asset}: {reason}"},
    {"value.sample_table_row_unknown@1", "{asset} has no row {row}; its rows are {rows}"},
    // UX-15b, decision 2 (engine EL-2): a frozen physical plan (a
    // vulkan_physical_fragments package) whose fingerprint -- `fingerprint`
    // is "logical_graph" or "automatic_plan" -- is not the plan's any more.
    {"physical_fragment_stale",
     "The frozen plan no longer matches the graph ({fingerprint}); Eject again"},
    // FO-1s, decisions 1 and 4 (engine FO-1, EB-1). frame_node_order: the
    // order put `first` before `second`, but `second` must run first; {why}
    // is said from `reason` (frameNodeOrderWhy). An image's or a buffer's
    // key the engine does not read: {hint} is the nearest key it does, or
    // the keys it reads.
    {"frame_node_order.conflicts_dependency@1", "{second} must stay before {first}: {why}"},
    {"graph.image_key_unknown@1", "Image {image} has a key {key} the engine does not read{hint}"},
    {"graph.buffer_key_unknown@1", "Buffer {buffer} has a key {key} the engine does not read{hint}"},
}};

// IA-1s, decision 2: graph.image_initial_mismatch@1, by `what`. extent reads
// as "64×64", format as the format's name, color_space as "sRGB" or
// "linear" with the format beside it.
struct GraphImageInitialMismatchText {
    std::string_view what;
    std::string_view sentence;
};
constexpr std::array<GraphImageInitialMismatchText, 3> GraphImageInitialMismatchTexts{{
    {"extent",
     "{asset} is {actual} but {node} is {expected} \xE2\x80\x94 bake the image to match "
     "(pelican_cli image ktx2) or change the image's size"},
    {"format",
     "{asset} is {actual} but {node} is {expected} \xE2\x80\x94 bake the image to match "
     "(pelican_cli image ktx2) or change the image's format"},
    {"color_space",
     "{asset} is {actual} but {node} is {expected} \xE2\x80\x94 bake the image to match "
     "(pelican_cli image ktx2) or change the image's format to the other color space"},
}};

// UX-5c: graph.loop_output_reference_unknown@1's reasons, each a sentence that
// says what to change.
struct GraphLoopOutputReferenceReasonText {
    std::string_view reason;
    std::string_view sentence;
};
constexpr std::array<GraphLoopOutputReferenceReasonText, 6> GraphLoopOutputReferenceReasonTexts{{
    {"loop_unknown",
     "{pass} reads {reference}, but no loop, target or buffer has that name"},
    {"not_a_loop", "{pass} reads {reference}, but the node before the dot is not a loop"},
    {"output_unknown", "{pass} reads {reference}, but the loop has no output of that name"},
    {"per_element",
     "{pass} reads {reference}, an output with one value per element: a pass reads a loop's "
     "single output (the last element's)"},
    {"not_a_resource",
     "{pass} reads {reference}, which is a value, not an image or a buffer"},
    {"loop_inactive",
     "{pass} reads {reference}, but this graph variant leaves that loop out"},
}};

// Q-19: graph.input_index_invalid@1's two reasons, each a sentence of its own.
struct GraphInputIndexReasonText {
    std::string_view reason;
    std::string_view sentence;
};
constexpr std::array<GraphInputIndexReasonText, 2> GraphInputIndexReasonTexts{{
    {"missing",
     "No port of {node} reads input {input_index}: its numbered inputs run from 0 without a gap"},
    {"duplicate", "Ports {ports} of {node} all read input {input_index}: each number is one port's"},
}};

// UX-8, decision 12 (c): graph.render_scalar_layout_invalid@1's two reasons.
constexpr std::array<GraphInputIndexReasonText, 2> GraphScalarLayoutReasonTexts{{
    {"overlap",
     "The render.scalar {node} writes {target} from byte {offset}, over the sink before it "
     "(which ends at byte {expected_offset})"},
    {"gap",
     "The render.scalar {node} writes {target} from byte {offset}, leaving bytes "
     "{expected_offset} up to {offset} unwritten"},
}};

// LP-4e: graph.loop_tier_unsupported@1 says why in `reason`; the draw_loop
// reasons each have a sentence that says what to change. Any other reason
// reads as the code's own sentence.
struct GraphLoopTierReasonText {
    std::string_view reason;
    std::string_view sentence;
};
constexpr std::array<GraphLoopTierReasonText, 4> GraphLoopTierReasonTexts{{
    {"carry_in_draw_loop",
     "Loop {node} draws inside one material pass, where no material hands a value to the "
     "next: remove its carry"},
    {"limit_in_draw_loop", "Loop {node} draws every material of its pass: remove its limit"},
    {"resource_in_draw_body",
     "A body of loop {node} makes a resource, which a draw_loop body cannot: it holds draw "
     "steps only"},
    {"mixed_body",
     "The bodies of loop {node} mix draw steps with passes: a body holds draw steps only "
     "(draw_loop) or passes only (expand)"},
}};

// The fields after a code in a message: "<code> {...}" (how a GraphRegistryError
// spells itself) or "<code>: {...}".
std::optional<nlohmann::json> graphFieldsAfterCode(std::string_view message, std::string_view code) {
    if (auto fields = fieldsAfterCode(message, code)) return fields;
    const auto at = message.find(code);
    if (at == std::string_view::npos) return std::nullopt;
    auto rest = message.substr(at + code.size());
    if (!rest.starts_with(" {")) return std::nullopt;
    rest.remove_prefix(1);
    // The object runs to its matching brace; the parser is asked to take
    // exactly that much by trying each closing brace in turn.
    for (std::size_t end = rest.find('}'); end != std::string_view::npos;
         end = rest.find('}', end + 1)) {
        auto fields = nlohmann::json::parse(rest.substr(0, end + 1), nullptr, false);
        if (!fields.is_discarded() && fields.is_object()) return fields;
    }
    return std::nullopt;
}

std::string graphRefusalFieldText(const nlohmann::json &fields, std::string_view key) {
    const auto found = fields.find(std::string{key});
    if (found == fields.end()) return "?";
    if (found->is_string()) return found->get<std::string>();
    if (found->is_number_integer() || found->is_number_unsigned()) return found->dump();
    if (found->is_array()) {
        std::string joined;
        for (const auto &value : *found) {
            if (!joined.empty()) joined += ", ";
            joined += value.is_string() ? value.get<std::string>() : value.dump();
        }
        return joined;
    }
    return found->dump();
}

// IA-1s, decision 6: a sentence that names a definition of its own (the one
// the refusal is about) is not told the definition the node was written in.
bool graphRefusalSentenceNamesDefinition(std::string_view code) {
    for (const auto &candidate : GraphLoopRefusalTexts)
        if (candidate.code == code)
            return candidate.sentence.find("{definition}") != std::string_view::npos;
    return false;
}

// FO-1s, decision 1 (engine FO-1): why `second` must run before `first`. The
// engine names the reason of the first step from `second` on the shortest
// path it found; `via` is the rest of that path, `first` at its end.
std::string frameNodeOrderWhy(const nlohmann::json &fields) {
    const auto first = graphRefusalFieldText(fields, "first");
    const auto second = graphRefusalFieldText(fields, "second");
    std::vector<std::string> via;
    if (const auto found = fields.find("via"); found != fields.end() && found->is_array())
        for (const auto &node : *found)
            if (node.is_string()) via.push_back(node.get<std::string>());
    const auto reason = graphRefusalFieldText(fields, "reason");
    if (reason == "canonical_order") {
        const auto anchor = fields.contains("anchor") ? graphRefusalFieldText(fields, "anchor") : std::string{};
        return "the stages of a frame run in a fixed order" +
               (anchor.empty() ? std::string{} : " (" + anchor + " stands between them)");
    }
    const auto &next = via.empty() ? first : via.front();
    std::string why;
    if (reason == "reads")
        why = next + " reads " + graphRefusalFieldText(fields, "resource") + ", which " + second + " writes";
    else if (reason == "after")
        why = next + " is set to run after " + second;
    else if (reason == "before")
        why = second + " is set to run before " + next;
    else
        why = next + " waits on " + second;
    if (!via.empty()) {
        why += ", and " + first + " waits on " + next;
        if (via.size() > 1) {
            std::string rest;
            for (std::size_t index = 1; index < via.size(); ++index) rest += (rest.empty() ? "" : ", ") + via[index];
            why += " through " + rest;
        }
    }
    return why;
}

// FO-1s, decision 4 (engines FO-1 / EB-1): an image's or a buffer's key the
// engine does not read -- the nearest key it reads (a slip of two letters at
// most), else the keys it reads.
std::string unknownKeyHint(const nlohmann::json &fields) {
    const auto key = graphRefusalFieldText(fields, "key");
    std::vector<std::string> allowed;
    if (const auto found = fields.find("allowed"); found != fields.end() && found->is_array())
        for (const auto &name : *found)
            if (name.is_string()) allowed.push_back(name.get<std::string>());
    if (allowed.empty()) return {};
    const auto distance = [](std::string_view from, std::string_view to) {
        std::vector<std::size_t> row(to.size() + 1);
        for (std::size_t j = 0; j <= to.size(); ++j) row[j] = j;
        for (std::size_t i = 1; i <= from.size(); ++i) {
            std::size_t diagonal = row[0];
            row[0] = i;
            for (std::size_t j = 1; j <= to.size(); ++j) {
                const std::size_t above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (from[i - 1] == to[j - 1] ? 0u : 1u)});
                diagonal = above;
            }
        }
        return row[to.size()];
    };
    const std::string *nearest = nullptr;
    std::size_t best = 3;
    for (const auto &name : allowed)
        if (const auto d = distance(key, name); d < best) {
            best = d;
            nearest = &name;
        }
    if (nearest != nullptr) return "; the nearest it reads is " + *nearest;
    std::string keys;
    for (const auto &name : allowed) keys += (keys.empty() ? "" : ", ") + name;
    return "; it reads " + keys;
}

} // namespace

std::optional<nlohmann::json> graphRefusalFields(const nlohmann::json &error, std::string_view code) {
    if (!error.is_object()) return std::nullopt;
    std::optional<nlohmann::json> fields;
    if (error.value("code", std::string{}) == code) {
        const auto relayed = error.find("error_fields");
        fields = relayed != error.end() && relayed->is_object() ? *relayed : error;
    }
    if (auto quoted = graphFieldsAfterCode(error.value("message", std::string{}), code)) {
        if (!fields) fields = nlohmann::json::object();
        for (auto member = quoted->begin(); member != quoted->end(); ++member)
            (*fields)[member.key()] = member.value();
    }
    return fields;
}

std::string graphLoopRefusalDescription(const nlohmann::json &error, bool name_document) {
    if (!error.is_object()) return {};
    const auto own_code = error.value("code", std::string{});
    const auto message = error.value("message", std::string{});
    const GraphLoopRefusalText *entry = nullptr;
    nlohmann::json fields = nlohmann::json::object();
    for (const auto &candidate : GraphLoopRefusalTexts) {
        if (candidate.code != own_code) continue;
        entry = &candidate;
        if (const auto relayed = error.find("error_fields");
            relayed != error.end() && relayed->is_object())
            fields = *relayed;
        else
            fields = error;
        // A refusal may also quote its fields after its code; what it quotes
        // is its own, and wins over a member beside the code of the same
        // name. (LP-4d2: none of them is called `variant` any more -- that
        // name beside the code is the graph variant, always.)
        if (auto quoted = graphFieldsAfterCode(message, candidate.code))
            for (auto member = quoted->begin(); member != quoted->end(); ++member)
                fields[member.key()] = member.value();
    }
    if (entry == nullptr) {
        for (const auto &candidate : GraphLoopRefusalTexts)
            if (auto quoted = graphFieldsAfterCode(message, candidate.code)) {
                entry = &candidate;
                fields = std::move(*quoted);
                break;
            }
    }
    if (entry == nullptr) return {};
    // GL-3a: a fold decision's subject is an object; it reads as the
    // Decisions section writes it.
    if (const auto subject = fields.find("subject");
        subject != fields.end() && subject->is_object()) {
        auto line = subject->value("node", std::string{"?"});
        if (const auto element = subject->find("element");
            element != subject->end() && element->is_number_integer())
            line += " [" + element->dump() + "]";
        if (const auto port = subject->find("port"); port != subject->end() && port->is_string())
            line += "." + port->get<std::string>();
        fields["subject"] = line;
    }
    // LC-1s: a push-constant range reads as the bytes it covers.
    if (entry->code == "shader.loop_push_overlap@1")
        if (const auto range = fields.find("range"); range != fields.end() && range->is_object()) {
            const auto offset = range->find("offset");
            const auto size = range->find("size");
            if (offset != range->end() && offset->is_number_integer() && size != range->end() &&
                size->is_number_integer()) {
                const auto from = offset->get<std::int64_t>();
                const auto to = from + size->get<std::int64_t>();
                fields["range"] = std::to_string(from) + ".." + std::to_string(to);
            }
        }
    // UX-12a, decision 4: why, in the reason's word -- the values an enum
    // takes, the range a number lies in.
    if (entry->code == "graph.render_node_parameter_invalid@1") {
        const auto reason = graphRefusalFieldText(fields, "reason");
        std::string why = "wrong type";
        if (reason == "enum") {
            why = "one of: " + graphRefusalFieldText(fields, "allowed");
        } else if (reason == "range") {
            const bool low = fields.contains("min");
            const bool high = fields.contains("max");
            const auto min = low ? graphRefusalFieldText(fields, "min") : std::string{};
            const auto max = high ? graphRefusalFieldText(fields, "max") : std::string{};
            why = "range: " + (low && high ? min + ".." + max
                               : low       ? "at least " + min
                                           : "at most " + max);
        }
        fields["why"] = why;
    }
    std::string sentence;
    std::string_view pattern = entry->sentence;
    if (entry->code == "graph.loop_tier_unsupported@1") {
        const auto reason = graphRefusalFieldText(fields, "reason");
        for (const auto &candidate : GraphLoopTierReasonTexts)
            if (candidate.reason == reason) pattern = candidate.sentence;
    }
    if (entry->code == "graph.input_index_invalid@1") {
        const auto reason = graphRefusalFieldText(fields, "reason");
        for (const auto &candidate : GraphInputIndexReasonTexts)
            if (candidate.reason == reason) pattern = candidate.sentence;
    }
    if (entry->code == "graph.render_scalar_layout_invalid@1") {
        const auto reason = graphRefusalFieldText(fields, "reason");
        for (const auto &candidate : GraphScalarLayoutReasonTexts)
            if (candidate.reason == reason) pattern = candidate.sentence;
    }
    // UX-13b, decision 3 (engine CT-5): what the buffer is and must be, and
    // the Fix the Graph's button applies -- or, for a buffer declared outside
    // the graph (no `node`), where to apply it.
    if (entry->code == "graph.render_scalar_target_incompatible@1") {
        const auto side = [&](const char *key) {
            const auto found = fields.find(key);
            if (found == fields.end() || !found->is_object()) return std::string{"?"};
            const auto host = found->find("host_source");
            std::string line = host != found->end() && host->is_string() ? host->get<std::string>()
                                                                         : std::string{"no host_source"};
            if (const auto size = found->find("size"); size != found->end() && size->is_number_integer())
                line += ", " + size->dump() + " bytes";
            return line;
        };
        const auto target = graphRefusalFieldText(fields, "target");
        std::string fix = "Fix: make " + target + " runtime_f32";
        if (const auto expected = fields.find("expected"); expected != fields.end() && expected->is_object())
            if (const auto size = expected->find("size"); size != expected->end() && size->is_number_integer())
                fix += ", " + size->dump() + " bytes";
        fields["actual"] = side("actual");
        fields["expected"] = side("expected");
        if (!fields.contains("node") || !fields.at("node").is_string()) {
            pattern = "A render.scalar writes {target} ({actual}), declared outside the graph, which must be "
                      "{expected} \xE2\x80\x94 {fix} where it is declared";
        }
        fields["fix"] = fix;
    }
    // UX-13b, decision 6 (engine CT-5): refused inside a definition, it is
    // the definition's other user that has no owner; the document and the
    // definition follow in the one sentence.
    if (entry->code == "graph.op_requires_owned_instance@1" && fields.contains("definition") &&
        fields.at("definition").is_string())
        pattern = "{op} runs only in a definition attached to an object; this definition is also used by "
                  "an instance with no owner";
    if (entry->code == "graph.loop_output_reference_unknown@1") {
        const auto reason = graphRefusalFieldText(fields, "reason");
        for (const auto &candidate : GraphLoopOutputReferenceReasonTexts)
            if (candidate.reason == reason) pattern = candidate.sentence;
    }
    // IA-1s, decision 2: what the asset is and what the image is, in the
    // words of what differs.
    if (entry->code == "graph.image_initial_mismatch@1") {
        const auto what = graphRefusalFieldText(fields, "what");
        for (const auto &candidate : GraphImageInitialMismatchTexts)
            if (candidate.what == what) pattern = candidate.sentence;
        const auto side = [&](const char *key) -> std::string {
            const auto found = fields.find(key);
            if (found == fields.end() || !found->is_object()) return "?";
            const auto format = found->find("format");
            const auto format_text = format != found->end() && format->is_string()
                                         ? format->get<std::string>()
                                         : std::string{"?"};
            if (what == "extent") {
                const auto number = [&](const char *axis) {
                    const auto value = found->find(axis);
                    return value != found->end() &&
                                   (value->is_number_integer() || value->is_number_unsigned())
                               ? value->dump()
                               : std::string{"?"};
                };
                return number("width") + "×" + number("height");
            }
            if (what == "color_space")
                return std::string{format_text.find("SRGB") != std::string::npos ? "sRGB"
                                                                                  : "linear"} +
                       " (" + format_text + ")";
            return format_text;
        };
        const auto actual = side("actual");
        const auto expected = side("expected");
        fields["actual"] = actual;
        fields["expected"] = expected;
    }
    // FO-1s, decisions 1 and 4.
    if (entry->code == "frame_node_order.conflicts_dependency@1") fields["why"] = frameNodeOrderWhy(fields);
    if (entry->code == "graph.image_key_unknown@1" || entry->code == "graph.buffer_key_unknown@1")
        fields["hint"] = unknownKeyHint(fields);
    for (std::size_t index = 0; index < pattern.size();) {
        const auto open = pattern.find('{', index);
        if (open == std::string_view::npos) {
            sentence += pattern.substr(index);
            break;
        }
        const auto close = pattern.find('}', open);
        sentence += pattern.substr(index, open - index);
        sentence += graphRefusalFieldText(fields, pattern.substr(open + 1, close - open - 1));
        index = close + 1;
    }
    // IA-1s, decision 6 (engine CT-2): the document the node was written in,
    // and the definition whose body holds it -- unless the sentence names a
    // definition of its own already (the definition the refusal is about).
    std::string where;
    if (name_document) {
        const auto document = graphRefusalSource(fields, "document");
        if (!document.empty()) {
            const auto definition = graphRefusalSentenceNamesDefinition(entry->code)
                                        ? std::string{}
                                        : graphRefusalSource(fields, "definition");
            where = definition.empty() ? "in " + document
                                       : "in definition " + definition + " of " + document;
        }
    }
    return sentence + " (" + (where.empty() ? std::string{} : where + ", ") +
           std::string{entry->code} + ").";
}

std::string graphRefusalSource(const nlohmann::json &fields, std::string_view key) {
    if (!fields.is_object()) return {};
    const auto found = fields.find(std::string{key});
    if (found == fields.end() || !found->is_string()) return {};
    // A document reads as the project names it: relative to its root.
    std::string_view value = found->get_ref<const std::string &>();
    constexpr std::string_view project = "project://";
    if (key == "document" && value.starts_with(project)) value.remove_prefix(project.size());
    return std::string{value};
}

namespace {

// AB-1s, decision 1: bake_audio_analysis's refusals (engine AB-1).
constexpr std::array<GraphLoopRefusalText, 5> AudioAnalysisRefusalTexts{{
    {"audio.analysis_settings_invalid@1", "The analysis cannot take {field} = {value} ({reason})"},
    {"audio.analysis_busy@1", "{out} is being baked already; wait for it to finish"},
    {"audio.analysis_unavailable@1", "This engine cannot bake a sound's analysis ({reason})"},
    {"audio.format_unsupported@1",
     "{asset} is not a sound the analysis reads: its extension and its contents must both say "
     "WAV, or both Ogg Vorbis"},
    {"audio.analysis_failed@1", "{asset} could not be baked into {out}: {message}"},
}};

// UX-14, decision 5 (engine AB-2): the table or its manifest was held open.
constexpr std::string_view AudioAnalysisOutputLockedText =
    "{asset} could not be baked into {out}: the table is in use \xE2\x80\x94 try Bake again";

} // namespace

bool audioAnalysisOutputLocked(const nlohmann::json &error) {
    if (!error.is_object() || error.value("code", std::string{}) != "audio.analysis_failed@1") return false;
    const auto fields = graphRefusalFields(error, "audio.analysis_failed@1");
    return fields && fields->is_object() && fields->value("reason", std::string{}) == "output_locked";
}

std::string audioAnalysisRefusalDescription(const nlohmann::json &error) {
    if (!error.is_object()) return {};
    const auto code = error.value("code", std::string{});
    const auto entry = std::ranges::find(AudioAnalysisRefusalTexts, code, &GraphLoopRefusalText::code);
    if (entry == AudioAnalysisRefusalTexts.end()) return {};
    auto fields = graphRefusalFields(error, entry->code).value_or(nlohmann::json::object());
    // A field the refusal leaves out reads as nothing rather than "?".
    for (const char *key : {"field", "value", "reason", "out", "asset", "message"})
        if (!fields.contains(key)) fields[key] = key == std::string_view{"message"}
                                                     ? error.value("message", std::string{})
                                                     : std::string{};
    if (const auto value = fields.find("value"); value != fields.end() && !value->is_string())
        fields["value"] = value->dump();
    for (const char *key : {"asset", "out"})
        fields[key] = graphRefusalSource(nlohmann::json{{"document", graphRefusalFieldText(fields, key)}},
                                         "document");
    std::string sentence;
    const std::string_view pattern =
        audioAnalysisOutputLocked(error) ? AudioAnalysisOutputLockedText : entry->sentence;
    for (std::size_t index = 0; index < pattern.size();) {
        const auto open = pattern.find('{', index);
        if (open == std::string_view::npos) {
            sentence += pattern.substr(index);
            break;
        }
        const auto close = pattern.find('}', open);
        sentence += pattern.substr(index, open - index);
        sentence += graphRefusalFieldText(fields, pattern.substr(open + 1, close - open - 1));
        index = close + 1;
    }
    return sentence + " (" + std::string{entry->code} + ").";
}

namespace {

// HW-2s / AV-1s: the Assets dock's refusals (engine PJ-1 and AV-1).
constexpr std::array<GraphLoopRefusalText, 10> ProjectSettingsRefusalTexts{{
    {"project.settings_change_unsupported@1",
     "{store} cannot change that way while the engine runs: {reason}"},
    {"external_modification", "project.json changed outside Studio since it was read; try again"},
    {"project.hot_reload_invalid@1", "Reload when saved outside cannot take that path: {reason}"},
    {"project.settings_invalid@1", "{store} cannot be declared that way: {reason}"},
    {"project.settings_transaction_busy@1",
     "project.json is being written by another edit; try again"},
    {"assets_store_unknown@1", "No asset store is named {store}"},
    {"assets_manifest_undeclared@1", "Asset store {store} declares no manifest"},
    {"assets_manifest_write_failed@1", "The manifest of {store} could not be written: {reason}"},
    {"assets_verification_unavailable@1", "This engine does not check asset contents"},
    // FO-1s, decision 4 (engine EB-1): Update manifest while a check runs.
    {"assets_verification_running@1",
     "The verification of {store} is running \xE2\x80\x94 try again once it is done"},
}};

// An Undo the engine turned back because the store was there when it started.
constexpr std::string_view ProjectSettingsUndoNeedsRestartText =
    "{store} was there when the engine started, so it stays declared \xE2\x80\x94 only stores added since then can be undone";

// MT-1s, decision 7: the material editor's refusals (engine MT-1).
constexpr std::array<GraphLoopRefusalText, 16> MaterialRefusalTexts{{
    {"material.value_invalid@1", "{param} cannot take that value: {reason}"},
    {"material.param_unknown@1", "The surface of {path} declares no {kind} {param}"},
    {"material.document_unknown@1", "{path} is not a material document the project lists: {reason}"},
    {"material.document_invalid@1", "{path} would not load: {reason}"},
    {"material.name_unknown@1", "No material is named {name}: {reason}"},
    {"material.name_invalid@1", "{name} cannot be a material name: {reason}"},
    {"material.name_conflict@1", "A material named {name} exists already"},
    {"material.path_invalid@1", "{path} cannot be a material document: {reason}"},
    {"material.path_conflict@1", "{path} is there already"},
    {"material.surface_unknown@1", "The surface {surface} cannot be read: {reason}"},
    {"material.model_unknown@1", "{model} cannot take a material: {reason}"},
    {"material.binding_invalid@1", "The materials of {model} cannot be bound that way: {reason}"},
    {"material.object_unknown@1", "Object {object} cannot take a material of its own: {reason}"},
    {"material.transaction_busy@1", "{path} is being written by another edit; try again"},
    // MT-3s, decision 5 (engine MT-3): {path, material, routing, bound_by:
    // [{model, routing}]}; {models} is bound_by's models, one after another.
    {"material.routing_conflict@1",
     "{material} keeps its routing: {models} draw it through their bindings; bind them to another "
     "material first"},
    {"external_modification", "{path} changed outside Studio since it was read; it was read again, try again"},
}};

std::string fillRefusalPattern(std::string_view pattern, const nlohmann::json &fields) {
    std::string sentence;
    for (std::size_t index = 0; index < pattern.size();) {
        const auto open = pattern.find('{', index);
        if (open == std::string_view::npos) {
            sentence += pattern.substr(index);
            break;
        }
        const auto close = pattern.find('}', open);
        sentence += pattern.substr(index, open - index);
        sentence += graphRefusalFieldText(fields, pattern.substr(open + 1, close - open - 1));
        index = close + 1;
    }
    return sentence;
}

} // namespace

std::string materialRefusalDescription(const nlohmann::json &error) {
    if (!error.is_object()) return {};
    const auto code = error.value("code", std::string{});
    const auto entry = std::ranges::find(MaterialRefusalTexts, code, &GraphLoopRefusalText::code);
    if (entry == MaterialRefusalTexts.end()) return {};
    auto fields = graphRefusalFields(error, entry->code).value_or(nlohmann::json::object());
    if (!fields.is_object()) fields = nlohmann::json::object();
    // What the engine leaves out reads as a word, not a "?".
    if (!fields.contains("path") || !fields["path"].is_string()) fields["path"] = "the material";
    if (!fields.contains("kind") || !fields["kind"].is_string()) fields["kind"] = "value";
    if (!fields.contains("reason")) fields["reason"] = error.value("message", std::string{});
    // MT-3s, decision 5: the models whose bindings route through it.
    if (code == "material.routing_conflict@1") {
        std::string models;
        if (const auto bound_by = fields.find("bound_by"); bound_by != fields.end() && bound_by->is_array())
            for (const auto &entry : *bound_by) {
                const auto model = entry.is_object() ? entry.value("model", std::string{}) : std::string{};
                if (model.empty()) continue;
                models += (models.empty() ? "" : ", ") + model;
            }
        fields["models"] = models.empty() ? std::string{"models"} : models;
        if (!fields.contains("material") || !fields["material"].is_string()) fields["material"] = "The material";
    }
    // MT-2s-b: an object with a name is called by it (engine MT-2b).
    std::string_view pattern = entry->sentence;
    if (code == "material.object_unknown@1" && fields.contains("name") && fields["name"].is_string() &&
        !fields["name"].get_ref<const std::string &>().empty())
        pattern = "{name} cannot take a material of its own: {reason}";
    return fillRefusalPattern(pattern, fields) + " (" + std::string{entry->code} + ").";
}

namespace {

// MI-1s, decision 5: register_model's and unregister_model's refusals
// (engine MI-1). {why} is said from `reason` (ModelRefusalReasonTexts);
// {objects} is in_use's names, one after another.
constexpr std::array<GraphLoopRefusalText, 11> ModelRefusalTexts{{
    {"model.path_invalid@1", "{path} cannot name a model file: {why}"},
    {"model.format_unsupported@1", "{path} is not a .glb, .gltf or .vrm file"},
    {"model.file_missing@1", "{path} is not a file in the project"},
    {"model.path_conflict@1", "{path} is registered already as model {name}"},
    {"model.name_invalid@1", "{name} cannot be a model name: {why}"},
    {"model.name_conflict@1", "A model named {name} exists already"},
    {"model.load_failed@1", "{path} does not load: {message}"},
    {"model.unknown@1", "No model is named {name} in this project"},
    {"model.in_use@1", "{name} is used by {count} objects: {objects}"},
    {"model.transaction_busy@1", "The project's asset list is being written by another edit; try again"},
    {"external_modification", "{path} changed outside Studio since it was read; try again"},
}};

struct ModelRefusalReasonText {
    std::string_view reason;
    std::string_view words;
};
constexpr std::array<ModelRefusalReasonText, 6> ModelRefusalReasonTexts{{
    {"fragment", "it names a part of a file (#)"},
    {"dot_segment", "it holds a . or .. folder"},
    {"backslash", "it uses \\ where / goes"},
    {"absolute", "it is not inside the project"},
    {"empty", "it is empty"},
    {"engine_reference", "engine:// names the engine's own shapes"},
}};

} // namespace

std::string modelRefusalDescription(const nlohmann::json &error) {
    if (!error.is_object()) return {};
    const auto code = error.value("code", std::string{});
    const auto entry = std::ranges::find(ModelRefusalTexts, code, &GraphLoopRefusalText::code);
    if (entry == ModelRefusalTexts.end()) return {};
    auto fields = graphRefusalFields(error, entry->code).value_or(nlohmann::json::object());
    if (!fields.is_object()) fields = nlohmann::json::object();
    const auto reason = fields.value("reason", std::string{});
    std::string_view pattern = entry->sentence;
    // An engine shape is not taken off (unregister_model).
    if (code == "model.name_invalid@1" && reason == "engine_primitive")
        pattern = "{name} is one of the engine's own shapes, which stay";
    const auto words = std::ranges::find(ModelRefusalReasonTexts, reason, &ModelRefusalReasonText::reason);
    fields["why"] = words != ModelRefusalReasonTexts.end() ? std::string{words->words}
                    : reason.empty()                       ? std::string{"the engine does not take it"}
                                                           : reason;
    if (!fields.contains("path") || !fields["path"].is_string()) fields["path"] = "The file";
    if (!fields.contains("name") || !fields["name"].is_string()) fields["name"] = "The model";
    // load_failed: the engine's own words, as it wrote them.
    if (!fields.contains("message") || !fields["message"].is_string())
        fields["message"] = error.value("message", std::string{});
    if (code == "model.in_use@1") {
        std::string objects;
        std::size_t listed = 0;
        if (const auto names = fields.find("objects"); names != fields.end() && names->is_array())
            for (const auto &name : *names) {
                if (!name.is_string()) continue;
                objects += (objects.empty() ? "" : ", ") + name.get<std::string>();
                ++listed;
            }
        const auto count = fields.value("count", static_cast<std::uint64_t>(listed));
        // The engine names the first 8.
        if (count > listed) objects += objects.empty() ? "\xE2\x80\xA6" : ", \xE2\x80\xA6";
        fields["count"] = count;
        fields["objects"] = objects;
        if (count == 1) pattern = "{name} is used by 1 object: {objects}";
    }
    return fillRefusalPattern(pattern, fields) + " (" + std::string{entry->code} + ").";
}

std::string modelUnknownEditDescription(const nlohmann::json &error) {
    if (!error.is_object()) return {};
    const auto payload = error.find("payload");
    if (payload == error.end() || !payload->is_object() ||
        payload->value("reason", std::string{}) != "model_unknown")
        return {};
    auto model = payload->value("model", std::string{});
    if (model.empty()) model = "?";
    return "Model " + model +
           " is not registered in this project. Choose its file with Import\xE2\x80\xA6 beside the "
           "Inspector's model field, or Add Object > Model from file\xE2\x80\xA6";
}

bool inspectorListShown(std::string_view component, std::string_view list_path) {
    return !(component == "simplemodelview" && list_path == "/materials");
}

std::string projectSettingsRefusalDescription(const nlohmann::json &error, bool undo) {
    if (!error.is_object()) return {};
    const auto code = error.value("code", std::string{});
    const auto entry = std::ranges::find(ProjectSettingsRefusalTexts, code, &GraphLoopRefusalText::code);
    if (entry == ProjectSettingsRefusalTexts.end()) return {};
    auto fields = graphRefusalFields(error, entry->code).value_or(nlohmann::json::object());
    if (!fields.is_object()) fields = nlohmann::json::object();
    // A refusal about a key and not a store names the key instead.
    if (!fields.contains("store") || !fields["store"].is_string())
        fields["store"] = fields.value("key", std::string{"asset_stores"});
    if (!fields.contains("reason")) fields["reason"] = error.value("message", std::string{});
    const std::string_view pattern =
        undo && code == "project.settings_change_unsupported@1" ? ProjectSettingsUndoNeedsRestartText
                                                                : entry->sentence;
    std::string sentence;
    for (std::size_t index = 0; index < pattern.size();) {
        const auto open = pattern.find('{', index);
        if (open == std::string_view::npos) {
            sentence += pattern.substr(index);
            break;
        }
        const auto close = pattern.find('}', open);
        sentence += pattern.substr(index, open - index);
        sentence += graphRefusalFieldText(fields, pattern.substr(open + 1, close - open - 1));
        index = close + 1;
    }
    return sentence + " (" + std::string{entry->code} + ").";
}

std::string projectRenderDocument(const std::filesystem::path &project_root) {
    if (project_root.empty()) return {};
    std::ifstream input{project_root / "project.json", std::ios::binary};
    if (!input) return {};
    const auto project = nlohmann::json::parse(input, nullptr, false);
    if (!project.is_object()) return {};
    const auto basic = project.find("basic_config");
    if (basic == project.end() || !basic->is_object()) return {};
    const auto document = basic->find("rendering_config_json");
    return document != basic->end() && document->is_string() ? document->get<std::string>()
                                                              : std::string{};
}

std::string engineStoppedSentence(std::string_view fatal_line, std::string_view render_document) {
    constexpr std::string_view marker = "Pelican fatal error";
    const auto at = fatal_line.find(marker);
    if (at == std::string_view::npos) return {};
    auto message = fatal_line.substr(at + marker.size());
    while (!message.empty() && (message.front() == ' ' || message.front() == ':'))
        message.remove_prefix(1);
    // The first `<namespace>.<name>@<version>` the message names.
    std::string code;
    for (std::size_t index = 0; index < message.size() && code.empty(); ++index) {
        const auto version = message.find('@', index);
        if (version == std::string_view::npos) break;
        std::size_t begin = version;
        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(message[begin - 1])) ||
                             message[begin - 1] == '_' || message[begin - 1] == '.'))
            --begin;
        std::size_t end = version + 1;
        while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end]))) ++end;
        const auto candidate = message.substr(begin, end - begin);
        if (end > version + 1 && candidate.find('.') != std::string_view::npos &&
            candidate.front() != '.')
            code = std::string{candidate};
        index = version;
    }
    if (code.empty()) return {};
    // The document is said once, after the sentence, not inside it too.
    auto sentence = knownRefusalDescription(code, message, false);
    if (sentence.empty()) return {};
    // "<sentence> (<code>)." -- the sentence, without its full stop.
    while (!sentence.empty() && (sentence.back() == '.' || sentence.back() == ' ')) sentence.pop_back();
    // IA-1s, decision 6 (engine CT-2): the refusal names the document its
    // node was written in, and the definition whose body holds it.
    std::string file;
    std::string definition;
    if (const auto fields = graphFieldsAfterCode(message, code)) {
        file = graphRefusalSource(*fields, "document");
        if (!file.empty() && !graphRefusalSentenceNamesDefinition(code))
            definition = graphRefusalSource(*fields, "definition");
    }
    // Before CT-2: the last path ending in .json the message names.
    if (file.empty())
        for (std::size_t end = message.find(".json"); end != std::string_view::npos;
             end = message.find(".json", end + 1)) {
            std::size_t begin = end;
            while (begin > 0 && message[begin - 1] != ' ' && message[begin - 1] != '\'' &&
                   message[begin - 1] != '"' && message[begin - 1] != '(')
                --begin;
            file = std::string{message.substr(begin, end + 5 - begin)};
        }
    // Studio's guess, only when the engine named none.
    if (file.empty()) file = std::string{render_document};
    if (file.empty()) file = "the project's render documents";
    if (!definition.empty()) file = "definition " + definition + " of " + file;
    return "The engine stopped: " + sentence + " in " + file +
           ". Fix the file (or Reload from disk) to start it again.";
}

RenderPipelineReading readRenderPipeline(const nlohmann::json &reply) {
    RenderPipelineReading reading;
    if (!reply.is_object()) return reading;
    const auto pipeline = reply.find("render_pipeline");
    if (pipeline == reply.end() || !pipeline->is_object()) return reading;
    const auto state = pipeline->value("state", std::string{});
    if (state == "fallback") reading.state = RenderPipelineReading::State::Fallback;
    else if (state == "ready") reading.state = RenderPipelineReading::State::Ready;
    else return reading;
    if (const auto root = pipeline->find("root"); root != pipeline->end() && root->is_string())
        reading.root = root->get<std::string>();
    if (const auto refusal = pipeline->find("refusal"); refusal != pipeline->end() && refusal->is_object())
        reading.refusal = *refusal;
    else
        reading.refusal = nlohmann::json::object();
    return reading;
}

std::string renderFallbackDocument(const RenderPipelineReading &reading) {
    if (reading.state != RenderPipelineReading::State::Fallback) return {};
    if (auto document = graphRefusalSource(reading.refusal, "document"); !document.empty())
        return document;
    // The root as the engine names it: project-relative, maybe project://.
    return graphRefusalSource(nlohmann::json{{"document", reading.root}}, "document");
}

std::string renderFallbackNode(const RenderPipelineReading &reading) {
    if (reading.state != RenderPipelineReading::State::Fallback) return {};
    return graphRefusalSource(reading.refusal, "node");
}

std::string renderFallbackSentence(const RenderPipelineReading &reading) {
    if (reading.state != RenderPipelineReading::State::Fallback) return {};
    const auto &refusal = reading.refusal;
    const auto code = refusal.value("code", std::string{});
    const auto message = refusal.value("message", std::string{});
    // The fields beside the code first (the refusal is a rejection's
    // `error`), then whatever table knows the code from its message. The
    // document is said once, after the sentence.
    // BS-1s, decision 7: a document that is not JSON names itself.
    if (auto syntax = renderDocumentSyntaxDescription(refusal); !syntax.empty()) {
        while (!syntax.empty() && (syntax.back() == '.' || syntax.back() == ' ')) syntax.pop_back();
        return "Showing a placeholder: the rendering setup was refused \xE2\x80\x94 " + syntax +
               ". Fix it, then Reload from disk.";
    }
    auto sentence = graphLoopRefusalDescription(refusal, false);
    if (sentence.empty()) sentence = knownRefusalDescription(code, message, false);
    // PB-1s (engine PB-1): a start whose materials find no pass for their
    // route falls back too; the Frame Plan's notice for it says which.
    if (sentence.empty() && code == "material_route_unserved@1")
        if (auto route = materialRouteUnservedNotice(refusal); !route.empty()) {
            while (!route.empty() && (route.back() == '.' || route.back() == ' ')) route.pop_back();
            sentence = route + " (" + code + ")";
        }
    if (sentence.empty()) {
        // A refusal Studio has no sentence for reads as the engine wrote it,
        // with its code when the message does not start with it.
        sentence = message.empty() ? code
                   : code.empty() || message.starts_with(code) ? message
                                                              : message + " (" + code + ")";
        if (const auto newline = sentence.find('\n'); newline != std::string::npos)
            sentence.erase(newline);
    }
    while (!sentence.empty() && (sentence.back() == '.' || sentence.back() == ' ')) sentence.pop_back();
    auto file = renderFallbackDocument(reading);
    if (file.empty()) file = "the project's render documents";
    if (const auto definition = graphRefusalSource(refusal, "definition");
        !definition.empty() && !graphRefusalSentenceNamesDefinition(code))
        file = "definition " + definition + " of " + file;
    return "Showing a placeholder: the rendering setup was refused \xE2\x80\x94 " + sentence + " in " +
           file + ". Fix it, then Reload from disk.";
}

std::string renderDocumentSyntaxDescription(const nlohmann::json &error) {
    if (!error.is_object()) return {};
    constexpr std::string_view code = "render_document_syntax@1";
    const auto fields = graphRefusalFields(error, code);
    if (!fields || !fields->is_object() || (!fields->contains("document") && !fields->contains("reason")))
        return {};
    auto document = graphRefusalSource(*fields, "document");
    if (document.empty()) document = "A render document";
    std::string where;
    const auto line = fields->find("line");
    const auto column = fields->find("column");
    if (line != fields->end() && line->is_number_integer() && column != fields->end() &&
        column->is_number_integer())
        where = " at line " + std::to_string(line->get<std::int64_t>()) + ", column " +
                std::to_string(column->get<std::int64_t>());
    auto reason = fields->value("reason", std::string{});
    std::string sentence = document + " is not valid JSON" + where;
    if (!reason.empty()) sentence += ": " + reason;
    return sentence + " (" + std::string{code} + ").";
}

std::string renderRestartFeature(const nlohmann::json &error) {
    // PB-1s, decision 6 (engine PB-1): the refusal has a code of its own and
    // names the feature; its words are not read.
    const auto fields = graphRefusalFields(error, RenderFeatureRequiresRestartCode);
    if (!fields || !fields->is_object()) return {};
    auto feature = graphRefusalSource(*fields, "feature");
    return feature.empty() ? std::string{"the feature"} : feature;
}

std::string graphRefusalVariant(const nlohmann::json &error) {
    if (!error.is_object()) return {};
    const auto member = error.find("variant");
    if (member == error.end() || !member->is_string()) return {};
    // LP-4d2, decision 8: `variant` beside the code is the graph variant
    // whatever the refusal quotes -- the one refusal that had a field of that
    // name (graph.loop_variant_missing@1) calls it material_variant now.
    return member->get<std::string>();
}

std::string componentUnavailableTag(std::string_view code) {
    const auto *entry = findUnavailableComponentText(code);
    return entry ? std::string{entry->tag} : std::string{"unavailable"};
}

std::string sceneEditRefusalSentence(const SceneEditOutcome &outcome, std::string_view object,
                                     std::string_view target) {
    const std::string name = object.empty() ? std::string{"the object"} : std::string{object};
    const std::string parent = target.empty() ? std::string{"its new parent"} : std::string{target};
    const char *verb = "change";
    switch (outcome.kind) {
    case SceneEditKind::Spawn: verb = "add"; break;
    case SceneEditKind::Duplicate: verb = "duplicate"; break;
    case SceneEditKind::Destroy: verb = "delete"; break;
    case SceneEditKind::Reparent: verb = "move"; break;
    case SceneEditKind::Undo: verb = "undo"; break;
    case SceneEditKind::Redo: verb = "redo"; break;
    }
    const std::string &code = outcome.code;
    const std::string cannot = std::string{"Cannot "} + verb + " " + name;
    const std::string under = "Cannot move " + name + " under " + parent;
    // Q-6's table: a build without the module says so the way Add component
    // does.
    if (!componentUnavailableSentence(code).empty())
        return cannot + ": " + componentUnavailableDescription(code) + ".";
    if (code == "closure_unresolvable") return name + " is no longer in the scene.";
    if (code == "editor_preview_unsupported@1")
        return cannot + " while a live preview is open; finish the preview first.";
    if (code == "stale_revision")
        return "The scene changed before " + name + " could be " +
               (outcome.kind == SceneEditKind::Destroy ? std::string{"deleted"}
                                                       : std::string{verb} + "d") +
               "; try again.";
    if (outcome.kind == SceneEditKind::Reparent) {
        if (code == "cycle_detected")
            return under + ": " + parent + " is inside " + name + ".";
        if (code == "parent_not_found")
            return under + ": " + parent + " is not a named object in this scene.";
        if (code == "schema_violation")
            return name + " has no name, so it cannot be moved to another parent.";
        if (code == "zero_scale")
            return under + ": a zero scale on the way leaves no place to keep it.";
        if (code == "non_finite_transform")
            return under + ": its transform there would not be a finite number.";
        if (code == "trs_unrepresentable")
            return under + " where it is: that place is not a position, rotation and "
                           "scale under it. Hold Alt to keep its own transform instead.";
        if (code == "preserve_missing")
            return under + ": the move did not say which transform to keep.";
    }
    if (outcome.kind == SceneEditKind::Duplicate && code == "schema_violation")
        return cannot + ": the copy has no place in the scene's order.";
    const std::string detail = code.empty() ? outcome.message
                               : outcome.message.empty() ? code
                                                         : code + " - " + outcome.message;
    return cannot + (detail.empty() ? std::string{"."} : ": " + detail);
}

std::vector<ComponentAddRow> componentAddRows(const Json &catalog) {
    std::vector<ComponentAddRow> rows;
    const auto candidates = catalog.find("candidates");
    if (candidates == catalog.end() || !candidates->is_array()) return rows;
    for (std::size_t index = 0; index < candidates->size(); ++index) {
        const auto &candidate = candidates->at(index);
        ComponentAddRow row;
        row.candidate = index;
        row.key = candidate.value("key", std::string{});
        row.label = candidate.contains("behavior_type")
            ? "Behavior: " + candidate.at("behavior_type").get<std::string>()
            : candidate.value("name", std::string{});
        const auto reason = candidate.value("unavailable_reason", std::string{});
        if (!candidate.value("available", false) && isComponentUnavailableCode(reason)) {
            row.addable = false;
            row.label += " \xE2\x80\x94 " + componentUnavailableTag(reason);
            row.blocked = componentUnavailableDescription(reason);
        }
        rows.push_back(std::move(row));
    }
    std::stable_partition(rows.begin(), rows.end(), [](const auto &row) { return row.addable; });
    return rows;
}

std::vector<std::string> modelAssetIds(const Json &list_assets_result) {
    return assetIdsOfKind(list_assets_result, "model");
}

std::vector<std::string> assetIdsOfKind(const Json &list_assets_result, std::string_view kind) {
    const auto &assets = requiredField(list_assets_result, "assets", "list_assets result");
    if (!assets.is_array()) throw std::runtime_error("list_assets assets must be an array");
    std::vector<std::string> ids;
    for (const auto &asset : assets) {
        if (!asset.is_object() || asset.value("kind", std::string{}) != kind) continue;
        const auto id = asset.find("id");
        if (id != asset.end() && id->is_string() && !id->get_ref<const std::string &>().empty())
            ids.push_back(id->get<std::string>());
    }
    return ids;
}

struct InspectorModel::Impl {
    enum class RequestPurpose : std::uint8_t {
        OpenSession,
        HistoryLabels,
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
        // PM-3b: Take from game reads the object's runtime Transform first.
        TakeFromGame,
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
        // OL-1: set for an edit the Outliner follows to its end.
        std::optional<SceneEditKind> scene_edit;
        std::uint64_t scene_edit_subject = 0;
        // PM-3b: marked Keep in Play changes as soon as it commits.
        bool keep = false;
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
        std::optional<SceneEditKind> scene_edit;
        std::uint64_t scene_edit_subject = 0;
        // Q-16: for a watch request, how many of this session's own commits
        // had been seen when it went out.
        std::uint64_t own_commit_generation = 0;
        // PM-3b: an edit to mark Keep when it commits (Take from game).
        bool keep = false;
        // UX-13c: a SceneTree request that went out as scene_outline.
        bool scene_outline = false;
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
    // UX-10: the texture ids of the same answer, for sprite_view.texture.
    std::optional<std::vector<std::string>> texture_assets;
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
    // Q-16: counts the commits this session made itself (edits, undo, redo,
    // preview commits, saves). A watch answer to a request sent before the latest of
    // them says nothing about anyone else.
    std::uint64_t own_commit_generation = 0;
    // UX-13c: the selected object's id is confirmed with scene_outline; an
    // engine that does not know it (-32601) is asked with scene_tree for the
    // rest of its session.
    bool scene_outline_unknown = false;
    bool refresh_in_flight = false;
    bool refresh_pending = false;
    bool refresh_pending_sync = false;
    bool watch_request_in_flight = false;
    bool watch_poll_pending = false;
    bool watch_sync_pending = false;

    // Q-7: the one Add object waiting for the edit in flight.
    struct QueuedSpawn {
        std::string scene_id;
        Json object;
        std::string name;
    };
    std::optional<QueuedSpawn> queued_spawn;
    // OL-1: the last spawn, Outliner edit, undo or redo to finish.
    std::optional<SceneEditOutcome> last_scene_edit;
    std::uint64_t scene_edit_serial = 0;

    void recordSceneEdit(std::optional<SceneEditKind> kind, std::uint64_t subject,
                         const Json *result, std::string failure = {}) {
        if (!kind) return;
        SceneEditOutcome outcome{.serial = ++scene_edit_serial,
                                 .kind = *kind,
                                 .subject_id = subject};
        if (result != nullptr) {
            outcome.committed = result->value("status", std::string{}) == "committed";
            if (outcome.committed) {
                const auto created = result->find("created_objects");
                if (created != result->end() && created->is_array())
                    outcome.created_objects = *created;
            } else {
                outcome.code = resultErrorCode(*result);
                outcome.message = resultErrorMessage(*result);
                // MI-1s: an unregistered model is said in words alone.
                if (const auto error = result->find("error");
                    error != result->end() && !modelUnknownEditDescription(*error).empty())
                    outcome.code.clear();
            }
        } else {
            outcome.message = std::move(failure);
        }
        last_scene_edit = std::move(outcome);
    }

    void setNotice(InspectorNoticeKind kind, std::string message) {
        notice = {.kind = kind, .message = std::move(message)};
        notice_transaction.clear();
    }

    // PM-3b: what this session committed, for the Play changes panel to name
    // (takeCommittedEdits), and the transaction the notice now reports.
    std::vector<CommittedEdit> committed_edits;
    std::string notice_transaction;
    // Labels follow committed transactions only; refusals leave both tops alone.
    std::vector<std::string> undo_labels;
    std::vector<std::string> redo_labels;
    std::map<std::string, std::string> transaction_labels;

    void refreshHistoryLabels() {
        undo_labels.clear();
        redo_labels.clear();
        if (connected && actor_id)
            queue("query_journal", Json::object(), {.purpose = RequestPurpose::HistoryLabels});
    }

    void readHistoryLabels(const Json &result) {
        // A newer local commit must not be replaced by an older journal read.
        const auto revision = unsignedInteger(result.at("scene_revision"), "query_journal scene_revision");
        if (revision < current_scene_revision.value_or(0)) {
            refreshHistoryLabels();
            return;
        }
        observeSceneRevision(revision);
        std::vector<std::string> undo, redo;
        for (const auto &record : result.at("records")) {
            if (!actor_id || record.at("actor_id") != *actor_id) continue;
            const auto operation = record.value("operation_kind", std::string{});
            if (operation == "undo") {
                if (!undo.empty()) { redo.push_back(undo.back()); undo.pop_back(); }
            } else if (operation == "redo") {
                if (!redo.empty()) { undo.push_back(redo.back()); redo.pop_back(); }
            } else {
                const auto label = transaction_labels.find(record.at("transaction_id").get<std::string>());
                undo.push_back(label != transaction_labels.end() ? label->second :
                               operation == "play_keep" ? "Keep game changes" : "Scene edit");
                redo.clear();
            }
        }
        undo_labels = std::move(undo);
        redo_labels = std::move(redo);
    }

    void noteCommitted(const Json &result, const std::string &description, bool keep) {
        const auto transaction = result.find("transaction_id");
        if (transaction == result.end() || !transaction->is_string()) return;
        CommittedEdit edit{.transaction_id = transaction->get<std::string>(),
                           .label = description,
                           .keep = keep};
        const auto operation = result.value("operation", std::string{});
        if (operation == "undo") edit.kind = CommittedEditKind::undo;
        if (operation == "redo") edit.kind = CommittedEditKind::redo;
        if (const auto source = result.find("source_transaction_id");
            source != result.end() && source->is_string())
            edit.source_transaction_id = source->get<std::string>();
        if (edit.kind == CommittedEditKind::undo) {
            if (!undo_labels.empty()) {
                redo_labels.push_back(undo_labels.back());
                undo_labels.pop_back();
            }
        } else if (edit.kind == CommittedEditKind::redo) {
            if (!redo_labels.empty()) {
                undo_labels.push_back(redo_labels.back());
                redo_labels.pop_back();
            }
        } else {
            undo_labels.push_back(description);
            redo_labels.clear();
        }
        if (edit.kind == CommittedEditKind::edit)
            transaction_labels[edit.transaction_id] = description;
        notice_transaction = edit.transaction_id;
        committed_edits.push_back(std::move(edit));
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
        // PM-3b: Stop put back the scene the file holds, under a new revision.
        if (adopt_after_revision && revision > *adopt_after_revision) {
            saved_scene_revision = revision;
            adopt_after_revision.reset();
        }
    }

    // PM-3b: the first revision past this one is the scene Stop put back.
    std::optional<std::uint64_t> adopt_after_revision;

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

    bool busy() const noexcept {
        return preview.has_value() || editPending() || saveInFlight();
    }

    bool spawnReady() const noexcept {
        return connected && actor_id && current_scene_revision;
    }

    void sendSpawn(const std::string &scene_id, Json object,
                   const std::string &name) {
        invalidateRayOccurrences();
        // No coalesce key: a new object is its own undo step, never folded
        // into the property edit before it.
        queue("edit",
              Json{{"actor_id", *actor_id},
                   {"base_revision", *current_scene_revision},
                   {"operations", Json::array({Json{{"op", "spawn"},
                                                    {"scene_id", scene_id},
                                                    {"object", std::move(object)}}})}},
              {.purpose = RequestPurpose::EditSubmit,
               .selection_generation = selection_generation,
               .description = "Add " + name,
               .scene_edit = SceneEditKind::Spawn});
    }

    // The waiting spawn goes out once nothing is in flight and no field is
    // held, on the revision the finished edit left behind.
    void flushQueuedSpawn() {
        if (!queued_spawn || !spawnReady() || busy() || refreshBlocked()) return;
        auto spawn = std::move(*queued_spawn);
        queued_spawn.reset();
        sendSpawn(spawn.scene_id, std::move(spawn.object), spawn.name);
        setNotice(InspectorNoticeKind::Information, "Adding " + spawn.name + "...");
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
               .selection_generation = selection_generation,
               .own_commit_generation = own_commit_generation});
    }

    // Q-16: this session's own commit (an edit, undo, redo, preview commit
    // or save) reached `revision`. The watch token moves with it, so the
    // next watch answer that reports that revision is not taken for another
    // client's edit: reading it again would switch Undo off for the length of
    // a refresh it does not need.
    void noteOwnCommit(std::uint64_t revision,
                       std::optional<std::uint64_t> preview_epoch = std::nullopt) {
        ++own_commit_generation;
        if (!observed_watch) return;
        observed_watch->scene_revision =
            std::max(observed_watch->scene_revision, revision);
        if (preview_epoch)
            observed_watch->preview_epoch =
                std::max(observed_watch->preview_epoch, *preview_epoch);
    }

    void noteOwnCommit(const Json &result) {
        std::optional<std::uint64_t> epoch;
        // A preview commit names the epoch it left behind.
        if (const auto found = result.find("preview_epoch"); found != result.end())
            epoch = unsignedInteger(*found, "preview_epoch");
        noteOwnCommit(unsignedInteger(result.at("committed_revision"),
                                      "edit committed_revision"),
                      epoch);
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
        queue(scene_outline_unknown ? "scene_tree" : "scene_outline",
              Json{{"scene_id", selection->scene_id}},
              {.purpose = RequestPurpose::SceneTree,
               .selection_generation = selection_generation,
               .synchronize_watch = sync,
               .scene_outline = !scene_outline_unknown});
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
        recordSceneEdit(pending.scene_edit, pending.scene_edit_subject, &result);
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
            noteCommitted(result, pending.description, pending.keep);
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
        if (!connected || !actor_id || !current_scene_revision || editPending() || preview ||
            !editing_widgets.empty()) {
            setNotice(InspectorNoticeKind::Error,
                      "Undo and redo require an idle inspector session.");
            return;
        }
        const std::string description = redo ? "Redo" : "Undo";
        queue(redo ? "redo" : "undo",
              Json{{"actor_id", *actor_id},
                   {"base_revision", *current_scene_revision}},
              {.purpose = RequestPurpose::EditSubmit,
               .selection_generation = selection_generation,
               .description = description,
               .scene_edit = redo ? SceneEditKind::Redo : SceneEditKind::Undo});
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
        // OL-1: the same object, moved in the declaration order by an edit
        // (a duplicate before it, an undo). The panel still shows it; only
        // where it stands has changed.
        if (selection && next && selection->scene_id == next->scene_id &&
            selection->authoring_object_id != 0 &&
            selection->authoring_object_id == next->authoring_object_id) {
            selection = std::move(next);
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
            const InspectorWidgetDescriptor *committed_widget = findWidget(field_key);
            const std::string committed_label =
                "Edit " + (committed_widget != nullptr
                               ? committed_widget->component_slot + "." +
                                     committed_widget->field_name
                               : field_key);
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
            noteCommitted(result, committed_label, false);
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
        // UX-13c: scene_outline, or scene_tree from an engine without it;
        // both list the ids and declaration indices read here.
        const std::string method = state.scene_outline ? "scene_outline" : "scene_tree";
        const std::string scene_id =
            requiredString(result, "scene_id", method + " result");
        if (scene_id != selection->scene_id) {
            throw std::runtime_error(method + " returned a different scene_id");
        }
        const Json &objects = requiredField(result, "objects", method + " result");
        if (!objects.is_array()) {
            throw std::runtime_error(method + " objects must be an array");
        }

        // OL-1: a selection that knows its object's authoring id is that
        // object wherever it stands now; one that does not is the object at
        // its declaration index.
        std::optional<std::uint64_t> authoring_object_id;
        for (const Json &object : objects) {
            const auto id = unsignedInteger(
                requiredField(object, "authoring_object_id", method + " object"),
                method + " authoring_object_id", false);
            if (selection->authoring_object_id != 0) {
                if (id == selection->authoring_object_id) {
                    authoring_object_id = id;
                    break;
                }
                continue;
            }
            const auto index = sizeValue(
                requiredField(object, "declaration_index", method + " object"),
                method + " declaration_index");
            if (index == selection->declaration_index) {
                authoring_object_id = id;
                break;
            }
        }
        if (!authoring_object_id) {
            throw std::runtime_error(
                method + (selection->authoring_object_id != 0
                              ? " does not contain the selected object"
                              : " does not contain the selected declaration_index"));
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
                return (component.name == "simplemodelview" || component.name == "sprite_view") &&
                       !component.generated;
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
        texture_assets.reset();
        ++model_assets_revision;
    }

    void handleWatch(const RequestState &state, const Json &result) {
        if (state.selection_generation != selection_generation) {
            return;
        }
        watch_request_in_flight = false;
        const WatchToken current = parseWatchToken(result);
        if (state.own_commit_generation != own_commit_generation) {
            // Q-16: sent before one of this session's own commits. Whether
            // the engine answered before or after that commit, the difference
            // is ours, and the commit already asked for its own refresh. The
            // next poll judges what anyone else did.
        } else if (state.purpose == RequestPurpose::WatchSync) {
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
            .scene_edit = state.scene_edit,
            .scene_edit_subject = state.scene_edit_subject,
            .keep = state.keep,
        };
        if (result.value("status", std::string{}) == "accepted") {
            pending.ticket = requiredString(result, "ticket", "edit response");
            pending_edits.push_back(std::move(pending));
            setNotice(InspectorNoticeKind::Information,
                      state.description +
                          " accepted; waiting for the player frame boundary.");
            return;
        }
        // MT-2s: set_object_material answers changed: false, and no ticket,
        // when the object already carries what was asked.
        if (!result.contains("status") && result.value("changed", true) == false) {
            setNotice(InspectorNoticeKind::Information,
                      state.description + ": nothing to change.");
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
        // Saving stages the document again under a new revision: that is
        // this session's own doing as well.
        noteOwnCommit(revision);
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
            if (state.purpose == RequestPurpose::EditSubmit ||
                state.purpose == RequestPurpose::EditPoll ||
                state.purpose == RequestPurpose::PreviewPoll) {
                noteOwnCommit(result);
            }
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
            state.purpose != RequestPurpose::HistoryLabels &&
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
        case RequestPurpose::HistoryLabels:
            readHistoryLabels(result);
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
            texture_assets = assetIdsOfKind(result, "texture");
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
        case RequestPurpose::TakeFromGame:
            handleTakeFromGame(result);
            break;
        }
    }

    // PM-3b (design_play_mode 7, 12.3). The object's Transform as the game has
    // it now (get_components' runtime_json: the ECS values), written back as
    // one edit of the authored pos, rotation and scale, marked Keep so it
    // stays after Stop.
    void handleTakeFromGame(const Json &result) {
        if (!snapshot || !actor_id) return;
        const auto components = result.find("components");
        if (components == result.end() || !components->is_array()) {
            setNotice(InspectorNoticeKind::Error, "Take from game: the engine sent no components.");
            return;
        }
        const Json *local = nullptr;
        std::string slot;
        for (const auto &component : *components) {
            if (!component.is_object() || component.value("name", std::string{}) != "transform")
                continue;
            const auto runtime = component.find("runtime_json");
            if (runtime == component.end() || !runtime->is_object()) continue;
            const auto trs = runtime->find("local_trs");
            if (trs == runtime->end() || !trs->is_object()) continue;
            local = &*trs;
            slot = component.value("name", std::string{});
            break;
        }
        if (local == nullptr) {
            setNotice(InspectorNoticeKind::Error,
                      "Take from game: the game has no Transform for this object right now.");
            return;
        }
        Json operations = Json::array();
        for (const char *key : {"pos", "rotation", "scale"}) {
            const auto value = local->find(key);
            if (value == local->end()) continue;
            operations.push_back({{"op", "set_component_value"},
                                  {"object_id", snapshot->authoring_object_id},
                                  {"component_slot", slot},
                                  {"field_path", std::string{"/"} + key},
                                  {"value", *value}});
        }
        if (operations.empty()) {
            setNotice(InspectorNoticeKind::Error,
                      "Take from game: the game's Transform has no position, rotation or scale.");
            return;
        }
        const auto revision = result.find("scene_revision");
        queue("edit",
              Json{{"actor_id", *actor_id},
                   {"base_revision", revision != result.end() && revision->is_number_unsigned()
                                         ? revision->get<std::uint64_t>()
                                         : snapshot->scene_revision},
                   {"operations", std::move(operations)}},
              {.purpose = RequestPurpose::EditSubmit,
               .selection_generation = selection_generation,
               .description = "Take Transform from game",
               .component_slot = slot,
               .keep = true});
        setNotice(InspectorNoticeKind::Information, "Taking the Transform from the game...");
    }

    void handleFailure(const RequestState &state, std::string message,
                       bool unknown_method = false, bool refused = false) {
        if (state.purpose == RequestPurpose::SceneTree && unknown_method &&
            state.scene_outline) {
            // UX-13c, decision 5: an engine from before scene_outline. Ask it
            // with scene_tree from now on, and ask again now.
            scene_outline_unknown = true;
            refresh_in_flight = false;
            if (state.selection_generation == selection_generation)
                requestRefresh(state.synchronize_watch);
            return;
        }
        if (state.purpose != RequestPurpose::OpenSession &&
            state.purpose != RequestPurpose::Save &&
            state.purpose != RequestPurpose::EditSubmit &&
            state.purpose != RequestPurpose::EditPoll &&
            state.purpose != RequestPurpose::ModelAssets &&
            state.purpose != RequestPurpose::HistoryLabels &&
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
                recordSceneEdit(pending->scene_edit, pending->scene_edit_subject, nullptr,
                                message);
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
            // A refusal wrote nothing; a lost reply may have written.
            if (!refused) scene_revision_uncertain = true;
            recordSceneEdit(state.scene_edit, state.scene_edit_subject, nullptr, message);
            requestRefresh(true);
            break;
        case RequestPurpose::Save:
            last_save_succeeded = false;
            ++save_completion;
            break;
        case RequestPurpose::ComponentAddCandidates:
            component_add_candidates.reset();
            break;
        case RequestPurpose::HistoryLabels:
            return;
        case RequestPurpose::ModelAssets:
            // The primitives are listed without it; nothing to report.
            return;
        case RequestPurpose::RayOccurrences:
            if (state.ray_catalog_epoch != ray_catalog_epoch) return;
            ray_occurrences.reset();
            break;
        case RequestPurpose::TakeFromGame:
            message = "Take from game: " + message;
            break;
        }
        setNotice(InspectorNoticeKind::Error, std::move(message));
    }
};

InspectorModel::InspectorModel() : impl_{std::make_unique<Impl>()} {}

InspectorModel::~InspectorModel() = default;

void InspectorModel::startSession() {
    impl_->abandonSessionOperations();
    impl_->undo_labels.clear();
    impl_->redo_labels.clear();
    impl_->transaction_labels.clear();
    impl_->first_session_revision = true;
    impl_->connected = true;
    impl_->scene_outline_unknown = false;
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
    // The engine the spawn was meant for is gone; the next one reads the
    // scene from disk and the Outliner starts over.
    impl_->queued_spawn.reset();
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
    impl_->undo_labels.clear();
    impl_->redo_labels.clear();
    impl_->transaction_labels.clear();
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
    // The engine the spawn was meant for is gone; the next one reads the
    // scene from disk and the Outliner starts over.
    impl_->queued_spawn.reset();
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
    impl_->flushQueuedSpawn();
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
                                       std::string message,
                                       bool unknown_method, bool refused) {
    const auto request = impl_->requests.find(request_id);
    if (request == impl_->requests.end()) {
        return;
    }
    Impl::RequestState state = std::move(request->second);
    impl_->requests.erase(request);
    if (message.empty()) {
        message = "Inspector RPC failed without an error message.";
    }
    impl_->handleFailure(state, std::move(message), unknown_method, refused);
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
    // Q-7: an edit that has been let go (committing or aborting) is over for
    // the panel. Beginning it again would hold the Inspector for an edit
    // nobody is making: the view's focus moving on as a field is disabled
    // did exactly that, and nothing ever finished it.
    const bool preview_is_ending =
        impl_->preview && impl_->preview->field_key == key &&
        (impl_->preview->release_requested || impl_->preview->abort_requested);
    if (impl_->editPending() || another_widget_is_editing ||
        another_preview_is_active || preview_is_ending) {
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

const std::optional<std::vector<std::string>> &InspectorModel::textureAssets() const noexcept {
    return impl_->texture_assets;
}

void InspectorModel::refreshModelAssets() { impl_->requestModelAssets(); }

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

SpawnOutcome InspectorModel::spawnObject(const std::string &scene_id, Json object) {
    if (!impl_->spawnReady()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "The engine editor session is not ready.");
        return SpawnOutcome::Refused;
    }
    // One waiting before this one goes first: this spawn's base revision is
    // the one that spawn leaves.
    impl_->flushQueuedSpawn();
    const std::string name = object.value("name", std::string{});
    if (busy() || refreshBlocked()) {
        if (impl_->queued_spawn) {
            impl_->setNotice(InspectorNoticeKind::Error,
                             impl_->queued_spawn->name +
                                 " is already waiting to be added; add another once it is in.");
            return SpawnOutcome::Refused;
        }
        impl_->queued_spawn = Impl::QueuedSpawn{
            .scene_id = scene_id, .object = std::move(object), .name = name};
        impl_->setNotice(InspectorNoticeKind::Information,
                         "Adding " + name + " after the current edit.");
        return SpawnOutcome::Queued;
    }
    impl_->sendSpawn(scene_id, std::move(object), name);
    return SpawnOutcome::Sent;
}

std::optional<std::string> InspectorModel::queuedSpawnName() const {
    if (!impl_->queued_spawn) return std::nullopt;
    return impl_->queued_spawn->name;
}

SpawnOutcome InspectorModel::submitSceneEdit(SceneEditKind kind, std::uint64_t subject_id,
                                             Json operation, std::string description) {
    if (!impl_->spawnReady()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "The engine editor session is not ready.");
        return SpawnOutcome::Refused;
    }
    impl_->flushQueuedSpawn();
    if (busy() || refreshBlocked()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "Finish the edit in progress first, then " + description + " again.");
        return SpawnOutcome::Refused;
    }
    impl_->invalidateRayOccurrences();
    // No coalesce key: each is its own undo step.
    impl_->queue("edit",
                 Json{{"actor_id", *impl_->actor_id},
                      {"base_revision", *impl_->current_scene_revision},
                      {"operations", Json::array({std::move(operation)})}},
                 {.purpose = Impl::RequestPurpose::EditSubmit,
                  .selection_generation = impl_->selection_generation,
                  .description = std::move(description),
                  .scene_edit = kind,
                  .scene_edit_subject = subject_id});
    return SpawnOutcome::Sent;
}

SpawnOutcome InspectorModel::submitObjectMaterial(Json params, std::string description) {
    if (!impl_->spawnReady() || !params.is_object()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "The engine editor session is not ready.");
        return SpawnOutcome::Refused;
    }
    impl_->flushQueuedSpawn();
    if (busy() || refreshBlocked()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "Finish the edit in progress first, then choose the material again.");
        return SpawnOutcome::Refused;
    }
    params["actor_id"] = *impl_->actor_id;
    params["base_scene_revision"] = *impl_->current_scene_revision;
    // Its own undo step, as the Outliner's edits are.
    impl_->queue("set_object_material", std::move(params),
                 {.purpose = Impl::RequestPurpose::EditSubmit,
                  .selection_generation = impl_->selection_generation,
                  .description = std::move(description)});
    return SpawnOutcome::Sent;
}

const std::optional<SceneEditOutcome> &InspectorModel::lastSceneEdit() const noexcept {
    return impl_->last_scene_edit;
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

void InspectorModel::adoptRestoredRevisionAsSaved() {
    impl_->adopt_after_revision = impl_->current_scene_revision.value_or(0);
}

void InspectorModel::resetSceneSaveState() {
    impl_->current_scene_revision.reset();
    impl_->saved_scene_revision.reset();
    impl_->adopt_after_revision.reset();
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

std::vector<CommittedEdit> InspectorModel::takeCommittedEdits() {
    return std::exchange(impl_->committed_edits, {});
}

const std::string &InspectorModel::noticeTransaction() const noexcept {
    return impl_->notice_transaction;
}

bool InspectorModel::canTakeTransformFromGame() const noexcept {
    if (!canEdit() || busy() || impl_->refresh_in_flight || refreshBlocked()) return false;
    return std::any_of(impl_->snapshot->components.begin(), impl_->snapshot->components.end(),
                       [](const InspectorComponentSnapshot &component) {
                           return component.name == "transform" && component.editable &&
                                  !component.pending;
                       });
}

bool InspectorModel::takeTransformFromGame() {
    if (!canTakeTransformFromGame()) {
        impl_->setNotice(InspectorNoticeKind::Error,
                         "Take from game needs a selected object with a Transform and no edit "
                         "in flight.");
        return false;
    }
    impl_->queue("get_components",
                 Json{{"scene_id", impl_->snapshot->scene_id},
                      {"authoring_object_id", impl_->snapshot->authoring_object_id}},
                 {.purpose = Impl::RequestPurpose::TakeFromGame,
                  .selection_generation = impl_->selection_generation});
    impl_->setNotice(InspectorNoticeKind::Information, "Reading the game's Transform...");
    return true;
}

std::uint64_t InspectorModel::contentRevision() const noexcept {
    return impl_->content_revision;
}

bool InspectorModel::refreshBlocked() const noexcept {
    return impl_->refreshBlocked();
}

bool InspectorModel::busy() const noexcept {
    return impl_->busy();
}

bool InspectorModel::canEdit() const noexcept {
    return impl_->connected && impl_->actor_id && impl_->snapshot.has_value();
}

bool InspectorModel::canUndoRedo() const noexcept {
    return impl_->connected && impl_->actor_id && impl_->current_scene_revision &&
           !busy() && !impl_->refresh_in_flight;
}

void InspectorModel::refreshHistoryLabels() { impl_->refreshHistoryLabels(); }

std::string InspectorModel::undoLabel() const {
    return impl_->undo_labels.empty() ? std::string{} : impl_->undo_labels.back();
}

std::string InspectorModel::redoLabel() const {
    return impl_->redo_labels.empty() ? std::string{} : impl_->redo_labels.back();
}

bool InspectorModel::canSave() const noexcept {
    return impl_->connected && impl_->actor_id && !busy() &&
           !refreshBlocked() &&
           !impl_->refresh_in_flight;
}

} // namespace PelicanStudio
