#include "document.hpp"

#include "../userpublic/details/event/registerer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>

namespace Pelican::ui {
namespace {

using Json = nlohmann::json;

void error(std::vector<UiError> &out, UiErrorCode code, std::string path, std::string message) {
    out.push_back({UiPhase::Parse, code, std::move(path), std::move(message)});
}

bool closed(const Json &object, std::initializer_list<std::string_view> allowed, const std::string &path,
            std::vector<UiError> &errors) {
    if (!object.is_object()) {
        error(errors, UiErrorCode::TypeMismatch, path, "expected object");
        return false;
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
            error(errors, UiErrorCode::UnknownField, path + "/" + it.key(), "unknown property");
    }
    return true;
}

std::optional<std::int32_t> i32(const Json &value) {
    if (!value.is_number_integer()) return std::nullopt;
    const auto v = value.get<std::int64_t>();
    if (v < INT32_MIN || v > INT32_MAX) return std::nullopt;
    return static_cast<std::int32_t>(v);
}

RectI rect(const Json &value, const std::string &path, std::vector<UiError> &errors) {
    if (!value.is_array() || value.size() != 4) {
        error(errors, UiErrorCode::TypeMismatch, path, "expected four integers");
        return {};
    }
    RectI result;
    std::int32_t *parts[] = {&result.left, &result.top, &result.right, &result.bottom};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto parsed = i32(value[i]);
        if (!parsed) error(errors, UiErrorCode::TypeMismatch, path + "/" + std::to_string(i), "expected int32");
        else *parts[i] = *parsed;
    }
    return result;
}

AxisSpec axis(const Json &j, const std::string &path, std::vector<UiError> &errors) {
    AxisSpec result;
    if (!closed(j, {"mode", "value", "min", "max", "weight"}, path, errors)) return result;
    const auto mode = j.value("mode", "fixed");
    if (mode == "fixed") result.mode = SizeMode::Fixed;
    else if (mode == "content") result.mode = SizeMode::Content;
    else if (mode == "fill") result.mode = SizeMode::Fill;
    else error(errors, UiErrorCode::TypeMismatch, path + "/mode", "unknown size mode");
    for (const auto &[name, target] : std::initializer_list<std::pair<const char *, std::int32_t *>>{
             {"value", &result.value}, {"min", &result.min}, {"max", &result.max}}) {
        if (j.contains(name)) {
            if (auto parsed = i32(j.at(name))) *target = *parsed;
            else error(errors, UiErrorCode::TypeMismatch, path + "/" + name, "expected int32");
        }
    }
    if (j.contains("weight")) {
        const auto w = i32(j.at("weight"));
        if (!w || *w <= 0) error(errors, UiErrorCode::RangeViolation, path + "/weight", "weight must be positive");
        else result.weight = static_cast<std::uint32_t>(*w);
    }
    if (result.min > result.max) error(errors, UiErrorCode::RangeViolation, path, "min exceeds max");
    return result;
}

double decimal(const Json &j, const std::string &path, std::vector<UiError> &errors) {
    if (!j.is_number()) {
        error(errors, UiErrorCode::TypeMismatch, path, "expected number");
        return 0;
    }
    const auto token = j.dump();
    double value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || !std::isfinite(value)) {
        error(errors, UiErrorCode::TypeMismatch, path, "number is not finite binary64");
        return 0;
    }
    return value;
}

const PayloadFieldSchema *findField(const EventPayloadSchema &schema, std::string_view name) {
    const auto it = std::find_if(schema.fields.begin(), schema.fields.end(), [&](const auto &f) { return f.name == name; });
    return it == schema.fields.end() ? nullptr : &*it;
}

bool typeMatches(PayloadFieldType type, EmitSourceKind source, const Json *value, std::string_view widget_path) {
    if (source == EmitSourceKind::StableId) return type == PayloadFieldType::String;
    if (source == EmitSourceKind::DragDeltaUi) return type == PayloadFieldType::Vec2;
    if (source == EmitSourceKind::WidgetValue) {
        if (widget_path == "text") return type == PayloadFieldType::String;
        if (widget_path == "checked") return type == PayloadFieldType::I8 || type == PayloadFieldType::U8;
        return type == PayloadFieldType::F32 || type == PayloadFieldType::F64 ||
               (type >= PayloadFieldType::I8 && type <= PayloadFieldType::U64);
    }
    if (value == nullptr) return false;
    if (type == PayloadFieldType::String) return value->is_string();
    if (type == PayloadFieldType::Vec2) return value->is_array() && value->size() == 2;
    if (type == PayloadFieldType::Vec3) return value->is_array() && value->size() == 3;
    if (type == PayloadFieldType::Vec4 || type == PayloadFieldType::Quat) return value->is_array() && value->size() == 4;
    if (type >= PayloadFieldType::I8 && type <= PayloadFieldType::U64) return value->is_number_integer();
    return value->is_number();
}

bool staticInRange(const PayloadFieldSchema &field, const Json *value) {
    if (value == nullptr || field.range.index() == 0) return true;
    if (const auto *range = std::get_if<std::pair<std::int64_t, std::int64_t>>(&field.range)) {
        if (!value->is_number_integer()) return true;
        const auto v = value->get<std::int64_t>();
        return v >= range->first && v <= range->second;
    }
    if (const auto *range = std::get_if<std::pair<std::uint64_t, std::uint64_t>>(&field.range)) {
        if (!value->is_number_unsigned() && !value->is_number_integer()) return true;
        if (value->is_number_integer() && value->get<std::int64_t>() < 0) return false;
        const auto v = value->get<std::uint64_t>();
        return v >= range->first && v <= range->second;
    }
    const auto *range = std::get_if<std::pair<double, double>>(&field.range);
    if (range == nullptr) return true;
    auto in = [&](double v) { return std::isfinite(v) && v >= range->first && v <= range->second; };
    if (value->is_array()) return std::ranges::all_of(*value, [&](const auto &v) { return v.is_number() && in(v.get<double>()); });
    return value->is_number() && in(value->get<double>());
}

void parseEmit(const Json &emit, DocumentNode &node, const std::string &path, const EventLookup &lookup,
               std::vector<UiError> &errors) {
    if (!emit.is_object()) {
        error(errors, UiErrorCode::TypeMismatch, path, "emit must be object");
        return;
    }
    for (auto trigger_it = emit.begin(); trigger_it != emit.end(); ++trigger_it) {
        const auto trigger_path = path + "/" + trigger_it.key();
        const auto &binding_json = trigger_it.value();
        if (!closed(binding_json, {"event", "fields"}, trigger_path, errors)) continue;
        if (!binding_json.contains("event") || !binding_json.at("event").is_string()) {
            error(errors, UiErrorCode::RequiredMissing, trigger_path + "/event", "event name is required");
            continue;
        }
        EmitBinding binding{.trigger = trigger_it.key(), .event = binding_json.at("event").get<std::string>()};
        const auto schema = lookup(binding.event);
        if (schema.state == EventSchemaLookup::State::UnknownEvent) {
            error(errors, UiErrorCode::UnknownEvent, trigger_path + "/event", "unknown event '" + binding.event + "'");
            continue;
        }
        const bool has_fields = binding_json.contains("fields");
        if ((schema.state == EventSchemaLookup::State::Opaque || schema.state == EventSchemaLookup::State::Payloadless) &&
            has_fields && !binding_json.at("fields").empty()) {
            error(errors, UiErrorCode::NoPayloadEvent, trigger_path + "/fields", "event has no bindable payload");
            continue;
        }
        if (schema.state == EventSchemaLookup::State::Typed && !has_fields) {
            error(errors, UiErrorCode::RequiredMissing, trigger_path + "/fields", "typed event fields are required");
            continue;
        }
        if (!has_fields) { node.emits.push_back(std::move(binding)); continue; }
        const auto &fields = binding_json.at("fields");
        if (!fields.is_array()) {
            error(errors, UiErrorCode::TypeMismatch, trigger_path + "/fields", "fields must be array");
            continue;
        }
        std::set<std::string> names;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto fp = trigger_path + "/fields/" + std::to_string(i);
            const auto &field_json = fields[i];
            if (!closed(field_json, {"name", "from", "value"}, fp, errors)) continue;
            if (!field_json.contains("name") || !field_json.at("name").is_string() ||
                !field_json.contains("from") || !field_json.at("from").is_string()) {
                error(errors, UiErrorCode::RequiredMissing, fp, "name and from are required");
                continue;
            }
            EmitField field{.name = field_json.at("name").get<std::string>()};
            if (!names.insert(field.name).second) {
                error(errors, UiErrorCode::UnknownField, fp + "/name", "duplicate emit field");
                continue;
            }
            const auto from = field_json.at("from").get<std::string>();
            const Json *static_value = nullptr;
            if (from == "static") {
                field.source = EmitSourceKind::Static;
                if (!field_json.contains("value")) error(errors, UiErrorCode::RequiredMissing, fp + "/value", "static value required");
                else { static_value = &field_json.at("value"); field.static_json = static_value->dump(); }
            } else if (from == "stable_id") field.source = EmitSourceKind::StableId;
            else if (from == "drag_delta_ui") field.source = EmitSourceKind::DragDeltaUi;
            else if (from.starts_with("widget_value(") && from.ends_with(')')) {
                field.source = EmitSourceKind::WidgetValue;
                field.widget_path = from.substr(13, from.size() - 14);
                if (field.widget_path != "value" && field.widget_path != "checked" && field.widget_path != "text")
                    error(errors, UiErrorCode::UnknownWidgetValuePath, fp + "/from", "unknown widget value path");
            } else {
                error(errors, UiErrorCode::UnknownSource, fp + "/from", "unknown emit source");
                continue;
            }
            if (schema.schema != nullptr) {
                const auto *expected = findField(*schema.schema, field.name);
                if (expected == nullptr) error(errors, UiErrorCode::UnknownField, fp + "/name", "field not in event schema");
                else if (!typeMatches(expected->type, field.source, static_value, field.widget_path))
                    error(errors, UiErrorCode::TypeMismatch, fp, "source type does not match event field");
                else if (field.source == EmitSourceKind::Static && !staticInRange(*expected, static_value))
                    error(errors, UiErrorCode::RangeViolation, fp + "/value", "static value is outside event field range");
            }
            binding.fields.push_back(std::move(field));
        }
        if (schema.schema != nullptr) for (const auto &expected : schema.schema->fields) {
            if (!names.contains(std::string{expected.name}))
                error(errors, UiErrorCode::RequiredMissing, trigger_path + "/fields", "required event field missing: " + std::string{expected.name});
        }
        node.emits.push_back(std::move(binding));
    }
}

DocumentNode parseNode(const Json &j, std::string parent, std::int32_t &decl, std::set<std::string> &ids,
                       const std::string &json_path, const EventLookup &lookup, std::vector<UiError> &errors) {
    DocumentNode node;
    if (!closed(j, {"id", "type", "layout", "intrinsic_size", "visibility", "overflow", "enabled",
                    "hit_testable", "layer", "emit", "children", "sprite", "sampler", "color",
                    "nine_patch"}, json_path, errors)) return node;
    if (!j.contains("id") || !j.at("id").is_string()) error(errors, UiErrorCode::RequiredMissing, json_path + "/id", "id required");
    else node.stable_id = j.at("id").get<std::string>();
    node.path = parent.empty() ? node.stable_id : parent + "/" + node.stable_id;
    if (!ids.insert(node.path).second) error(errors, UiErrorCode::DuplicateStableId, json_path + "/id", "duplicate stable id");
    if (!j.contains("type") || !j.at("type").is_string()) error(errors, UiErrorCode::RequiredMissing, json_path + "/type", "type required");
    else node.type = j.at("type").get<std::string>();
    static const std::set<std::string> types{"panel", "image", "label", "button", "gauge", "stack"};
    if (!types.contains(node.type)) error(errors, UiErrorCode::UnknownWidgetType, json_path + "/type", "unknown widget type");
    if (node.type == "image" && !j.contains("color")) node.color = {255, 255, 255, 255};
    if (j.contains("sprite")) {
        if (!j.at("sprite").is_string() || j.at("sprite").get_ref<const std::string &>().empty())
            error(errors, UiErrorCode::TypeMismatch, json_path + "/sprite", "sprite must be a non-empty #sprite reference");
        else {
            node.sprite = j.at("sprite").get<std::string>();
            if (node.sprite.find("#sprite/") == std::string::npos)
                error(errors, UiErrorCode::TypeMismatch, json_path + "/sprite", "expected an atlas #sprite/name reference");
        }
    }
    const auto sampler = j.value("sampler", "linear");
    if (sampler == "nearest") node.sampler = Sampler::Nearest;
    else if (sampler == "linear") node.sampler = Sampler::Linear;
    else error(errors, UiErrorCode::TypeMismatch, json_path + "/sampler", "sampler must be nearest or linear");
    if (j.contains("color")) {
        const auto &color = j.at("color");
        if (!color.is_array() || color.size() != 4) {
            error(errors, UiErrorCode::TypeMismatch, json_path + "/color", "color must be four linear RGBA8 integers");
        } else {
            for (std::size_t i = 0; i < 4; ++i) {
                const auto value = i32(color[i]);
                if (!value || *value < 0 || *value > 255)
                    error(errors, UiErrorCode::RangeViolation, json_path + "/color/" + std::to_string(i), "RGBA8 component outside [0,255]");
                else node.color[i] = static_cast<std::uint8_t>(*value);
            }
        }
    }
    if (j.contains("nine_patch")) node.nine_patch = rect(j.at("nine_patch"), json_path + "/nine_patch", errors);
    if (!node.sprite.empty() && node.type != "image" && node.type != "panel")
        error(errors, UiErrorCode::TypeMismatch, json_path + "/sprite", "only image and panel widgets draw sprites in v1");
    if (node.type == "image" && node.sprite.empty())
        error(errors, UiErrorCode::RequiredMissing, json_path + "/sprite", "image widget requires a sprite");
    if (node.nine_patch && node.type != "panel")
        error(errors, UiErrorCode::TypeMismatch, json_path + "/nine_patch", "nine_patch is only valid on panel widgets");
    node.decl_seq = decl++;
    if (j.contains("layer")) { if (auto v = i32(j.at("layer"))) node.layer = *v; else error(errors, UiErrorCode::TypeMismatch, json_path + "/layer", "expected int32"); }
    node.enabled = j.value("enabled", true);
    node.hit_testable = j.value("hit_testable", true);
    const auto visibility = j.value("visibility", "visible");
    if (visibility == "hidden") node.visibility = Visibility::Hidden;
    else if (visibility == "collapsed") node.visibility = Visibility::Collapsed;
    else if (visibility != "visible") error(errors, UiErrorCode::TypeMismatch, json_path + "/visibility", "unknown visibility");
    node.overflow_clip = j.value("overflow", "visible") == "clip";
    if (j.contains("intrinsic_size")) {
        const auto &v = j.at("intrinsic_size");
        if (!v.is_array() || v.size() != 2 || !i32(v[0]) || !i32(v[1])) error(errors, UiErrorCode::TypeMismatch, json_path + "/intrinsic_size", "expected two int32");
        else node.intrinsic_size = {*i32(v[0]), *i32(v[1])};
    }
    if (j.contains("layout")) {
        const auto &l = j.at("layout");
        if (closed(l, {"x", "y", "anchor_min", "anchor_max", "offsets", "stack", "gap", "padding", "align", "justify"}, json_path + "/layout", errors)) {
            if (l.contains("x")) node.layout.x = axis(l.at("x"), json_path + "/layout/x", errors);
            if (l.contains("y")) node.layout.y = axis(l.at("y"), json_path + "/layout/y", errors);
            auto anchors = [&](const char *name, double &x, double &y) {
                if (!l.contains(name)) return;
                const auto &v = l.at(name);
                if (!v.is_array() || v.size() != 2) error(errors, UiErrorCode::TypeMismatch, json_path + "/layout/" + name, "expected two anchors");
                else { x = decimal(v[0], json_path + "/layout/" + name + "/0", errors); y = decimal(v[1], json_path + "/layout/" + name + "/1", errors); }
                if (x < 0 || x > 1 || y < 0 || y > 1) error(errors, UiErrorCode::RangeViolation, json_path + "/layout/" + name, "anchor outside [0,1]");
            };
            anchors("anchor_min", node.layout.anchor_min_x, node.layout.anchor_min_y);
            anchors("anchor_max", node.layout.anchor_max_x, node.layout.anchor_max_y);
            if (l.contains("offsets")) node.layout.offsets = rect(l.at("offsets"), json_path + "/layout/offsets", errors);
            if (l.contains("padding")) node.layout.padding = rect(l.at("padding"), json_path + "/layout/padding", errors);
            const auto stack = l.value("stack", "none");
            if (stack == "horizontal") node.layout.stack = StackDirection::Horizontal;
            else if (stack == "vertical") node.layout.stack = StackDirection::Vertical;
            else if (stack != "none") error(errors, UiErrorCode::TypeMismatch, json_path + "/layout/stack", "unknown stack direction");
            if (l.contains("gap")) { if (auto v = i32(l.at("gap"))) node.layout.gap = *v; else error(errors, UiErrorCode::TypeMismatch, json_path + "/layout/gap", "expected int32"); }
            const auto align = l.value("align", "start");
            if (align == "center") node.layout.align = Align::Center;
            else if (align == "end") node.layout.align = Align::End;
            else if (align == "stretch") node.layout.align = Align::Stretch;
            else if (align != "start") error(errors, UiErrorCode::TypeMismatch, json_path + "/layout/align", "unknown align");
            const auto justify = l.value("justify", "start");
            if (justify == "center") node.layout.justify = Justify::Center;
            else if (justify == "end") node.layout.justify = Justify::End;
            else if (justify == "space_between") node.layout.justify = Justify::SpaceBetween;
            else if (justify != "start") error(errors, UiErrorCode::TypeMismatch, json_path + "/layout/justify", "unknown justify");
        }
    }
    if (j.contains("emit")) parseEmit(j.at("emit"), node, json_path + "/emit", lookup, errors);
    if (j.contains("children")) {
        if (!j.at("children").is_array()) error(errors, UiErrorCode::TypeMismatch, json_path + "/children", "children must be array");
        else for (std::size_t i = 0; i < j.at("children").size(); ++i)
            node.children.push_back(parseNode(j.at("children")[i], node.path, decl, ids, json_path + "/children/" + std::to_string(i), lookup, errors));
    }
    return node;
}

} // namespace

DocumentParseResult parseUiDocument(const nlohmann::json &json, EventLookup lookup) {
    DocumentParseResult result;
    if (!lookup) lookup = [](std::string_view name) { return Pelican::internal::findEventSchema(name); };
    if (!closed(json, {"schema", "version", "key", "revision", "direction", "root"}, "", result.errors)) return result;
    if (json.value("schema", "") != "pelican.ui" || json.value("version", 0) != 1)
        error(result.errors, UiErrorCode::TypeMismatch, "/schema", "expected pelican.ui version 1");
    UiDocument document;
    if (!json.contains("key") || !json.at("key").is_string() || json.at("key").get<std::string>().empty())
        error(result.errors, UiErrorCode::RequiredMissing, "/key", "document key required");
    else document.key = json.at("key").get<std::string>();
    document.revision = json.value("revision", "");
    document.direction = json.value("direction", "ltr");
    if (document.direction != "ltr") error(result.errors, UiErrorCode::UnsupportedControl, "/direction", "v1 is LTR only");
    if (!json.contains("root")) error(result.errors, UiErrorCode::RequiredMissing, "/root", "root required");
    else {
        std::set<std::string> ids;
        std::int32_t decl = 0;
        document.root = parseNode(json.at("root"), {}, decl, ids, "/root", lookup, result.errors);
    }
    if (result.errors.empty()) result.document = std::move(document);
    return result;
}

} // namespace Pelican::ui
