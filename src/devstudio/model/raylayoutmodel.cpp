#include "raylayoutmodel.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace PelicanStudio {
namespace {

using Json = RayJson;

std::string stringAt(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

Json arrayAt(const Json &object, const char *key) {
    if (!object.is_object()) return Json::array();
    const auto found = object.find(key);
    return found != object.end() && found->is_array() ? *found : Json::array();
}

Json objectAt(const Json &object, const char *key) {
    if (!object.is_object()) return Json::object();
    const auto found = object.find(key);
    return found != object.end() && found->is_object() ? *found : Json::object();
}

bool contains(const std::vector<std::string> &names, const std::string &name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// A legacy stage field (miss / closesthit): one reference or a list.
std::vector<std::string> referenceList(const Json &value) {
    std::vector<std::string> references;
    if (value.is_string()) references.push_back(value.get<std::string>());
    if (value.is_array())
        for (const auto &entry : value)
            if (entry.is_string()) references.push_back(entry.get<std::string>());
    return references;
}

std::optional<std::string> readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) return std::nullopt;
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

bool fileExists(const std::filesystem::path &path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

RayLayoutDocument parseDocument(const Json &value) {
    RayLayoutDocument document;
    if (!value.is_object()) return document;
    document.named = true;
    document.reference = stringAt(value, "reference");
    document.digest = stringAt(value, "digest");
    if (const auto found = value.find("document"); found != value.end()) document.document = *found;
    if (const auto error = value.find("error"); error != value.end() && error->is_object()) {
        document.error_code = stringAt(*error, "code");
        document.error_message = stringAt(*error, "message");
        if (document.error_code.empty()) document.error_code = "render_document_unreadable";
    }
    document.text = stringAt(value, "text");
    return document;
}

const char *const StudioRayTracingSetMark = "a ray tracing set Studio wrote (raygen + miss + closest hit)";
const char *const LayoutHeader = "__pelican_ray_group_layout.glsl";

// project://shaders/x -> (directory "project://shaders/", stem "x",
// extension "" ); with an extension it is kept.
struct ReferenceParts {
    std::string head;
    std::string stem;
    std::string extension;
};

ReferenceParts splitReference(const std::string &reference) {
    ReferenceParts parts;
    const auto slash = reference.find_last_of('/');
    parts.head = slash == std::string::npos ? std::string{} : reference.substr(0, slash + 1);
    auto tail = slash == std::string::npos ? reference : reference.substr(slash + 1);
    const auto dot = tail.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        parts.extension = tail.substr(dot);
        tail = tail.substr(0, dot);
    }
    parts.stem = tail;
    return parts;
}

std::string joinReference(const ReferenceParts &parts) { return parts.head + parts.stem + parts.extension; }

// A file name beside `reference` for a stem of its own, `<stem>_<suffix>`
// (then _2, _3...) whose stage files do not exist yet -- or hold `wanted`
// already (an earlier Enable that was taken back left it).
std::string freshStem(const std::filesystem::path &root, const ReferenceParts &parts,
                      const std::string &suffix,
                      const std::vector<std::pair<std::string, std::string>> &wanted) {
    const auto base = parts.stem + "_" + suffix;
    for (int index = 1;; ++index) {
        auto candidate = parts;
        candidate.stem = index == 1 ? base : base + "_" + std::to_string(index);
        candidate.extension.clear();
        bool free = true;
        for (const auto &[stage, content] : wanted) {
            const auto path = rayShaderFile(root, joinReference(candidate), stage);
            if (!path) return candidate.stem;
            if (!fileExists(*path)) continue;
            if (readText(*path) != content) free = false;
        }
        if (free) return candidate.stem;
    }
}

Json withRayTracing(const RayTracingTask &task, const Json &ray_tracing) {
    Json body = task.body;
    body["ray_tracing"] = ray_tracing;
    return body;
}

// The plan's layout, starting from the one read.
Json layoutOf(const RayLayoutRead &read) {
    return read.layout.document.is_object() ? read.layout.document : Json::object();
}

std::vector<std::size_t> layoutTasks(const std::vector<RayTracingTask> &tasks) {
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < tasks.size(); ++index)
        if (rayTracingUsesLayout(tasks[index].rayTracing())) indices.push_back(index);
    return indices;
}

bool layoutReady(const RayLayoutRead &read, RayLayoutPlan &plan) {
    if (read.layout.usable()) return true;
    plan.problem = read.layout.named ? "The ray layout cannot be read: " + read.layout.error_message
                                     : std::string{"This project has no ray layout. Enable hit groups first."};
    return false;
}

Json recordZeros(const Json &layout) {
    Json record = Json::object();
    for (const auto &field : rayLayoutRecord(layout)) record[field.name] = rayRecordZero(field.type);
    return record;
}

// A material row's record made to hold exactly the layout's fields.
Json fittedRecord(const Json &row, const Json &layout) {
    const auto old = objectAt(row, "record");
    Json record = Json::object();
    for (const auto &field : rayLayoutRecord(layout))
        record[field.name] = old.contains(field.name) ? old.at(field.name) : rayRecordZero(field.type);
    return record;
}

std::string writeFileName(const std::filesystem::path &root, const std::filesystem::path &path) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return (error ? path : relative).generic_string();
}

// A stem under shaders/ for a template of its own.
std::string templateStem(const std::filesystem::path &root, const std::string &base,
                         const std::vector<std::string> &extensions) {
    for (int index = 1;; ++index) {
        const auto stem = index == 1 ? base : base + "_" + std::to_string(index);
        bool free = true;
        for (const auto &extension : extensions)
            if (fileExists(root / "shaders" / (stem + extension))) free = false;
        if (free) return stem;
    }
}

void addWrite(RayLayoutPlan &plan, const std::filesystem::path &path, std::string content) {
    for (auto &write : plan.shaders)
        if (write.path == path) {
            write.content = std::move(content);
            return;
        }
    plan.shaders.push_back({path, std::move(content), readText(path)});
}

std::string groupTypeOf(const Json &layout, const std::string &group) {
    for (const auto &entry : rayLayoutGroups(layout))
        if (entry.name == group) return entry.type;
    return {};
}

// RTL-2s-c: a plan that changes the layout's record writes the record block
// of every hit shader the tasks trace with, before the edit (the engine
// checks each against the record at the commit).
void fitRecords(RayLayoutPlan &plan, const std::vector<RayTracingTask> &tasks, const std::filesystem::path &root) {
    if (root.empty() || !plan.layout.is_object()) return;
    auto fit = fitRayShaderRecords(rayTracedHitShaderFiles(tasks, root), plan.layout, root);
    for (auto &write : fit.writes) addWrite(plan, write.path, std::move(write.content));
    for (auto &problem : fit.problems) plan.notes.push_back(std::move(problem));
}

} // namespace

// --- get_ray_layout.

bool RayLayoutRead::restartPending() const {
    if (!mode) return false;
    if (layout.named && *mode == "none") return true;
    return !layout.named && *mode == "layout";
}

RayLayoutRead parseRayLayoutRead(const Json &response) {
    RayLayoutRead read;
    if (!response.is_object()) return read;
    if (const auto digest = response.find("source_digest"); digest != response.end()) {
        if (digest->is_object()) read.source_digest = stringAt(*digest, "hex");
        else if (digest->is_string()) read.source_digest = digest->get<std::string>();
    }
    if (const auto mode = response.find("mode"); mode != response.end() && mode->is_string())
        read.mode = mode->get<std::string>();
    read.layout = parseDocument(response.value("layout", Json(nullptr)));
    read.bindings = parseDocument(response.value("bindings", Json(nullptr)));
    for (const auto &entry : arrayAt(response, "selectable_materials"))
        if (entry.is_object() && entry.contains("selector"))
            read.materials.push_back({stringAt(entry, "label"), stringAt(entry, "name"), entry.at("selector")});
    for (const auto &entry : arrayAt(response, "selectable_procedural_assets"))
        if (entry.is_object())
            read.procedurals.push_back({stringAt(entry, "asset"), objectAt(entry, "record_fields")});
    for (const auto &entry : arrayAt(response, "unresolved_bindings"))
        if (entry.is_object())
            read.unresolved.push_back({entry.value("index", std::int64_t{0}),
                                       entry.value("selector", Json(nullptr)), stringAt(entry, "group")});
    return read;
}

// --- Names.

std::optional<std::string> rayNameProblem(RayNameKind kind, std::string_view name,
                                          const std::vector<std::string> &taken) {
    if (name.empty()) return std::string{"A name is needed."};
    if (name.size() > 64) return std::string{"A name is at most 64 characters."};
    if (!std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
        return std::string{"A name is letters, digits and '_' only."};
    if (contains(taken, std::string{name})) return "'" + std::string{name} + "' is taken.";
    if (kind == RayNameKind::record_field) {
        static const std::set<std::string, std::less<>> reserved{
            "material_index",      "geometry_index",      "geometry_kind",       "world_to_geometry_0",
            "world_to_geometry_1", "world_to_geometry_2", "geometry_to_world_0", "geometry_to_world_1",
            "geometry_to_world_2"};
        if (reserved.contains(name)) return "'" + std::string{name} + "' is the engine record's.";
    }
    return std::nullopt;
}

std::string rayFreeName(const std::string &base, const std::vector<std::string> &taken) {
    if (!contains(taken, base)) return base;
    for (int index = 2;; ++index)
        if (auto name = base + "_" + std::to_string(index); !contains(taken, name)) return name;
}

// --- The layout document.

const std::vector<std::string> &rayRecordTypes() {
    static const std::vector<std::string> types{"f32", "i32", "u32", "vec2", "vec3", "vec4"};
    return types;
}

const std::vector<std::string> &rayEngineRecordFields() {
    static const std::vector<std::string> fields{"material_index", "geometry_index", "geometry_kind",
                                                 "procedural_transform"};
    return fields;
}

Json rayRecordZero(const std::string &type) {
    if (type == "vec2") return Json::array({0.0, 0.0});
    if (type == "vec3") return Json::array({0.0, 0.0, 0.0});
    if (type == "vec4") return Json::array({0.0, 0.0, 0.0, 0.0});
    if (type == "f32") return 0.0;
    return 0;
}

std::vector<std::string> rayLayoutRayTypes(const Json &layout) {
    std::vector<std::string> names;
    for (const auto &entry : arrayAt(layout, "ray_types"))
        if (entry.is_string()) names.push_back(entry.get<std::string>());
    return names;
}

std::vector<RayGroupEntry> rayLayoutGroups(const Json &layout) {
    std::vector<RayGroupEntry> groups;
    for (const auto &entry : arrayAt(layout, "groups"))
        if (entry.is_object()) groups.push_back({stringAt(entry, "name"), stringAt(entry, "type")});
    return groups;
}

std::vector<std::string> rayLayoutGroupNames(const Json &layout) {
    std::vector<std::string> names;
    for (const auto &group : rayLayoutGroups(layout)) names.push_back(group.name);
    return names;
}

std::vector<std::string> rayLayoutMisses(const Json &layout) {
    std::vector<std::string> names;
    for (const auto &entry : arrayAt(layout, "misses"))
        if (entry.is_object()) names.push_back(stringAt(entry, "name"));
    return names;
}

std::vector<RayRecordField> rayLayoutRecord(const Json &layout) {
    std::vector<RayRecordField> fields;
    for (const auto &entry : arrayAt(layout, "record"))
        if (entry.is_object()) fields.push_back({stringAt(entry, "name"), stringAt(entry, "type")});
    return fields;
}

std::vector<std::string> rayLayoutEngineRecord(const Json &layout) {
    std::vector<std::string> fields;
    for (const auto &entry : arrayAt(layout, "engine_record"))
        if (entry.is_string()) fields.push_back(entry.get<std::string>());
    return fields;
}

std::string rayLayoutDefaultGroup(const Json &layout) { return stringAt(layout, "default_group"); }

Json defaultRayLayout() {
    return {{"schema", "pelican.ray_group_layout"},
            {"version", 3},
            {"ray_types", Json::array({"primary"})},
            {"default_group", "default"},
            {"groups", Json::array({{{"name", "default"}, {"type", "triangles"}}})},
            {"misses", Json::array({{{"name", "miss"}}})},
            {"engine_record", Json::array({"material_index", "geometry_index"})},
            {"record", Json::array()}};
}

// --- Tasks.

const Json &RayTracingTask::rayTracing() const {
    static const Json none = Json::object();
    const auto found = body.find("ray_tracing");
    return found != body.end() && found->is_object() ? *found : none;
}

Json RayTracingTask::edit(const Json &ray_tracing) const {
    if (kind == Kind::graph_node)
        return {{"method", "edit_graph_document"},
                {"params",
                 {{"ops", Json::array({{{"op", "set_node_parameters"},
                                        {"scope", scope},
                                        {"node", name},
                                        {"parameters", withRayTracing(*this, ray_tracing)}}})}}}};
    return {{"method", "edit_compute_task"},
            {"params",
             {{"task_name", name}, {"base_task_source_digest", digest}, {"task", withRayTracing(*this, ray_tracing)}}}};
}

std::vector<RayTracingTask> rayTracingTasks(const Json &render_documents) {
    std::vector<RayTracingTask> tasks;
    for (const auto &document : arrayAt(render_documents, "documents")) {
        const auto kind = stringAt(document, "kind");
        if (kind != "root" && kind != "feature") continue;
        const auto content = Json::parse(stringAt(document, "content"), nullptr, false);
        if (!content.is_object()) continue;
        if (kind == "root") {
            const auto nodes = [&](const Json &list, const std::string &scope) {
                if (!list.is_array()) return;
                for (const auto &node : list) {
                    if (stringAt(node, "op") != "render.compute") continue;
                    const auto parameters = objectAt(node, "parameters");
                    if (!parameters.contains("ray_tracing") || !parameters.at("ray_tracing").is_object()) continue;
                    RayTracingTask task;
                    task.kind = RayTracingTask::Kind::graph_node;
                    task.name = stringAt(node, "id");
                    task.scope = scope;
                    task.document = stringAt(document, "reference");
                    task.digest = stringAt(document, "digest");
                    task.body = parameters;
                    tasks.push_back(std::move(task));
                }
            };
            nodes(content.value("nodes", Json::array()), "root");
            const auto definitions = objectAt(content, "definitions");
            for (const auto &[name, body] : definitions.items()) nodes(arrayAt(body, "nodes"), name);
            continue;
        }
        for (const auto &task_json : arrayAt(content, "compute_tasks")) {
            if (!task_json.is_object() || !task_json.contains("ray_tracing") || !task_json.at("ray_tracing").is_object())
                continue;
            RayTracingTask task;
            task.kind = RayTracingTask::Kind::feature;
            task.name = stringAt(task_json, "name");
            task.document = stringAt(document, "reference");
            task.digest = stringAt(document, "digest");
            task.body = task_json;
            tasks.push_back(std::move(task));
        }
    }
    return tasks;
}

bool rayTracingUsesLayout(const Json &ray_tracing) {
    return ray_tracing.is_object() &&
           (ray_tracing.contains("hit_shaders") || ray_tracing.contains("misses") ||
            ray_tracing.contains("traced_ray_types"));
}

RayLayoutTemplateNames rayLayoutTemplateNames(const Json &layout) {
    if (auto names = rayLayoutTemplateNamesFromDocument(layout)) return std::move(*names);
    return {};
}

Json rayTracingInLayoutForm(const Json &legacy, const Json &layout, const std::string &intersection) {
    Json converted = legacy.is_object() ? legacy : Json::object();
    const auto hits = referenceList(converted.value("closesthit", Json(nullptr)));
    const auto misses = referenceList(converted.value("miss", Json(nullptr)));
    converted.erase("miss");
    converted.erase("closesthit");
    const auto ray_types = rayLayoutRayTypes(layout);
    Json traced = Json::array();
    Json hit_shaders = Json::object();
    for (std::size_t k = 0; k < hits.size() && k < ray_types.size(); ++k) {
        traced.push_back(ray_types[k]);
        Json row = Json::object();
        for (const auto &group : rayLayoutGroups(layout)) {
            Json cell{{"closesthit", hits[k]}};
            if (group.type == "procedural" && !intersection.empty()) cell["intersection"] = intersection;
            row[group.name] = std::move(cell);
        }
        hit_shaders[ray_types[k]] = std::move(row);
    }
    Json miss_map = Json::object();
    if (!misses.empty()) {
        const auto names = rayLayoutMisses(layout);
        for (std::size_t j = 0; j < names.size(); ++j) miss_map[names[j]] = misses[std::min(j, misses.size() - 1)];
    }
    converted["traced_ray_types"] = std::move(traced);
    converted["misses"] = std::move(miss_map);
    converted["hit_shaders"] = std::move(hit_shaders);
    return converted;
}

std::vector<std::filesystem::path> rayTracedHitShaderFiles(const std::vector<RayTracingTask> &tasks,
                                                           const std::filesystem::path &root) {
    std::vector<std::filesystem::path> files;
    const auto add = [&](const std::string &reference, const char *stage) {
        const auto file = rayShaderFile(root, reference, stage);
        if (file && std::find(files.begin(), files.end(), *file) == files.end()) files.push_back(*file);
    };
    for (const auto &task : tasks) {
        const auto &rt = task.rayTracing();
        if (!rayTracingUsesLayout(rt)) continue;
        const auto hit_shaders = objectAt(rt, "hit_shaders");
        for (const auto &traced : arrayAt(rt, "traced_ray_types")) {
            if (!traced.is_string()) continue;
            // Held: items() of a temporary would outlive it.
            const auto row = objectAt(hit_shaders, traced.get<std::string>().c_str());
            for (const auto &[group, cell] : row.items()) {
                (void)group;
                if (const auto reference = stringAt(cell, "closesthit"); !reference.empty()) add(reference, "closesthit");
                if (const auto reference = stringAt(cell, "anyhit"); !reference.empty()) add(reference, "anyhit");
                if (const auto reference = stringAt(cell, "intersection"); !reference.empty())
                    add(reference, "intersection");
            }
        }
    }
    return files;
}

RayRecordFit fitRayShaderRecords(const std::vector<std::filesystem::path> &files, const Json &layout,
                                 const std::filesystem::path &root) {
    RayRecordFit fit;
    const auto names = rayLayoutTemplateNames(layout);
    const bool needs_record = !names.engine_record.empty() || !names.record.empty();
    std::string fields = names.engine_record.empty() ? std::string{} : std::string{"the engine record"};
    for (const auto &[field, type] : names.record) fields += (fields.empty() ? "" : ", ") + field + " (" + type + ")";
    std::vector<std::filesystem::path> seen;
    for (const auto &file : files) {
        if (std::find(seen.begin(), seen.end(), file) != seen.end()) continue;
        seen.push_back(file);
        const auto text = readText(file);
        if (!text) continue;
        if (auto rewritten = rewriteRayRecordBlock(*text, names)) {
            if (*rewritten != *text) fit.writes.push_back({file, std::move(*rewritten), *text});
            continue;
        }
        const bool intersection = file.extension() == ".rint";
        if (declaresRayShaderRecord(*text)) {
            fit.problems.push_back(writeFileName(root, file) +
                                   " declares its own shader record: give it the ray layout's fields in order (" +
                                   (fields.empty() ? std::string{"none"} : fields) + "), or take it out.");
        } else if (needs_record && !intersection) {
            fit.problems.push_back(writeFileName(root, file) +
                                   " has no shader record, which a hit shader of a traced ray type needs: declare "
                                   "layout(shaderRecordEXT, std430) buffer with " + fields + ".");
        }
    }
    return fit;
}

std::vector<std::string> rayTracedRowNotes(const Json &ray_tracing, const std::string &ray_type, const Json &layout,
                                           const std::filesystem::path &root) {
    std::vector<std::string> notes;
    const auto traced = arrayAt(ray_tracing, "traced_ray_types");
    if (traced.empty() || !traced.front().is_string()) return notes;
    const auto first = traced.front().get<std::string>();
    if (first == ray_type) return notes;
    // Held: items() of a temporary would outlive it.
    const auto hit_shaders = objectAt(ray_tracing, "hit_shaders");
    const auto row = objectAt(hit_shaders, first.c_str());
    std::vector<std::filesystem::path> files;
    std::string names;
    for (const auto &[group, cell] : row.items()) {
        (void)group;
        for (const char *stage : {"closesthit", "anyhit", "intersection"}) {
            const auto reference = stringAt(cell, stage);
            if (reference.empty()) continue;
            const auto file = rayShaderFile(root, reference, stage);
            if (!file || std::find(files.begin(), files.end(), *file) != files.end()) continue;
            files.push_back(*file);
            if (std::string_view{stage} != "intersection")
                names += (names.empty() ? "" : ", ") + writeFileName(root, *file);
        }
    }
    if (!names.empty())
        notes.push_back(ray_type + "'s row took " + first + "'s hit shaders (" + names + "), which answer in " +
                        first + "'s payload: New... in each " + ray_type + " cell writes one for " + ray_type + ".");
    for (auto &problem : fitRayShaderRecords(files, layout, root).problems) notes.push_back(std::move(problem));
    return notes;
}

// --- Shaders.

std::string_view rayStageExtension(std::string_view stage) {
    if (stage == "raygen") return ".rgen";
    if (stage == "miss") return ".rmiss";
    if (stage == "closesthit") return ".rchit";
    if (stage == "anyhit") return ".rahit";
    if (stage == "intersection") return ".rint";
    if (stage == "callable") return ".rcall";
    return {};
}

std::optional<std::filesystem::path> rayShaderFile(const std::filesystem::path &project_root,
                                                   const std::string &reference, std::string_view stage) {
    constexpr std::string_view scheme = "project://";
    if (project_root.empty() || !reference.starts_with(scheme)) return std::nullopt;
    auto relative = reference.substr(scheme.size());
    if (relative.empty() || relative.find("..") != std::string::npos) return std::nullopt;
    const auto parts = splitReference(reference);
    if (parts.extension.empty()) relative += std::string{rayStageExtension(stage)};
    return project_root / std::filesystem::path{relative};
}

namespace {

// The arguments of the call whose '(' is at `open`; `close` gets the ')'.
std::vector<std::pair<std::size_t, std::size_t>> callArguments(const std::string &source, std::size_t open,
                                                               std::size_t &close) {
    std::vector<std::pair<std::size_t, std::size_t>> arguments;
    int depth = 0;
    std::size_t start = open + 1;
    for (std::size_t at = open; at < source.size(); ++at) {
        const char c = source[at];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (--depth == 0) {
                arguments.emplace_back(start, at);
                close = at;
                return arguments;
            }
        } else if (c == ',' && depth == 1) {
            arguments.emplace_back(start, at);
            start = at + 1;
        }
    }
    close = std::string::npos;
    return {};
}

std::string trimmed(std::string_view text) {
    auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    auto end = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(begin, end - begin + 1)};
}

std::optional<std::size_t> integerLiteral(const std::string &text) {
    auto value = text;
    if (!value.empty() && (value.back() == 'u' || value.back() == 'U')) value.pop_back();
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::nullopt;
    return static_cast<std::size_t>(std::stoul(value));
}

bool identifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool inLineComment(const std::string &source, std::size_t at) {
    const auto line = source.rfind('\n', at);
    const auto from = line == std::string::npos ? 0 : line + 1;
    return source.substr(from, at - from).find("//") != std::string::npos;
}

// Puts `#include "__pelican_ray_group_layout.glsl"` (and the include
// directive) after the shader's #extension lines.
std::string withLayoutHeader(const std::string &source) {
    if (source.find(LayoutHeader) != std::string::npos) return source;
    const auto newline = source.find("\r\n") != std::string::npos ? std::string{"\r\n"} : std::string{"\n"};
    std::string lines;
    if (source.find("GL_GOOGLE_include_directive") == std::string::npos)
        lines += "#extension GL_GOOGLE_include_directive : enable" + newline;
    lines += std::string{"#include \""} + LayoutHeader + "\"" + newline;
    std::size_t insert = 0;
    std::size_t at = 0;
    while (at < source.size()) {
        const auto end = source.find('\n', at);
        const auto line = source.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const auto text = trimmed(line);
        if (text.starts_with("#version") || text.starts_with("#extension")) insert = end == std::string::npos ? source.size() : end + 1;
        if (end == std::string::npos) break;
        at = end + 1;
        if (!text.empty() && !text.starts_with("#") && !text.starts_with("//")) break;
    }
    return source.substr(0, insert) + lines + source.substr(insert);
}

} // namespace

std::optional<RayTraceRewrite> rewriteLegacyTraceCalls(const std::string &source,
                                                       const std::vector<std::string> &ray_types,
                                                       const std::vector<std::string> &misses,
                                                       std::string *problem) {
    RayTraceRewrite result;
    std::string out;
    std::size_t copied = 0;
    const std::string word = "traceRayEXT";
    std::size_t next = 0;
    for (auto at = source.find(word); at != std::string::npos; at = source.find(word, next)) {
        next = at + word.size();
        if ((at > 0 && identifierChar(source[at - 1])) ||
            (at + word.size() < source.size() && identifierChar(source[at + word.size()])))
            continue;
        if (inLineComment(source, at)) continue;
        auto open = source.find_first_not_of(" \t\r\n", at + word.size());
        if (open == std::string::npos || source[open] != '(') continue;
        std::size_t close = 0;
        const auto arguments = callArguments(source, open, close);
        if (arguments.size() != 11) {
            if (problem) *problem = "a traceRayEXT call does not have its 11 arguments";
            return std::nullopt;
        }
        const auto argument = [&](std::size_t index) {
            return trimmed(std::string_view{source}.substr(arguments[index].first,
                                                           arguments[index].second - arguments[index].first));
        };
        const auto offset = integerLiteral(argument(3));
        const auto stride = integerLiteral(argument(4));
        const auto miss = integerLiteral(argument(5));
        if (!offset || !stride || !miss) {
            if (problem) *problem = "traceRayEXT routes with " + argument(3) + ", " + argument(4) + ", " + argument(5) +
                                    " (not integer literals)";
            return std::nullopt;
        }
        if (*offset >= ray_types.size() || *miss >= misses.size()) {
            if (problem) *problem = "traceRayEXT routes to an offset or miss the task does not have";
            return std::nullopt;
        }
        const std::array<std::string, 3> replacement{"PELICAN_RAY_TYPE_" + ray_types[*offset],
                                                     std::string{"pelican_ray_type_count"},
                                                     "PELICAN_MISS_" + misses[*miss]};
        for (std::size_t index = 0; index < 3; ++index) {
            const auto [begin, end] = arguments[3 + index];
            // Keep the whitespace around the literal.
            const auto first = source.find_first_not_of(" \t\r\n", begin);
            const auto last = source.find_last_not_of(" \t\r\n", end - 1) + 1;
            out += source.substr(copied, first - copied);
            out += replacement[index];
            copied = last;
        }
        ++result.calls;
        next = close;
    }
    if (result.calls == 0) return std::nullopt;
    out += source.substr(copied);
    result.content = withLayoutHeader(out);
    return result;
}

int countMacroUses(const std::string &source, const std::string &macro) {
    int count = 0;
    for (auto at = source.find(macro); at != std::string::npos; at = source.find(macro, at + macro.size())) {
        const bool before = at > 0 && identifierChar(source[at - 1]);
        const bool after = at + macro.size() < source.size() && identifierChar(source[at + macro.size()]);
        if (!before && !after) ++count;
    }
    return count;
}

std::string replaceMacroUses(const std::string &source, const std::string &from, const std::string &to) {
    std::string out;
    std::size_t copied = 0;
    for (auto at = source.find(from); at != std::string::npos; at = source.find(from, at + from.size())) {
        const bool before = at > 0 && identifierChar(source[at - 1]);
        const bool after = at + from.size() < source.size() && identifierChar(source[at + from.size()]);
        if (before || after) continue;
        out += source.substr(copied, at - copied) + to;
        copied = at + from.size();
    }
    return out + source.substr(copied);
}

std::vector<std::filesystem::path> rayProjectShaderFiles(const std::filesystem::path &project_root) {
    static const std::set<std::string> extensions{".glsl", ".rgen", ".rmiss", ".rchit", ".rahit", ".rint",
                                                  ".rcall", ".comp", ".vert", ".frag", ".hlsl"};
    std::vector<std::filesystem::path> files;
    std::error_code error;
    const auto directory = project_root / "shaders";
    if (!std::filesystem::is_directory(directory, error)) return files;
    for (std::filesystem::recursive_directory_iterator it{directory, error}, end; !error && it != end;
         it.increment(error))
        if (it->is_regular_file(error) && extensions.contains(it->path().extension().string()))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    return files;
}

// --- Plans.

RayLayoutRequest rayLayoutRequest(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                                  const RayLayoutPlan &plan) {
    Json layout_edit = Json::object();
    if (!plan.layout.is_null() || plan.remove_layout) {
        layout_edit["layout"] = plan.remove_layout ? Json(nullptr) : plan.layout;
        if (read.layout.named && !read.layout.digest.empty()) layout_edit["base_layout_digest"] = read.layout.digest;
    }
    if (!plan.bindings.is_null() || plan.remove_bindings) {
        layout_edit["bindings"] = plan.remove_bindings ? Json(nullptr) : plan.bindings;
        if (read.bindings.named && !read.bindings.digest.empty())
            layout_edit["base_bindings_digest"] = read.bindings.digest;
    }
    if (plan.procedural_records.is_object() && !plan.procedural_records.empty())
        layout_edit["procedural_records"] = plan.procedural_records;
    if (plan.tasks.empty()) {
        layout_edit["base_source_digest"] = read.source_digest;
        return {"edit_ray_layout", layout_edit};
    }
    Json edits = Json::array();
    if (!layout_edit.empty()) edits.push_back({{"method", "edit_ray_layout"}, {"params", layout_edit}});
    // A graph's nodes in one edit_graph_document; each feature task its own
    // edit_compute_task.
    Json graph_ops = Json::array();
    for (const auto &[index, ray_tracing] : plan.tasks) {
        if (index >= tasks.size()) continue;
        auto edit = tasks[index].edit(ray_tracing);
        if (tasks[index].kind == RayTracingTask::Kind::graph_node) {
            for (auto &op : edit.at("params").at("ops")) graph_ops.push_back(std::move(op));
            continue;
        }
        edits.push_back(std::move(edit));
    }
    if (!graph_ops.empty())
        edits.push_back({{"method", "edit_graph_document"}, {"params", {{"ops", std::move(graph_ops)}}}});
    return {"edit_batch", {{"base_source_digest", read.source_digest}, {"edits", std::move(edits)}}};
}

RayLayoutPlan planEnableHitGroups(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                                  const std::filesystem::path &root) {
    RayLayoutPlan plan;
    plan.label = "Enable hit groups";
    if (read.layout.named) {
        plan.problem = "This project already has a ray layout.";
        return plan;
    }
    // As many ray types as the longest closesthit list, as many misses as
    // the longest miss list: a legacy trace's sbtRecordOffset k picked
    // closesthit[k], so k becomes ray type k (every object in the default
    // group), and its missIndex j miss j.
    std::size_t ray_type_count = 1;
    std::size_t miss_count = 1;
    for (const auto &task : tasks) {
        const auto &rt = task.rayTracing();
        if (rayTracingUsesLayout(rt)) continue;
        ray_type_count = std::max(ray_type_count, referenceList(rt.value("closesthit", Json(nullptr))).size());
        miss_count = std::max(miss_count, referenceList(rt.value("miss", Json(nullptr))).size());
    }
    auto layout = defaultRayLayout();
    std::vector<std::string> ray_types{"primary"};
    for (std::size_t index = 1; index < ray_type_count; ++index)
        ray_types.push_back(rayFreeName("ray_" + std::to_string(index + 1), ray_types));
    std::vector<std::string> misses{"miss"};
    for (std::size_t index = 1; index < miss_count; ++index)
        misses.push_back(rayFreeName("miss_" + std::to_string(index + 1), misses));
    layout["ray_types"] = ray_types;
    layout["misses"] = Json::array();
    for (const auto &miss : misses) layout["misses"].push_back({{"name", miss}});
    plan.layout = layout;
    const std::string group = rayLayoutDefaultGroup(layout);

    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const auto &task = tasks[index];
        auto rt = task.rayTracing();
        if (rayTracingUsesLayout(rt)) continue;
        auto task_misses = referenceList(rt.value("miss", Json(nullptr)));
        auto task_hits = referenceList(rt.value("closesthit", Json(nullptr)));
        auto raygen = stringAt(rt, "raygen");
        if (task_misses.empty() || task_hits.empty() || raygen.empty()) {
            plan.notes.push_back(task.name + " has no raygen, miss or closest hit; it is left as it is.");
            continue;
        }
        // Studio's own starting set (CT-6s), one miss and one closest hit:
        // the layout's starting set in its place, beside it.
        const auto raygen_file = rayShaderFile(root, raygen, "raygen");
        const auto raygen_text = raygen_file ? readText(*raygen_file) : std::nullopt;
        const bool studio_set = raygen_text && raygen_text->find(StudioRayTracingSetMark) != std::string::npos &&
                                task_misses.size() == 1 && task_hits.size() == 1;
        if (studio_set) {
            auto parts = splitReference(raygen);
            // RTL-2s-c: the Graph tab's templates (raylayouttemplate), for
            // the layout's first ray type, first miss and default group.
            const auto names = rayLayoutTemplateNames(layout);
            const auto set = [&](const std::string &stem) {
                return std::vector<std::pair<std::string, std::string>>{
                    {"raygen", rayLayoutShaderTemplate(names, {RayLayoutTemplateStage::raygen, {}, {}, {}}, stem)},
                    {"miss", rayLayoutShaderTemplate(names, {RayLayoutTemplateStage::miss, {}, {}, misses.front()},
                                                     stem)},
                    {"closesthit",
                     rayLayoutShaderTemplate(names, {RayLayoutTemplateStage::closest_hit, ray_types.front(), group, {}},
                                             stem)}};
            };
            auto stem = freshStem(root, parts, "layout", set(parts.stem + "_layout"));
            parts.stem = stem;
            parts.extension.clear();
            const auto reference = joinReference(parts);
            const auto contents = set(stem);
            for (const auto &[stage, content] : contents)
                if (const auto path = rayShaderFile(root, reference, stage)) addWrite(plan, *path, content);
            raygen = reference;
            task_misses = {reference};
            task_hits = {reference};
            plan.notes.push_back(task.name + ": Studio's starting set is now " + reference +
                                 " in the ray layout's form (the files before are kept).");
        } else {
            // Any other shader: its literal trace routing rewritten, into a
            // file beside it.
            const auto rewrite = [&](const std::string &reference, const char *stage) -> std::string {
                const auto file = rayShaderFile(root, reference, stage);
                if (!file) return reference;
                const auto text = readText(*file);
                if (!text || text->find("traceRayEXT") == std::string::npos) return reference;
                std::vector<std::string> task_types(ray_types.begin(),
                                                    ray_types.begin() + static_cast<std::ptrdiff_t>(task_hits.size()));
                std::vector<std::string> task_miss_names(
                    misses.begin(), misses.begin() + static_cast<std::ptrdiff_t>(task_misses.size()));
                std::string why;
                const auto rewritten = rewriteLegacyTraceCalls(*text, task_types, task_miss_names, &why);
                if (!rewritten) {
                    plan.notes.push_back(writeFileName(root, *file) + " is left as it is: " + why +
                                         ". Write PELICAN_RAY_TYPE_" + ray_types.front() +
                                         ", pelican_ray_type_count and PELICAN_MISS_" + misses.front() +
                                         " there, and include __pelican_ray_group_layout.glsl.");
                    return reference;
                }
                auto parts = splitReference(reference);
                const auto extension = parts.extension;
                const auto stem = freshStem(root, parts, "layout", {{stage, rewritten->content}});
                parts.stem = stem;
                parts.extension = extension;
                const auto new_reference = joinReference(parts);
                if (const auto path = rayShaderFile(root, new_reference, stage))
                    addWrite(plan, *path, rewritten->content);
                plan.notes.push_back(writeFileName(root, *file) + ": its trace routing is written with the ray "
                                     "layout's names in " + new_reference + " (the file before is kept).");
                return new_reference;
            };
            raygen = rewrite(raygen, "raygen");
            for (auto &hit : task_hits) hit = rewrite(hit, "closesthit");
        }
        // RTL-2s-c: the same rule the Graph's palette places a task with.
        Json legacy = rt;
        legacy["raygen"] = raygen;
        legacy["miss"] = task_misses;
        legacy["closesthit"] = task_hits;
        plan.tasks.emplace_back(index, rayTracingInLayoutForm(legacy, layout));
        // A closest hit somebody wrote has no shader record yet: named.
        if (!studio_set) {
            std::vector<std::filesystem::path> hits;
            for (const auto &hit : task_hits)
                if (const auto file = rayShaderFile(root, hit, "closesthit")) hits.push_back(*file);
            for (auto &problem : fitRayShaderRecords(hits, layout, root).problems) plan.notes.push_back(problem);
        }
    }
    return plan;
}

RayLayoutPlan planDisableHitGroups(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                                   const std::filesystem::path &root) {
    RayLayoutPlan plan;
    plan.label = "Disable hit groups";
    if (!layoutReady(read, plan)) return plan;
    const auto &layout = read.layout.document;
    const auto groups = rayLayoutGroups(layout);
    if (groups.size() != 1 || groups.front().type != "triangles") {
        plan.problem = "Hit groups stay: the ray layout has " + std::to_string(groups.size()) +
                       " hit groups, and without a ray layout every object answers with the same closest hit.";
        return plan;
    }
    const auto ray_types = rayLayoutRayTypes(layout);
    const auto misses = rayLayoutMisses(layout);
    // A shader that names the generated header goes back to the file it was
    // made from, when that one is beside it and does not.
    const auto legacyReference = [&](const std::string &reference, const char *stage) -> std::optional<std::string> {
        const auto file = rayShaderFile(root, reference, stage);
        if (!file) return reference;
        // A file Enable made beside the one before (<stem>_layout...): that
        // one, when it is there and does not use the header.
        auto parts = splitReference(reference);
        if (const auto at = parts.stem.rfind("_layout"); at != std::string::npos) {
            parts.stem = parts.stem.substr(0, at);
            const auto before = joinReference(parts);
            const auto before_file = rayShaderFile(root, before, stage);
            const auto before_text = before_file ? readText(*before_file) : std::nullopt;
            if (before_text && before_text->find(LayoutHeader) == std::string::npos) return before;
        }
        const auto text = readText(*file);
        if (!text || text->find(LayoutHeader) == std::string::npos) return reference;
        return std::nullopt;
    };
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const auto &rt = tasks[index].rayTracing();
        if (!rayTracingUsesLayout(rt)) continue;
        Json legacy = rt;
        for (const auto *key : {"traced_ray_types", "misses", "hit_shaders"}) legacy.erase(key);
        const auto keep = [&](const std::string &reference, const char *stage) -> std::optional<std::string> {
            auto back = legacyReference(reference, stage);
            if (!back) {
                const auto file = rayShaderFile(root, reference, stage);
                plan.problem = "Hit groups stay: " + (file ? writeFileName(root, *file) : reference) +
                               " uses the ray layout's names (PELICAN_RAY_TYPE_*, PELICAN_MISS_*). Undo Enable hit "
                               "groups instead, or write it without them.";
            }
            return back;
        };
        const auto raygen = keep(stringAt(rt, "raygen"), "raygen");
        if (!raygen) return plan;
        legacy["raygen"] = *raygen;
        Json miss_list = Json::array();
        for (const auto &miss : misses) {
            const auto reference = stringAt(objectAt(rt, "misses"), miss.c_str());
            const auto back = keep(reference, "miss");
            if (!back) return plan;
            miss_list.push_back(*back);
        }
        Json hit_list = Json::array();
        const auto hit_shaders = objectAt(rt, "hit_shaders");
        for (const auto &type : ray_types) {
            const auto cell = objectAt(objectAt(hit_shaders, type.c_str()), groups.front().name.c_str());
            if (cell.contains("anyhit") || cell.contains("intersection")) {
                plan.problem = "Hit groups stay: " + tasks[index].name + " has an any hit shader, which needs a ray layout.";
                return plan;
            }
            auto reference = stringAt(cell, "closesthit");
            if (reference.empty() && !hit_list.empty()) reference = hit_list.front().get<std::string>();
            if (reference.empty()) continue;
            const auto back = keep(reference, "closesthit");
            if (!back) return plan;
            hit_list.push_back(*back);
        }
        legacy["miss"] = miss_list.size() == 1 ? miss_list.front() : miss_list;
        legacy["closesthit"] = hit_list.size() == 1 ? hit_list.front() : hit_list;
        plan.tasks.emplace_back(index, std::move(legacy));
    }
    plan.remove_layout = true;
    if (read.bindings.named) plan.remove_bindings = true;
    return plan;
}

RayLayoutPlan planAddRayType(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                             const std::string &name, const std::filesystem::path &root, RayFillMode mode) {
    RayLayoutPlan plan;
    plan.label = "Add ray type " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    const auto types = rayLayoutRayTypes(layout);
    if (auto problem = rayNameProblem(RayNameKind::ray_type, name, types)) {
        plan.problem = problem;
        return plan;
    }
    layout["ray_types"].push_back(name);
    plan.layout = layout;
    // RTL-2s-c: from the template, a closest hit of this ray type for each
    // hit group -- its payload (location = its index) and the record.
    Json made_row = nullptr;
    if (mode == RayFillMode::from_template && !root.empty()) {
        const auto names = rayLayoutTemplateNames(layout);
        made_row = Json::object();
        for (const auto &group : rayLayoutGroups(layout)) {
            const auto stem = templateStem(root, name + "_" + group.name, {".rchit"});
            addWrite(plan, root / "shaders" / (stem + ".rchit"),
                     rayLayoutShaderTemplate(names, {RayLayoutTemplateStage::closest_hit, name, group.name, {}}, stem));
            made_row[group.name] = {{"closesthit", "project://shaders/" + stem}};
        }
        plan.notes.push_back("The closest hits of " + name + " are shaders/" + name + "_<hit group>.rchit.");
    }
    // Each task traces it too, its row copied from the first ray type it
    // traces (or the template's, with the first row's intersections).
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        auto traced = arrayAt(rt, "traced_ray_types");
        const auto first = traced.empty() ? std::string{} : traced.front().get<std::string>();
        traced.push_back(name);
        rt["traced_ray_types"] = traced;
        const auto first_row = objectAt(objectAt(rt, "hit_shaders"), first.c_str());
        if (made_row.is_object()) {
            auto row = made_row;
            for (auto &[group, cell] : row.items())
                if (const auto intersection = stringAt(objectAt(first_row, group.c_str()), "intersection");
                    !intersection.empty())
                    cell["intersection"] = intersection;
            rt["hit_shaders"][name] = row;
        } else {
            rt["hit_shaders"][name] = first_row;
        }
        plan.tasks.emplace_back(index, std::move(rt));
    }
    return plan;
}

RayLayoutPlan planAddGroup(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                           const std::filesystem::path &root, const std::string &name, const std::string &type,
                           RayFillMode mode) {
    RayLayoutPlan plan;
    plan.label = "Add hit group " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    const auto groups = rayLayoutGroups(layout);
    if (auto problem = rayNameProblem(RayNameKind::group, name, rayLayoutGroupNames(layout))) {
        plan.problem = problem;
        return plan;
    }
    if (type != "triangles" && type != "procedural") {
        plan.problem = "A hit group is Triangles or Procedural.";
        return plan;
    }
    layout["groups"].push_back({{"name", name}, {"type", type}});
    plan.layout = layout;
    // The column copied from: the default group when it is of this type,
    // else the first group of this type.
    std::string source;
    if (groupTypeOf(layout, rayLayoutDefaultGroup(layout)) == type) source = rayLayoutDefaultGroup(layout);
    for (const auto &group : groups)
        if (source.empty() && group.type == type) source = group.name;
    if (mode == RayFillMode::copy && source.empty()) mode = RayFillMode::from_template;
    // RTL-2s-c: from the template (raylayouttemplate), a closest hit for each
    // ray type a task traces -- the first ray type's under hit_<name>, a later
    // one's under hit_<name>_<ray type>, each in its payload -- and a
    // procedural group's intersection.
    std::map<std::string, Json> made;
    if (mode == RayFillMode::from_template) {
        const bool procedural = type == "procedural";
        const auto names = rayLayoutTemplateNames(layout);
        const auto ray_types = rayLayoutRayTypes(layout);
        std::string intersection;
        if (procedural) {
            const auto stem = templateStem(root, "hit_" + name, {".rint"});
            addWrite(plan, root / "shaders" / (stem + ".rint"),
                     rayLayoutShaderTemplate(names, {RayLayoutTemplateStage::intersection, {}, name, {}}, stem));
            intersection = "project://shaders/" + stem;
        }
        std::vector<std::string> traced_types;
        for (const auto index : layoutTasks(tasks))
            for (const auto &traced : arrayAt(tasks[index].rayTracing(), "traced_ray_types"))
                if (traced.is_string() && !contains(traced_types, traced.get<std::string>()))
                    traced_types.push_back(traced.get<std::string>());
        // No task yet: the first ray type's, for the next one.
        if (traced_types.empty() && !ray_types.empty()) traced_types.push_back(ray_types.front());
        for (const auto &ray_type : traced_types) {
            const bool first = !ray_types.empty() && ray_types.front() == ray_type;
            const auto stem =
                templateStem(root, first ? "hit_" + name : "hit_" + name + "_" + ray_type, {".rchit"});
            addWrite(plan, root / "shaders" / (stem + ".rchit"),
                     rayLayoutShaderTemplate(names, {RayLayoutTemplateStage::closest_hit, ray_type, name, {}}, stem));
            Json cell{{"closesthit", "project://shaders/" + stem}};
            if (!intersection.empty()) cell["intersection"] = intersection;
            made[ray_type] = std::move(cell);
            if (first) plan.notes.push_back("The closest hit of " + name + " is shaders/" + stem + ".rchit.");
        }
    }
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        for (const auto &traced : arrayAt(rt, "traced_ray_types")) {
            if (!traced.is_string()) continue;
            const auto type_name = traced.get<std::string>();
            auto row = objectAt(objectAt(rt, "hit_shaders"), type_name.c_str());
            const auto found = made.find(type_name);
            row[name] = found != made.end() ? found->second : objectAt(row, source.c_str());
            rt["hit_shaders"][type_name] = row;
        }
        plan.tasks.emplace_back(index, std::move(rt));
    }
    return plan;
}

RayLayoutPlan planAddMiss(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                          const std::filesystem::path &root, const std::string &name, RayFillMode mode) {
    RayLayoutPlan plan;
    plan.label = "Add miss shader " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    const auto misses = rayLayoutMisses(layout);
    if (auto problem = rayNameProblem(RayNameKind::miss, name, misses)) {
        plan.problem = problem;
        return plan;
    }
    layout["misses"].push_back({{"name", name}});
    plan.layout = layout;
    std::string made;
    if (mode == RayFillMode::from_template || misses.empty()) {
        const auto stem = templateStem(root, "miss_" + name, {".rmiss"});
        // RTL-2s-c: miss i answers ray type i's payload (raylayouttemplate).
        addWrite(plan, root / "shaders" / (stem + ".rmiss"),
                 rayLayoutShaderTemplate(rayLayoutTemplateNames(layout), {RayLayoutTemplateStage::miss, {}, {}, name},
                                         stem));
        made = "project://shaders/" + stem;
    }
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        const auto task_misses = objectAt(rt, "misses");
        rt["misses"][name] = !made.empty() ? Json(made)
                             : task_misses.contains(misses.front()) ? task_misses.at(misses.front())
                                                                    : Json(made);
        plan.tasks.emplace_back(index, std::move(rt));
    }
    return plan;
}

RayLayoutPlan planRemoveRayType(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                                const std::string &name) {
    RayLayoutPlan plan;
    plan.label = "Remove ray type " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    auto types = rayLayoutRayTypes(layout);
    if (!contains(types, name)) {
        plan.problem = "There is no ray type " + name + ".";
        return plan;
    }
    if (types.size() == 1) {
        plan.problem = "A ray layout keeps at least one ray type.";
        return plan;
    }
    types.erase(std::remove(types.begin(), types.end(), name), types.end());
    layout["ray_types"] = types;
    plan.layout = layout;
    int tracing = 0;
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        Json traced = Json::array();
        for (const auto &entry : arrayAt(rt, "traced_ray_types"))
            if (entry != name) traced.push_back(entry);
        if (traced.size() == arrayAt(rt, "traced_ray_types").size()) continue;
        ++tracing;
        if (traced.empty()) {
            plan.problem = tasks[index].name + " traces only " + name + ".";
            return plan;
        }
        rt["traced_ray_types"] = traced;
        if (rt.contains("hit_shaders")) rt["hit_shaders"].erase(name);
        plan.tasks.emplace_back(index, std::move(rt));
    }
    plan.confirm = "Remove ray type " + name + "? " + std::to_string(tracing) +
                   " task(s) stop tracing it; a shader that names PELICAN_RAY_TYPE_" + name + " has to change too.";
    return plan;
}

RayLayoutPlan planRemoveGroup(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                              const std::string &name) {
    RayLayoutPlan plan;
    plan.label = "Remove hit group " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    if (rayLayoutDefaultGroup(layout) == name) {
        plan.problem = name + " is the default hit group: make another one the default first.";
        return plan;
    }
    Json groups = Json::array();
    for (const auto &entry : arrayAt(layout, "groups"))
        if (stringAt(entry, "name") != name) groups.push_back(entry);
    if (groups.size() == arrayAt(layout, "groups").size()) {
        plan.problem = "There is no hit group " + name + ".";
        return plan;
    }
    layout["groups"] = groups;
    plan.layout = layout;
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        bool changed = false;
        if (rt.contains("hit_shaders") && rt.at("hit_shaders").is_object())
            for (auto &[type_name, row] : rt["hit_shaders"].items())
                if (row.is_object() && row.contains(name)) {
                    row.erase(name);
                    changed = true;
                }
        if (changed) plan.tasks.emplace_back(index, std::move(rt));
    }
    std::size_t removed = 0;
    if (read.bindings.usable()) {
        auto bindings = read.bindings.document;
        Json kept = Json::array();
        for (const auto &row : arrayAt(bindings, "bindings"))
            if (stringAt(row, "group") == name) ++removed;
            else kept.push_back(row);
        if (removed > 0) {
            bindings["bindings"] = kept;
            plan.bindings = bindings;
        }
    }
    plan.confirm = "Remove hit group " + name + "? " + std::to_string(removed) +
                   " assignment(s) that pick it are removed too; their objects go to the default group.";
    return plan;
}

RayLayoutPlan planRemoveMiss(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                             const std::string &name) {
    RayLayoutPlan plan;
    plan.label = "Remove miss shader " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    Json misses = Json::array();
    for (const auto &entry : arrayAt(layout, "misses"))
        if (stringAt(entry, "name") != name) misses.push_back(entry);
    if (misses.size() == arrayAt(layout, "misses").size()) {
        plan.problem = "There is no miss shader " + name + ".";
        return plan;
    }
    if (misses.empty()) {
        plan.problem = "A ray layout keeps at least one miss shader.";
        return plan;
    }
    layout["misses"] = misses;
    plan.layout = layout;
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        if (rt.contains("misses") && rt.at("misses").is_object() && rt.at("misses").contains(name)) {
            rt["misses"].erase(name);
            plan.tasks.emplace_back(index, std::move(rt));
        }
    }
    plan.confirm = "Remove miss shader " + name + "? A shader that names PELICAN_MISS_" + name + " has to change too.";
    return plan;
}

RayLayoutPlan planRename(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks, RayEntryKind kind,
                         const std::string &from, const std::string &to,
                         const std::vector<std::filesystem::path> &shader_files) {
    RayLayoutPlan plan;
    plan.label = "Rename " + from + " to " + to;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    std::vector<std::string> names = kind == RayEntryKind::ray_type ? rayLayoutRayTypes(layout)
                                     : kind == RayEntryKind::group  ? rayLayoutGroupNames(layout)
                                                                    : rayLayoutMisses(layout);
    if (!contains(names, from)) {
        plan.problem = "There is no " + from + ".";
        return plan;
    }
    names.erase(std::remove(names.begin(), names.end(), from), names.end());
    const auto name_kind = kind == RayEntryKind::ray_type ? RayNameKind::ray_type
                           : kind == RayEntryKind::group  ? RayNameKind::group
                                                          : RayNameKind::miss;
    if (auto problem = rayNameProblem(name_kind, to, names)) {
        plan.problem = problem;
        return plan;
    }
    if (kind == RayEntryKind::ray_type) {
        for (auto &entry : layout["ray_types"])
            if (entry == from) entry = to;
    } else if (kind == RayEntryKind::group) {
        for (auto &entry : layout["groups"])
            if (stringAt(entry, "name") == from) entry["name"] = to;
        if (rayLayoutDefaultGroup(layout) == from) layout["default_group"] = to;
    } else {
        for (auto &entry : layout["misses"])
            if (stringAt(entry, "name") == from) entry["name"] = to;
    }
    plan.layout = layout;
    const auto renameKey = [&](Json &object) {
        if (!object.is_object() || !object.contains(from)) return false;
        object[to] = object.at(from);
        object.erase(from);
        return true;
    };
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        bool changed = false;
        if (kind == RayEntryKind::ray_type) {
            if (rt.contains("traced_ray_types"))
                for (auto &entry : rt["traced_ray_types"])
                    if (entry == from) {
                        entry = to;
                        changed = true;
                    }
            if (rt.contains("hit_shaders")) changed = renameKey(rt["hit_shaders"]) || changed;
        } else if (kind == RayEntryKind::group) {
            if (rt.contains("hit_shaders") && rt.at("hit_shaders").is_object())
                for (auto &[type_name, row] : rt["hit_shaders"].items()) changed = renameKey(row) || changed;
        } else if (rt.contains("misses")) {
            changed = renameKey(rt["misses"]);
        }
        if (changed) plan.tasks.emplace_back(index, std::move(rt));
    }
    if (kind == RayEntryKind::group && read.bindings.usable()) {
        auto bindings = read.bindings.document;
        bool changed = false;
        for (auto &row : bindings["bindings"])
            if (stringAt(row, "group") == from) {
                row["group"] = to;
                changed = true;
            }
        if (changed) plan.bindings = bindings;
    }
    if (kind != RayEntryKind::group) {
        const auto prefix = kind == RayEntryKind::ray_type ? std::string{"PELICAN_RAY_TYPE_"} : std::string{"PELICAN_MISS_"};
        const auto source = prefix + from;
        const auto target = prefix + to;
        std::string listed;
        for (const auto &file : shader_files) {
            const auto text = readText(file);
            if (!text) continue;
            const auto uses = countMacroUses(*text, source);
            if (uses == 0) continue;
            plan.shaders.push_back({file, replaceMacroUses(*text, source, target), *text});
            if (!listed.empty()) listed += ", ";
            listed += file.filename().string() + " (" + std::to_string(uses) + ")";
        }
        if (!listed.empty())
            plan.confirm = "Rename " + from + " to " + to + "? " + source + " becomes " + target + " in " + listed + ".";
    }
    return plan;
}

RayLayoutPlan planMove(const RayLayoutRead &read, RayEntryKind kind, const std::string &name, int by) {
    RayLayoutPlan plan;
    plan.label = "Move " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    const char *key = kind == RayEntryKind::ray_type ? "ray_types" : kind == RayEntryKind::group ? "groups" : "misses";
    auto list = arrayAt(layout, key);
    for (std::size_t index = 0; index < list.size(); ++index) {
        const auto entry_name = list[index].is_string() ? list[index].get<std::string>() : stringAt(list[index], "name");
        if (entry_name != name) continue;
        const auto other = static_cast<std::ptrdiff_t>(index) + by;
        if (other < 0 || other >= static_cast<std::ptrdiff_t>(list.size())) return plan;
        std::swap(list[index], list[static_cast<std::size_t>(other)]);
        layout[key] = list;
        plan.layout = layout;
        return plan;
    }
    return plan;
}

RayLayoutPlan planSetDefaultGroup(const RayLayoutRead &read, const std::string &name) {
    RayLayoutPlan plan;
    plan.label = "Default hit group " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    if (!contains(rayLayoutGroupNames(layout), name)) {
        plan.problem = "There is no hit group " + name + ".";
        return plan;
    }
    layout["default_group"] = name;
    plan.layout = layout;
    return plan;
}

RayLayoutPlan planSetGroupType(const RayLayoutRead &read, const std::vector<RayTracingTask> &tasks,
                               const std::string &name, const std::string &type) {
    RayLayoutPlan plan;
    plan.label = "Hit group " + name + " " + type;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    for (auto &entry : layout["groups"])
        if (stringAt(entry, "name") == name) entry["type"] = type;
    plan.layout = layout;
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        bool changed = false;
        if (rt.contains("hit_shaders") && rt.at("hit_shaders").is_object())
            for (auto &[type_name, row] : rt["hit_shaders"].items()) {
                if (!row.is_object() || !row.contains(name) || !row.at(name).is_object()) continue;
                auto &cell = row[name];
                if (type == "triangles" && cell.contains("intersection")) {
                    cell.erase("intersection");
                    changed = true;
                }
                if (type == "procedural" && !cell.contains("intersection")) {
                    plan.problem = tasks[index].name + "'s " + name + " cells need an intersection shader first.";
                    return plan;
                }
            }
        if (changed) plan.tasks.emplace_back(index, std::move(rt));
    }
    return plan;
}

RayLayoutPlan planAddRecordField(const RayLayoutRead &read, const std::string &name, const std::string &type,
                                 const std::vector<RayTracingTask> &tasks, const std::filesystem::path &root) {
    RayLayoutPlan plan;
    plan.label = "Add record field " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    std::vector<std::string> names;
    for (const auto &field : rayLayoutRecord(layout)) names.push_back(field.name);
    if (auto problem = rayNameProblem(RayNameKind::record_field, name, names)) {
        plan.problem = problem;
        return plan;
    }
    if (!contains(rayRecordTypes(), type)) {
        plan.problem = "A record field is f32, i32, u32, vec2, vec3 or vec4.";
        return plan;
    }
    if (!layout.contains("record") || !layout.at("record").is_array()) layout["record"] = Json::array();
    layout["record"].push_back({{"name", name}, {"type", type}});
    plan.layout = layout;
    if (read.bindings.usable()) {
        auto bindings = read.bindings.document;
        bool changed = false;
        for (auto &row : bindings["bindings"])
            if (row.is_object() && row.contains("material")) {
                row["record"] = fittedRecord(row, layout);
                changed = true;
            }
        if (changed) plan.bindings = bindings;
    }
    if (!read.procedurals.empty()) {
        plan.procedural_records = Json::object();
        for (const auto &asset : read.procedurals) {
            auto record = asset.record_fields;
            record[name] = rayRecordZero(type);
            plan.procedural_records[asset.asset] = record;
        }
    }
    fitRecords(plan, tasks, root);
    return plan;
}

RayLayoutPlan planRemoveRecordField(const RayLayoutRead &read, const std::string &name,
                                    const std::vector<RayTracingTask> &tasks, const std::filesystem::path &root) {
    RayLayoutPlan plan;
    plan.label = "Remove record field " + name;
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    Json record = Json::array();
    for (const auto &entry : arrayAt(layout, "record"))
        if (stringAt(entry, "name") != name) record.push_back(entry);
    layout["record"] = record;
    plan.layout = layout;
    if (read.bindings.usable()) {
        auto bindings = read.bindings.document;
        bool changed = false;
        for (auto &row : bindings["bindings"])
            if (row.is_object() && row.contains("material")) {
                row["record"] = fittedRecord(row, layout);
                changed = true;
            }
        if (changed) plan.bindings = bindings;
    }
    if (!read.procedurals.empty()) {
        plan.procedural_records = Json::object();
        for (const auto &asset : read.procedurals) {
            auto fields = asset.record_fields;
            fields.erase(name);
            plan.procedural_records[asset.asset] = fields;
        }
    }
    plan.confirm = "Remove record field " + name + "? Every assignment's value of it goes; a shader that reads it "
                   "from its shader record has to change too.";
    fitRecords(plan, tasks, root);
    return plan;
}

RayLayoutPlan planSetEngineRecord(const RayLayoutRead &read, const std::vector<std::string> &fields,
                                  const std::vector<RayTracingTask> &tasks, const std::filesystem::path &root) {
    RayLayoutPlan plan;
    plan.label = "Engine record";
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    if (fields.empty())
        layout.erase("engine_record");
    else
        layout["engine_record"] = fields;
    plan.layout = layout;
    fitRecords(plan, tasks, root);
    return plan;
}

// --- Bindings.

Json rayBindingsDocument(const RayLayoutRead &read) {
    if (read.bindings.usable()) return read.bindings.document;
    return {{"schema", "pelican.ray_bindings"}, {"version", 2}, {"bindings", Json::array()}};
}

std::optional<std::size_t> rayBindingIndex(const Json &bindings, const Json &selector) {
    const auto rows = arrayAt(bindings, "bindings");
    for (std::size_t index = 0; index < rows.size(); ++index)
        if (rows[index].is_object() && rows[index].contains("material") && rows[index].at("material") == selector)
            return index;
    return std::nullopt;
}

RayLayoutPlan planAssignMaterial(const RayLayoutRead &read, const Json &selector, const std::string &group) {
    RayLayoutPlan plan;
    plan.label = "Assign hit group";
    if (!layoutReady(read, plan)) return plan;
    const auto &layout = read.layout.document;
    if (!group.empty() && !contains(rayLayoutGroupNames(layout), group)) {
        plan.problem = "There is no hit group " + group + ".";
        return plan;
    }
    auto bindings = rayBindingsDocument(read);
    auto rows = arrayAt(bindings, "bindings");
    const auto found = rayBindingIndex(bindings, selector);
    if (group.empty()) {
        if (!found) return plan;
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(*found));
    } else if (found) {
        rows[*found]["group"] = group;
        rows[*found]["record"] = fittedRecord(rows[*found], layout);
    } else {
        rows.push_back({{"material", selector}, {"group", group}, {"record", recordZeros(layout)}});
    }
    bindings["bindings"] = rows;
    plan.bindings = bindings;
    return plan;
}

RayLayoutPlan planSetMaterialRecord(const RayLayoutRead &read, const Json &selector, const std::string &field,
                                    const Json &value) {
    RayLayoutPlan plan;
    plan.label = "Record " + field;
    if (!layoutReady(read, plan)) return plan;
    auto bindings = rayBindingsDocument(read);
    const auto found = rayBindingIndex(bindings, selector);
    if (!found) {
        plan.problem = "Assign a hit group first.";
        return plan;
    }
    auto &row = bindings["bindings"][*found];
    row["record"] = fittedRecord(row, read.layout.document);
    row["record"][field] = value;
    plan.bindings = bindings;
    return plan;
}

RayLayoutPlan planAssignProcedural(const RayLayoutRead &read, const std::string &asset, const std::string &group) {
    RayLayoutPlan plan;
    plan.label = "Assign hit group";
    if (!layoutReady(read, plan)) return plan;
    auto bindings = rayBindingsDocument(read);
    Json rows = Json::array();
    bool placed = false;
    for (const auto &row : arrayAt(bindings, "bindings")) {
        if (stringAt(row, "procedural") == asset) {
            placed = true;
            if (!group.empty()) rows.push_back({{"procedural", asset}, {"group", group}});
            continue;
        }
        rows.push_back(row);
    }
    if (!placed && !group.empty()) rows.push_back({{"procedural", asset}, {"group", group}});
    bindings["bindings"] = rows;
    plan.bindings = bindings;
    return plan;
}

RayLayoutPlan planSetProceduralRecord(const RayLayoutRead &read, const std::string &asset, const std::string &field,
                                      const Json &value) {
    RayLayoutPlan plan;
    plan.label = "Record " + field;
    if (!layoutReady(read, plan)) return plan;
    for (const auto &entry : read.procedurals)
        if (entry.asset == asset) {
            auto record = entry.record_fields;
            record[field] = value;
            plan.procedural_records = Json{{asset, record}};
            return plan;
        }
    plan.problem = "There is no procedural asset " + asset + ".";
    return plan;
}

RayLayoutPlan planRemoveBinding(const RayLayoutRead &read, std::size_t index) {
    RayLayoutPlan plan;
    plan.label = "Remove assignment";
    if (!read.bindings.usable()) return plan;
    auto bindings = read.bindings.document;
    auto rows = arrayAt(bindings, "bindings");
    if (index >= rows.size()) return plan;
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(index));
    bindings["bindings"] = rows;
    plan.bindings = bindings;
    return plan;
}

RayLayoutPlan planPutBack(const RayLayoutRead &before, const RayLayoutRead &now) {
    RayLayoutPlan plan;
    plan.label = "Undo";
    if (before.layout.usable() && before.layout.document != now.layout.document) plan.layout = before.layout.document;
    if (before.bindings.usable() && before.bindings.document != now.bindings.document)
        plan.bindings = before.bindings.document;
    Json records = Json::object();
    for (const auto &old : before.procedurals)
        for (const auto &current : now.procedurals)
            if (current.asset == old.asset && current.record_fields != old.record_fields)
                records[old.asset] = old.record_fields;
    if (!records.empty()) plan.procedural_records = records;
    return plan;
}

std::string raySelectorLabel(const RayLayoutRead &read, const Json &selector) {
    for (const auto &material : read.materials)
        if (material.selector == selector) return material.label.empty() ? material.name : material.label;
    if (selector.is_object() && selector.contains("procedural")) return stringAt(selector, "procedural");
    if (selector.is_object() && selector.contains("document")) return stringAt(selector, "document");
    if (selector.is_object() && selector.contains("asset"))
        return stringAt(selector, "asset") + " #" + std::to_string(selector.value("source_material_index", 0));
    return selector.dump();
}

bool sameRenderDocument(std::string_view left, std::string_view right) {
    constexpr std::string_view scheme = "project://";
    if (left.starts_with(scheme)) left.remove_prefix(scheme.size());
    if (right.starts_with(scheme)) right.remove_prefix(scheme.size());
    return left == right;
}

} // namespace PelicanStudio
