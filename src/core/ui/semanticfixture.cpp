#include "semanticfixture.hpp"

#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace Pelican::ui {
namespace {
using Json = nlohmann::json;

void issue(std::vector<FixtureIssue> &out, std::string clause, std::string path, std::string message) {
    out.push_back({std::move(clause), std::move(path), std::move(message)});
}

bool objectClosed(const Json &j, std::initializer_list<std::string_view> required,
                  std::initializer_list<std::string_view> optional, std::string_view path,
                  std::vector<FixtureIssue> &out) {
    if (!j.is_object()) { issue(out, "schema.type", std::string(path), "expected object"); return false; }
    for (auto key : required) if (!j.contains(key)) issue(out, "schema.required", std::string(path) + "/" + std::string(key), "required property missing");
    for (auto it = j.begin(); it != j.end(); ++it) {
        const bool known = std::find(required.begin(), required.end(), it.key()) != required.end() ||
                           std::find(optional.begin(), optional.end(), it.key()) != optional.end();
        if (!known) issue(out, "schema.additionalProperties", std::string(path) + "/" + it.key(), "unknown property");
    }
    return true;
}

bool integer(const Json &j, std::int64_t min, std::int64_t max) {
    if (!j.is_number_integer() && !j.is_number_unsigned()) return false;
    if (j.is_number_unsigned()) return j.get<std::uint64_t>() <= static_cast<std::uint64_t>(max);
    const auto value = j.get<std::int64_t>();
    return value >= min && value <= max;
}

bool enumValue(const Json &j, std::initializer_list<std::string_view> values) {
    if (!j.is_string()) return false;
    return std::find(values.begin(), values.end(), j.get_ref<const std::string &>()) != values.end();
}

void checkRect(const Json &j, std::string path, std::vector<FixtureIssue> &out, std::size_t count = 4) {
    if (!j.is_array() || j.size() != count) { issue(out, "schema.rect", std::move(path), "wrong array shape"); return; }
    for (std::size_t i = 0; i < count; ++i) if (!integer(j[i], INT32_MIN, INT32_MAX))
        issue(out, "schema.integer", path + "/" + std::to_string(i), "expected int32 integer token");
}

const std::regex revision_re{"^sha256:[0-9a-f]{64}$"};
const std::regex widget_re{"^[a-z0-9_]+(/[a-z0-9_]+)*$"};
const std::regex name_re{"^[a-z][a-z0-9_]*$"};
const std::regex texture_re{"^(atlas:[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)*(/[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)*)*/page:[0-9]+|white)$"};
const std::regex pointer_re{"^(/([^/~]|~[01])*)*$"};
const std::regex consumed_re{"^(mouse:(left|right|middle|move|wheel)|key:(a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r|s|t|u|v|w|x|y|z|0|1|2|3|4|5|6|7|8|9|f1|f2|f3|f4|f5|f6|f7|f8|f9|f10|f11|f12|escape|tab|enter|space|backspace|delete|insert|home|end|page_up|page_down|up|down|left|right|shift|ctrl|alt|super)|touch:(0|[1-9][0-9]?)|pad:(a|b|x|y|lb|rb|lt|rt|start|select|ls|rs|dpad_up|dpad_down|dpad_left|dpad_right|ls_x|ls_y|rs_x|rs_y))$"};

void uniqueStrings(const Json &array, const std::string &path, std::vector<FixtureIssue> &out) {
    if (!array.is_array()) { issue(out, "schema.type", path, "expected array"); return; }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (!array[i].is_string()) issue(out, "schema.type", path + "/" + std::to_string(i), "expected string");
        else if (!seen.insert(array[i].get<std::string>()).second) issue(out, "schema.uniqueItems", path, "duplicate item");
    }
}

void validateTrace(const Json &trace, const std::string &path, std::vector<FixtureIssue> &out) {
    if (!trace.is_object()) { issue(out, "schema.type", path, "expected object"); return; }
    const auto kind = trace.value("kind", "");
    if (kind == "pointer_down" || kind == "pointer_up")
        objectClosed(trace, {"event_seq", "kind", "pointer_id", "button", "position_ui"}, {"target", "consumed", "effects"}, path, out);
    else if (kind == "pointer_move")
        objectClosed(trace, {"event_seq", "kind", "pointer_id", "position_ui"}, {"target", "consumed", "effects"}, path, out);
    else if (kind == "pointer_cancel")
        objectClosed(trace, {"event_seq", "kind", "pointer_id"}, {"consumed", "effects"}, path, out);
    else { issue(out, "schema.enum", path + "/kind", "unknown input kind"); return; }
    if (!trace.contains("event_seq") || !integer(trace["event_seq"], 0, INT64_C(9007199254740991))) issue(out, "schema.range", path + "/event_seq", "invalid event_seq");
    if (!trace.contains("pointer_id") || !integer(trace["pointer_id"], 0, 255)) issue(out, "schema.range", path + "/pointer_id", "invalid pointer id");
    if (trace.contains("button") && !enumValue(trace["button"], {"left", "right", "middle"})) issue(out, "schema.enum", path + "/button", "invalid button");
    if (trace.contains("position_ui")) checkRect(trace["position_ui"], path + "/position_ui", out, 2);
    if (trace.contains("target") && (!trace["target"].is_string() || !std::regex_match(trace["target"].get<std::string>(), widget_re))) issue(out, "schema.pattern", path + "/target", "invalid widget path");
    if (trace.contains("effects")) {
        uniqueStrings(trace["effects"], path + "/effects", out);
        for (std::size_t i = 0; trace["effects"].is_array() && i < trace["effects"].size(); ++i)
            if (!enumValue(trace["effects"][i], {"capture", "release_capture", "click", "drag_start", "drag", "cancel", "hover_enter", "hover_exit"})) issue(out, "schema.enum", path + "/effects/" + std::to_string(i), "invalid effect");
    }
    if (trace.contains("consumed")) {
        uniqueStrings(trace["consumed"], path + "/consumed", out);
        for (std::size_t i = 0; trace["consumed"].is_array() && i < trace["consumed"].size(); ++i)
            if (!trace["consumed"][i].is_string() || !std::regex_match(trace["consumed"][i].get<std::string>(), consumed_re)) issue(out, "schema.pattern", path + "/consumed/" + std::to_string(i), "invalid consumed control");
    }
}

void validateLifecycle(const Json &j, const std::string &path, std::vector<FixtureIssue> &out) {
    if (!j.is_object()) { issue(out, "schema.type", path, "expected object"); return; }
    const auto kind = j.value("kind", "");
    if (kind == "controller_init" || kind == "controller_deinit") objectClosed(j, {"kind", "controller", "widget"}, {}, path, out);
    else if (kind == "capture_cancel") objectClosed(j, {"kind", "widget", "reason"}, {}, path, out);
    else if (kind == "document_swap") objectClosed(j, {"kind", "from_revision", "to_revision", "dropped_commands"}, {}, path, out);
    else if (kind == "command_dropped") objectClosed(j, {"kind", "count", "reason"}, {}, path, out);
    else { issue(out, "schema.enum", path + "/kind", "unknown lifecycle kind"); return; }
    if (j.contains("widget") && (!j["widget"].is_string() || !std::regex_match(j["widget"].get<std::string>(), widget_re))) issue(out, "schema.pattern", path + "/widget", "invalid widget path");
    if (kind == "capture_cancel" && !enumValue(j["reason"], {"hide", "disable", "remove", "reload", "scene_unload", "focus_loss", "capture_steal"})) issue(out, "schema.enum", path + "/reason", "bad cancel reason");
    if (kind == "command_dropped" && !enumValue(j["reason"], {"reload_swap", "target_removed", "limit_exceeded"})) issue(out, "schema.enum", path + "/reason", "bad drop reason");
    if (kind == "command_dropped" && (!j.contains("count") || !integer(j["count"], 1, INT32_MAX))) issue(out, "schema.range", path + "/count", "bad count");
    if (kind == "document_swap") {
        for (auto key : {"from_revision", "to_revision"}) if (!j.contains(key) || !j[key].is_string() || !std::regex_match(j[key].get<std::string>(), revision_re)) issue(out, "schema.pattern", path + "/" + key, "bad revision");
        if (!j.contains("dropped_commands") || !integer(j["dropped_commands"], 0, INT32_MAX)) issue(out, "schema.range", path + "/dropped_commands", "bad count");
    }
}

RectI asRect(const Json &j) { return {j[0].get<std::int32_t>(), j[1].get<std::int32_t>(), j[2].get<std::int32_t>(), j[3].get<std::int32_t>()}; }
std::vector<std::string> effectsOf(const Json &event) {
    if (!event.contains("effects")) return {};
    return event["effects"].get<std::vector<std::string>>();
}

Json readJson(const std::filesystem::path &path, std::vector<std::string> &failures) {
    std::ifstream stream(path);
    if (!stream) { failures.push_back("cannot open " + path.generic_string()); return {}; }
    try { return Json::parse(stream); } catch (const std::exception &e) { failures.push_back(path.generic_string() + ": " + e.what()); return {}; }
}

bool containsValue(const Json &fixture, std::string_view group, std::string_view value) {
    auto any = [&](std::string_view array, std::string_view field) {
        if (!fixture.contains(array) || !fixture[array].is_array()) return false;
        for (const auto &item : fixture[array]) {
            if (field == "consumed_class") {
                if (item.contains("consumed")) for (const auto &v : item["consumed"])
                    if (v.is_string() && v.get_ref<const std::string &>().starts_with(std::string(value) + ":")) return true;
            } else if (field == "effects") {
                if (item.contains("effects") && std::find(item["effects"].begin(), item["effects"].end(), std::string(value)) != item["effects"].end()) return true;
            } else if (item.contains(field) && item[field].is_string() && item[field].get_ref<const std::string &>() == value) return true;
        }
        return false;
    };
    if (group == "input_trace.kind") return any("input_trace", "kind");
    if (group == "input_trace.button") return any("input_trace", "button");
    if (group == "effects") return any("input_trace", "effects");
    if (group == "consumed_vocabulary_class") return any("input_trace", "consumed_class");
    if (group == "lifecycle.kind") return any("lifecycle", "kind");
    if (group == "capture_cancel.reason") {
        for (const auto &v : fixture.value("lifecycle", Json::array())) if (v.value("kind", "") == "capture_cancel" && v.value("reason", "") == value) return true;
        return false;
    }
    if (group == "command_dropped.reason") {
        for (const auto &v : fixture.value("lifecycle", Json::array())) if (v.value("kind", "") == "command_dropped" && v.value("reason", "") == value) return true;
        return false;
    }
    if (group == "errors.phase") return any("errors", "phase");
    if (group == "errors.code") return any("errors", "code");
    if (group == "sampler") return any("draw_runs", "sampler");
    return false;
}

} // namespace

bool FixtureValidation::failedClause(std::string_view clause) const noexcept {
    return std::ranges::any_of(semantic_issues, [&](const auto &v) { return v.clause == clause; });
}

std::vector<FixtureIssue> validateSemanticFixtureSchema(const Json &f) {
    std::vector<FixtureIssue> out;
    if (!objectClosed(f, {"schema", "version", "viewport", "document", "widgets", "draw_runs", "total_index_count", "input_trace", "lifecycle"}, {"errors"}, "", out)) return out;
    if (!f.contains("schema") || f["schema"] != "pelican.ui_semantic_fixture") issue(out, "schema.const", "/schema", "bad schema");
    if (!f.contains("version") || f["version"] != 1) issue(out, "schema.const", "/version", "bad version");
    if (f.contains("viewport") && objectClosed(f["viewport"], {"framebuffer_px", "ui_scale", "content_rect_px", "content_rect_ui"}, {}, "/viewport", out)) {
        if (f["viewport"].contains("framebuffer_px")) {
            const auto &v = f["viewport"]["framebuffer_px"];
            if (!v.is_array() || v.size() != 2 || !integer(v[0], 1, 16384) || !integer(v[1], 1, 16384)) issue(out, "schema.range", "/viewport/framebuffer_px", "bad framebuffer");
        }
        if (!f["viewport"].contains("ui_scale") || !f["viewport"]["ui_scale"].is_number() || f["viewport"]["ui_scale"].get<double>() <= 0 || f["viewport"]["ui_scale"].get<double>() > 16) issue(out, "schema.range", "/viewport/ui_scale", "bad scale");
        if (f["viewport"].contains("content_rect_px")) checkRect(f["viewport"]["content_rect_px"], "/viewport/content_rect_px", out);
        if (f["viewport"].contains("content_rect_ui")) checkRect(f["viewport"]["content_rect_ui"], "/viewport/content_rect_ui", out);
    }
    if (f.contains("document") && objectClosed(f["document"], {"key", "revision"}, {}, "/document", out)) {
        if (!f["document"].contains("key") || !f["document"]["key"].is_string() || f["document"]["key"].get_ref<const std::string &>().empty()) issue(out, "schema.length", "/document/key", "bad key");
        if (!f["document"].contains("revision") || !f["document"]["revision"].is_string() || !std::regex_match(f["document"]["revision"].get<std::string>(), revision_re)) issue(out, "schema.pattern", "/document/revision", "bad revision");
    }
    if (!f.contains("widgets") || !f["widgets"].is_array()) issue(out, "schema.type", "/widgets", "expected array");
    else for (std::size_t i = 0; i < f["widgets"].size(); ++i) {
        const auto path = "/widgets/" + std::to_string(i); const auto &w = f["widgets"][i];
        if (!objectClosed(w, {"id", "type", "rect_ui", "clip_ui", "layer", "decl_seq"}, {"overflow"}, path, out)) continue;
        if (!w.contains("id") || !w["id"].is_string() || !std::regex_match(w["id"].get<std::string>(), widget_re)) issue(out, "schema.pattern", path + "/id", "bad widget path");
        if (!w.contains("type") || !w["type"].is_string() || !std::regex_match(w["type"].get<std::string>(), name_re)) issue(out, "schema.pattern", path + "/type", "bad widget type");
        if (w.contains("rect_ui")) checkRect(w["rect_ui"], path + "/rect_ui", out);
        if (w.contains("clip_ui")) checkRect(w["clip_ui"], path + "/clip_ui", out);
        if (!w.contains("layer") || !integer(w["layer"], INT32_MIN, INT32_MAX)) issue(out, "schema.integer", path + "/layer", "bad layer");
        if (!w.contains("decl_seq") || !integer(w["decl_seq"], 0, INT32_MAX)) issue(out, "schema.range", path + "/decl_seq", "bad decl_seq");
        if (w.contains("overflow") && !enumValue(w["overflow"], {"visible", "clip"})) issue(out, "schema.enum", path + "/overflow", "bad overflow");
    }
    if (!f.contains("draw_runs") || !f["draw_runs"].is_array()) issue(out, "schema.type", "/draw_runs", "expected array");
    else for (std::size_t i = 0; i < f["draw_runs"].size(); ++i) {
        const auto path = "/draw_runs/" + std::to_string(i); const auto &r = f["draw_runs"][i];
        if (!objectClosed(r, {"pipeline", "sampler", "texture", "scissor_px", "first_index", "index_count"}, {}, path, out)) continue;
        if (!r.contains("pipeline") || !r["pipeline"].is_string() || !std::regex_match(r["pipeline"].get<std::string>(), name_re)) issue(out, "schema.pattern", path + "/pipeline", "bad pipeline");
        if (!r.contains("sampler") || !enumValue(r["sampler"], {"nearest", "linear"})) issue(out, "schema.enum", path + "/sampler", "bad sampler");
        if (!r.contains("texture") || !r["texture"].is_string() || !std::regex_match(r["texture"].get<std::string>(), texture_re)) issue(out, "schema.pattern", path + "/texture", "bad texture");
        if (r.contains("scissor_px")) checkRect(r["scissor_px"], path + "/scissor_px", out);
        for (auto key : {"first_index", "index_count"}) if (!r.contains(key) || !integer(r[key], 0, key == std::string_view{"index_count"} ? 98304 : INT32_MAX) || r[key].get<std::int64_t>() % 6 != 0) issue(out, "schema.multipleOf", path + "/" + key, "index count must be a nonnegative multiple of 6");
    }
    if (!f.contains("total_index_count") || !integer(f["total_index_count"], 0, 98304) || (f["total_index_count"].is_number_integer() && f["total_index_count"].get<std::int64_t>() % 6 != 0)) issue(out, "schema.multipleOf", "/total_index_count", "bad total index count");
    if (!f.contains("input_trace") || !f["input_trace"].is_array()) issue(out, "schema.type", "/input_trace", "expected array");
    else for (std::size_t i = 0; i < f["input_trace"].size(); ++i) validateTrace(f["input_trace"][i], "/input_trace/" + std::to_string(i), out);
    if (!f.contains("lifecycle") || !f["lifecycle"].is_array()) issue(out, "schema.type", "/lifecycle", "expected array");
    else for (std::size_t i = 0; i < f["lifecycle"].size(); ++i) validateLifecycle(f["lifecycle"][i], "/lifecycle/" + std::to_string(i), out);
    if (f.contains("errors")) {
        if (!f["errors"].is_array()) issue(out, "schema.type", "/errors", "expected array");
        else for (std::size_t i = 0; i < f["errors"].size(); ++i) {
            const auto p = "/errors/" + std::to_string(i); const auto &e = f["errors"][i];
            if (!objectClosed(e, {"phase", "code", "path"}, {"message"}, p, out)) continue;
            if (!e.contains("phase") || !enumValue(e["phase"], {"parse", "layout", "route", "commit"})) issue(out, "schema.enum", p + "/phase", "bad phase");
            if (!e.contains("code") || !enumValue(e["code"], {"unknown_event", "unknown_source", "unknown_widget_value_path", "type_mismatch", "range_violation", "required_missing", "unknown_field", "no_payload_event", "unknown_widget_type", "axis_conflict", "layout_cycle", "duplicate_stable_id", "limit_exceeded", "unsupported_control"})) issue(out, "schema.enum", p + "/code", "bad error code");
            if (!e.contains("path") || !e["path"].is_string() || !std::regex_match(e["path"].get<std::string>(), pointer_re)) issue(out, "schema.pattern", p + "/path", "bad JSON pointer");
        }
    }
    return out;
}

std::vector<FixtureIssue> validateSemanticFixture(const Json &f) {
    std::vector<FixtureIssue> out;
    if (!validateSemanticFixtureSchema(f).empty()) return out;
    auto ordered = [&](const Json &r, const std::string &clause, const std::string &path) { if (!asRect(r).ordered()) issue(out, clause, path, "rect edges inverted"); };
    ordered(f["viewport"]["content_rect_px"], "I1_rect_order", "/viewport/content_rect_px");
    ordered(f["viewport"]["content_rect_ui"], "I1_rect_order", "/viewport/content_rect_ui");
    for (std::size_t i = 0; i < f["widgets"].size(); ++i) { ordered(f["widgets"][i]["rect_ui"], "I1_rect_order", "/widgets/" + std::to_string(i) + "/rect_ui"); ordered(f["widgets"][i]["clip_ui"], "I1_rect_order", "/widgets/" + std::to_string(i) + "/clip_ui"); }
    for (std::size_t i = 0; i < f["draw_runs"].size(); ++i) ordered(f["draw_runs"][i]["scissor_px"], "I1_rect_order", "/draw_runs/" + std::to_string(i) + "/scissor_px");

    std::map<std::string, const Json *> widgets;
    std::int64_t previous_decl = -1;
    for (std::size_t i = 0; i < f["widgets"].size(); ++i) {
        const auto &w = f["widgets"][i]; const auto id = w["id"].get<std::string>();
        if (!widgets.emplace(id, &w).second) issue(out, "I4_id_unique", "/widgets/" + std::to_string(i) + "/id", "duplicate widget id");
        if (w["decl_seq"].get<std::int64_t>() <= previous_decl) issue(out, "I5a_decl_seq_monotonic", "/widgets/" + std::to_string(i) + "/decl_seq", "decl_seq not strictly increasing");
        previous_decl = w["decl_seq"].get<std::int64_t>();
    }
    for (const auto &[id, w] : widgets) {
        for (auto slash = id.find('/'); slash != std::string::npos; slash = id.find('/', slash + 1))
            if (!widgets.contains(id.substr(0, slash))) issue(out, "I2_ancestor_exists", "/widgets", "missing ancestor of " + id);
        RectI clip = asRect(f["viewport"]["content_rect_ui"]);
        for (auto slash = id.find('/'); slash != std::string::npos; slash = id.find('/', slash + 1)) {
            const auto parent = widgets.find(id.substr(0, slash));
            if (parent != widgets.end() && parent->second->value("overflow", "visible") == "clip") clip = intersect(clip, asRect((*parent->second)["rect_ui"]));
        }
        if (asRect((*w)["clip_ui"]) != clip) issue(out, "I3_clip_equality", "/widgets", "clip does not equal clipped-ancestor intersection for " + id);
    }
    std::int64_t previous_event = -1;
    for (std::size_t i = 0; i < f["input_trace"].size(); ++i) {
        const auto seq = f["input_trace"][i]["event_seq"].get<std::int64_t>();
        if (seq <= previous_event) issue(out, "I5b_event_seq_monotonic", "/input_trace/" + std::to_string(i) + "/event_seq", "event_seq not strictly increasing");
        previous_event = seq;
    }
    std::int64_t expected_first = 0;
    for (std::size_t i = 0; i < f["draw_runs"].size(); ++i) {
        const auto first = f["draw_runs"][i]["first_index"].get<std::int64_t>();
        if (i == 0 && first != 0) issue(out, "I6a_run0_zero", "/draw_runs/0/first_index", "first run must start at zero");
        if (i > 0 && first != expected_first) issue(out, "I6b_run_contiguous", "/draw_runs/" + std::to_string(i) + "/first_index", "run gap or overlap");
        expected_first = first + f["draw_runs"][i]["index_count"].get<std::int64_t>();
    }
    if (expected_first != f["total_index_count"].get<std::int64_t>()) issue(out, "I6c_total_match", "/total_index_count", "last run end differs from total");
    if (f["total_index_count"].get<std::int64_t>() / 6 > 16384) issue(out, "I6d_quad_limit", "/total_index_count", "quad limit exceeded");
    const auto fbw = f["viewport"]["framebuffer_px"][0].get<std::int32_t>(); const auto fbh = f["viewport"]["framebuffer_px"][1].get<std::int32_t>();
    for (std::size_t i = 0; i < f["draw_runs"].size(); ++i) { const auto r = asRect(f["draw_runs"][i]["scissor_px"]); if (r.left < 0 || r.top < 0 || r.right > fbw || r.bottom > fbh) issue(out, "I8_scissor_containment", "/draw_runs/" + std::to_string(i) + "/scissor_px", "scissor outside framebuffer"); }
    const std::set<std::string> missing_allowed{"remove", "reload", "scene_unload"};
    for (std::size_t i = 0; i < f["input_trace"].size(); ++i) if (f["input_trace"][i].contains("target") && !widgets.contains(f["input_trace"][i]["target"].get<std::string>())) issue(out, "I10a_dangling_forbidden", "/input_trace/" + std::to_string(i) + "/target", "dangling target");
    for (std::size_t i = 0; i < f["lifecycle"].size(); ++i) if (f["lifecycle"][i].contains("widget") && !widgets.contains(f["lifecycle"][i]["widget"].get<std::string>())) {
        const auto &l = f["lifecycle"][i]; if (l.value("kind", "") != "capture_cancel") issue(out, "I10a_dangling_forbidden", "/lifecycle/" + std::to_string(i) + "/widget", "dangling lifecycle widget");
        else if (!missing_allowed.contains(l.value("reason", ""))) issue(out, "I10b_missing_not_allowed_reason", "/lifecycle/" + std::to_string(i) + "/widget", "missing widget not allowed for reason");
    }

    struct Capture { std::string button; std::string owner; bool drag = false; };
    struct State { std::optional<Capture> capture; std::optional<std::string> hover; };
    std::map<int, State> pointers;
    for (std::size_t i = 0; i < f["input_trace"].size(); ++i) {
        const auto path = "/input_trace/" + std::to_string(i); const auto &e = f["input_trace"][i];
        auto &state = pointers[e["pointer_id"].get<int>()]; const auto kind = e["kind"].get<std::string>(); const auto effects = effectsOf(e);
        const auto target = e.contains("target") ? std::optional<std::string>{e["target"].get<std::string>()} : std::nullopt;
        const auto capture = std::find(effects.begin(), effects.end(), "capture") != effects.end();
        const auto release = std::find(effects.begin(), effects.end(), "release_capture") != effects.end();
        const auto click = std::find(effects.begin(), effects.end(), "click") != effects.end();
        const auto drag_start = std::find(effects.begin(), effects.end(), "drag_start") != effects.end();
        const auto drag = std::find(effects.begin(), effects.end(), "drag") != effects.end();
        const auto cancel = std::find(effects.begin(), effects.end(), "cancel") != effects.end();
        const auto hover_exit = std::find(effects.begin(), effects.end(), "hover_exit") != effects.end();
        const auto hover_enter = std::find(effects.begin(), effects.end(), "hover_enter") != effects.end();
        if (capture && (kind != "pointer_down" || !target)) issue(out, kind == "pointer_down" ? "I11a_capture_requires_target_down" : "I11b_capture_only_on_down", path + "/effects", "capture requires targeted down");
        if (kind == "pointer_down") {
            if (state.capture) issue(out, "I11i_captured_second_button", path, "second button down while captured");
            else if (capture && target) state.capture = Capture{e["button"].get<std::string>(), *target, false};
        } else if (kind == "pointer_move") {
            if (hover_exit && !state.hover) issue(out, "I11e_hover_initial_exit", path + "/effects", "hover_exit without owner");
            if (state.hover && target != state.hover && !(hover_exit && (target ? hover_enter : !hover_enter))) issue(out, "I11f_hover_owner_order", path + "/effects", "hover owner change requires exit then optional enter");
            if (!state.hover && target && !hover_enter) issue(out, "I11f_hover_owner_order", path + "/effects", "initial hover target requires enter");
            if (hover_enter && !target) issue(out, "I11f_hover_owner_order", path + "/target", "hover_enter requires target");
            if (hover_enter) state.hover = target; else if (hover_exit) state.hover.reset();
            if ((drag_start || drag) && !state.capture) issue(out, "I11b_capture_only_on_down", path + "/effects", "drag without capture");
            if (state.capture) {
                if (target && *target != state.capture->owner) issue(out, "I11h_up_owner_matches_capture", path + "/target", "captured move target differs from owner");
                if (drag_start && state.capture->drag) issue(out, "I11d_click_xor_drag", path + "/effects", "duplicate drag_start");
                if (drag && !drag_start && !state.capture->drag) issue(out, "I11d_click_xor_drag", path + "/effects", "drag precedes drag_start");
                if (drag_start) state.capture->drag = true;
            }
        } else if (kind == "pointer_up") {
            if (state.capture) {
                if (e["button"].get<std::string>() != state.capture->button) issue(out, "I11g_up_button_matches_capture", path + "/button", "up button differs from capture");
                if (!target || *target != state.capture->owner) issue(out, "I11h_up_owner_matches_capture", path + "/target", "up target differs from capture owner");
                if (!release) issue(out, "I11c_up_requires_release", path + "/effects", "captured up requires release");
                if (click && state.capture->drag) issue(out, "I11d_click_xor_drag", path + "/effects", "click after drag");
                if (release) state.capture.reset();
            } else if (release || click) issue(out, "I11c_up_requires_release", path + "/effects", "release/click while idle");
        } else {
            if (state.capture) {
                if (!(effects == std::vector<std::string>{"cancel", "release_capture"})) issue(out, "I11k_effect_order", path + "/effects", "captured cancel effects must be ordered cancel,release_capture");
                if (!(cancel && release)) issue(out, "I11c_up_requires_release", path + "/effects", "captured cancel requires cancel and release");
                state.capture.reset(); state.hover.reset();
            } else if (!effects.empty()) issue(out, "I11j_idle_cancel_empty", path + "/effects", "idle cancel must have zero effects");
        }
        auto before = effects;
        const std::map<std::string, int> rank{{"hover_exit",0},{"hover_enter",1},{"capture",2},{"drag_start",3},{"drag",4},{"cancel",5},{"release_capture",6},{"click",7}};
        if (!std::is_sorted(before.begin(), before.end(), [&](const auto &a, const auto &b) { return rank.at(a) < rank.at(b); })) issue(out, "I11k_effect_order", path + "/effects", "effects are not in canonical order");
    }
    const auto px = asRect(f["viewport"]["content_rect_px"]); const auto ui = asRect(f["viewport"]["content_rect_ui"]); const auto scale = f["viewport"]["ui_scale"].get<double>();
    if (px.left < 0 || px.top < 0 || px.right > fbw || px.bottom > fbh) issue(out, "I12a_viewport_containment", "/viewport/content_rect_px", "content outside framebuffer");
    if (ui.left != 0 || ui.top != 0) issue(out, "I12b_viewport_scale_origin", "/viewport/content_rect_ui", "content UI origin must be zero");
    const auto round_edge = [&](std::int32_t v) { return static_cast<std::int64_t>(std::floor(static_cast<double>(v) * scale + 0.5)); };
    if (px.width() != round_edge(ui.right) - round_edge(ui.left) || px.height() != round_edge(ui.bottom) - round_edge(ui.top)) issue(out, "I12b_viewport_scale_origin", "/viewport", "content extent differs from edge-rounded scale");
    return out;
}

FixtureValidation validateSemanticFixtureAll(const Json &fixture) {
    FixtureValidation result; result.schema_issues = validateSemanticFixtureSchema(fixture);
    if (result.schema_issues.empty()) result.semantic_issues = validateSemanticFixture(fixture);
    return result;
}

bool semanticFixtureEqual(const Json &actual, const Json &expected) { return actual == expected; }

std::string writeSemanticFixture(const Json &fixture) {
    const auto validation = validateSemanticFixtureAll(fixture);
    if (!validation.schemaValid()) throw std::invalid_argument("cannot write structurally invalid semantic fixture");
    return fixture.dump(2) + "\n";
}

CoverageGateResult runSemanticCoverageGate(const std::filesystem::path &root) {
    CoverageGateResult result;
    const auto base = root / "test/fixtures/ui_semantic/normative";
    auto manifest = readJson(base / "coverage.json", result.failures);
    auto registry = readJson(root / "docs/schemas/pelican.ui_semantic_clauses.json", result.failures);
    if (!result.failures.empty()) return result;
    std::set<std::string> registry_ids;
    if (!registry.is_object() || registry.value("schema", "") != "pelican.ui_semantic_clauses" || registry.value("version", 0) != 1 || !registry.contains("clauses") || !registry["clauses"].is_array()) result.failures.push_back("invalid clause registry header");
    else for (const auto &clause : registry["clauses"]) {
        if (!clause.is_object() || !clause.contains("id") || !clause["id"].is_string() || !registry_ids.insert(clause["id"].get<std::string>()).second) result.failures.push_back("invalid or duplicate registry clause");
    }
    if (!manifest.is_object() || manifest.value("schema", "") != "pelican.ui_semantic_fixture.coverage" || manifest.value("version", 0) != 3 || !manifest.contains("description") || !manifest["description"].is_string() || manifest["description"].get<std::string>().size() > 2000) result.failures.push_back("coverage manifest header/schema invalid");
    std::set<std::string> manifest_clauses;
    std::map<std::filesystem::path, FixtureValidation> cache;
    auto checkEntry = [&](const std::string &where, const Json &entry, const std::optional<std::pair<std::string,std::string>> &enum_witness, const std::optional<std::string> &semantic_clause) {
        ++result.checked_entries;
        if (!entry.is_object() || !entry.contains("fixture") || !entry["fixture"].is_string() || !entry.contains("gate") || !entry["gate"].is_string()) { result.failures.push_back(where + ": malformed entry"); return; }
        for (auto it = entry.begin(); it != entry.end(); ++it) if (it.key() != "fixture" && it.key() != "gate" && it.key() != "clause") result.failures.push_back(where + ": unknown entry property " + it.key());
        if (entry.contains("clause") && (!entry["clause"].is_string() || entry["clause"].get<std::string>().size() > 200)) result.failures.push_back(where + ": invalid clause prose");
        const auto rel = entry["fixture"].get<std::string>(); const auto path = base / rel; const auto gate = entry["gate"].get<std::string>();
        if (gate != "schema" && gate != "semantic") result.failures.push_back(where + ": invalid gate");
        if (!std::filesystem::exists(path)) { result.failures.push_back(where + ": fixture missing " + rel); return; }
        if (!cache.contains(path)) { auto j = readJson(path, result.failures); cache.emplace(path, validateSemanticFixtureAll(j)); ++result.checked_fixtures; }
        const auto &validation = cache.at(path);
        const bool expected_schema_valid = !rel.starts_with("invalid/");
        if (validation.schemaValid() != expected_schema_valid) result.failures.push_back(where + ": schema classification mismatch for " + rel);
        if (rel.starts_with("valid/") && !validation.semanticValid()) result.failures.push_back(where + ": valid fixture failed semantic validation");
        if (enum_witness) { auto j = readJson(path, result.failures); if (!containsValue(j, enum_witness->first, enum_witness->second)) result.failures.push_back(where + ": enum witness value absent"); }
        if (semantic_clause) {
            if (gate == "semantic" && rel.starts_with("semantic_invalid/") && !validation.failedClause(*semantic_clause)) result.failures.push_back(where + ": fixture did not fail named clause " + *semantic_clause);
            if (gate == "semantic" && rel.starts_with("valid/") && !validation.semanticValid()) result.failures.push_back(where + ": positive semantic witness failed");
            if (gate == "schema" && validation.schemaValid()) result.failures.push_back(where + ": schema witness unexpectedly valid");
        }
    };
    if (manifest.contains("enum_coverage") && manifest["enum_coverage"].is_object()) for (auto group = manifest["enum_coverage"].begin(); group != manifest["enum_coverage"].end(); ++group) {
        if (!group.value().is_object() || group.value().empty()) { result.failures.push_back("empty enum group " + group.key()); continue; }
        for (auto entry = group.value().begin(); entry != group.value().end(); ++entry) checkEntry("enum." + group.key() + "." + entry.key(), entry.value(), std::pair{group.key(), entry.key()}, std::nullopt);
    } else result.failures.push_back("enum_coverage missing");
    if (manifest.contains("invalid_class_coverage") && manifest["invalid_class_coverage"].is_object()) for (auto it = manifest["invalid_class_coverage"].begin(); it != manifest["invalid_class_coverage"].end(); ++it) checkEntry("invalid." + it.key(), it.value(), std::nullopt, std::nullopt);
    else result.failures.push_back("invalid_class_coverage missing");
    if (manifest.contains("semantic_invariant_coverage") && manifest["semantic_invariant_coverage"].is_object()) for (auto it = manifest["semantic_invariant_coverage"].begin(); it != manifest["semantic_invariant_coverage"].end(); ++it) { manifest_clauses.insert(it.key()); checkEntry("semantic." + it.key(), it.value(), std::nullopt, it.key()); }
    else result.failures.push_back("semantic_invariant_coverage missing");
    if (registry_ids != manifest_clauses) result.failures.push_back("clause registry and coverage manifest are not an exact set match");
    return result;
}

} // namespace Pelican::ui
