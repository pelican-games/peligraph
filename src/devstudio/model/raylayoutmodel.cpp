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

std::string rayLayoutRaygenTemplate(std::string_view name, std::string_view ray_type, std::string_view miss) {
    const std::string port = "color";
    return "#version 460\n"
           "#extension GL_GOOGLE_include_directive : enable\n"
           "#extension GL_EXT_ray_tracing : require\n"
           "\n"
           "// " + std::string{name} + ".rgen: a ray tracing set Studio wrote for a ray layout.\n"
           "// One ray per pixel of the image it writes (the task's dispatch.rays_from),\n"
           "// from the camera through that pixel. The hit group the object is assigned\n"
           "// to answers with the colour; the miss shader with the sky.\n"
           "\n"
           "// The ray layout's names: PELICAN_RAY_TYPE_<ray type>, PELICAN_MISS_<miss>\n"
           "// and pelican_ray_type_count, generated from the project's ray layout.\n"
           "#include \"__pelican_ray_group_layout.glsl\"\n"
           "#include \"pelican_sets.glsl\"\n"
           "#include \"pelican_frame.glsl\"\n"
           "\n"
           "layout(rgba16f, set = PELICAN_SET_PASS_INPUT, binding = 0)\n"
           "    writeonly uniform image2D pelican_resource_" + port + ";\n"
           "\n"
           "layout(set = PELICAN_SET_FRAME, binding = PELICAN_RAY_QUERY_TLAS_BINDING)\n"
           "    uniform accelerationStructureEXT pelicanRayTracingScene;\n"
           "\n"
           "// The payload: rgb is the colour, w is 1 on a hit and 0 when the miss ran.\n"
           "layout(location = 0) rayPayloadEXT vec4 hit;\n"
           "\n"
           "void main() {\n"
           "    vec2 pixel = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);\n"
           "    vec2 ndc = pixel / vec2(gl_LaunchSizeEXT.xy) * 2.0 - 1.0;\n"
           "    vec4 target = inverse(pelicanFrame.projection) * vec4(ndc, 0.5, 1.0);\n"
           "    vec3 direction = normalize((inverse(pelicanFrame.view) *\n"
           "                                vec4(normalize(target.xyz / target.w), 0.0)).xyz);\n"
           "    vec3 origin = (inverse(pelicanFrame.view) * vec4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
           "    hit = vec4(0.0);\n"
           "    // A second ray type (a shadow ray, say) is traced the same way with its\n"
           "    // own PELICAN_RAY_TYPE_ and PELICAN_MISS_ names.\n"
           "    traceRayEXT(pelicanRayTracingScene, gl_RayFlagsOpaqueEXT, 0xff,\n"
           "                PELICAN_RAY_TYPE_" + std::string{ray_type} + ", pelican_ray_type_count,\n"
           "                PELICAN_MISS_" + std::string{miss} + ", origin, 0.001, direction, 10000.0, 0);\n"
           "    vec3 color = hit.w > 0.5 ? hit.rgb\n"
           "                             : mix(vec3(0.04, 0.05, 0.07), vec3(0.18, 0.26, 0.36),\n"
           "                                   clamp(direction.y * 0.5 + 0.5, 0.0, 1.0));\n"
           "    imageStore(pelican_resource_" + port + ", ivec2(gl_LaunchIDEXT.xy), vec4(color, 1.0));\n"
           "}\n";
}

std::string rayLayoutMissTemplate(std::string_view name) {
    return "#version 460\n"
           "#extension GL_EXT_ray_tracing : require\n"
           "\n"
           "// " + std::string{name} + ".rmiss: runs where a ray hits nothing.\n"
           "layout(location = 0) rayPayloadInEXT vec4 hit;\n"
           "\n"
           "void main() {\n"
           "    hit = vec4(0.0);\n"
           "}\n";
}

std::array<double, 3> rayGroupTint(std::size_t index) {
    static const std::array<std::array<double, 3>, 6> tints{{{1.0, 0.55, 0.1},
                                                             {0.25, 0.85, 0.3},
                                                             {0.3, 0.5, 1.0},
                                                             {0.95, 0.25, 0.6},
                                                             {0.95, 0.9, 0.2},
                                                             {0.3, 0.9, 0.9}}};
    return tints[index % tints.size()];
}

namespace {
std::string vec3Text(const std::array<double, 3> &value) {
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(2);
    text << "vec3(" << value[0] << ", " << value[1] << ", " << value[2] << ")";
    return text.str();
}
} // namespace

std::string rayLayoutClosestHitTemplate(std::string_view name, bool engine_record,
                                        const std::array<double, 3> &tint) {
    std::string text =
        "#version 460\n"
        "#extension GL_GOOGLE_include_directive : enable\n"
        "#extension GL_EXT_ray_tracing : require\n";
    if (engine_record)
        text += "#extension GL_EXT_nonuniform_qualifier : require\n"
                "#extension GL_EXT_buffer_reference2 : require\n";
    text += "\n// " + std::string{name} +
            ".rchit: runs at the nearest surface a ray hits, for the objects\n"
            "// assigned to this hit group. It answers with the surface's normal as a\n"
            "// colour, tinted with the group's colour.\n"
            "#include \"__pelican_ray_group_layout.glsl\"\n"
            "\n"
            "layout(location = 0) rayPayloadInEXT vec4 hit;\n"
            "\n"
            "// This hit group's colour.\n"
            "const vec3 GROUP_TINT = " + vec3Text(tint) + ";\n\n";
    if (engine_record) {
        text += "hitAttributeEXT vec2 hit_attribute;\n"
                "// The engine's record: which geometry and material the ray hit.\n"
                "layout(shaderRecordEXT, std430) buffer HitRecord {\n"
                "    PELICAN_RAY_ENGINE_RECORD_MEMBERS\n"
                "} hit_record;\n"
                "\n"
                "void main() {\n"
                "    vec3 object_normal = pelicanRayFetchNormal(hit_record.geometry_index,\n"
                "                                               gl_PrimitiveID, hit_attribute);\n"
                "    vec3 normal = normalize(transpose(mat3(gl_WorldToObjectEXT)) * object_normal);\n"
                "    hit = vec4((normal * 0.5 + 0.5) * GROUP_TINT, 1.0);\n"
                "}\n";
    } else {
        text += "void main() {\n"
                "    // Without an engine record the normal cannot be read: the depth shades it.\n"
                "    hit = vec4(GROUP_TINT * clamp(1.5 - gl_HitTEXT * 0.1, 0.2, 1.0), 1.0);\n"
                "}\n";
    }
    return text;
}

std::string rayLayoutIntersectionTemplate(std::string_view name) {
    return "#version 460\n"
           "#extension GL_EXT_ray_tracing : require\n"
           "\n"
           "// " + std::string{name} + ".rint: a sphere of radius 0.5 about the geometry's origin.\n"
           "hitAttributeEXT vec2 hit_attribute;\n"
           "\n"
           "void main() {\n"
           "    vec3 origin = gl_ObjectRayOriginEXT;\n"
           "    vec3 direction = gl_ObjectRayDirectionEXT;\n"
           "    float a = dot(direction, direction);\n"
           "    float b = dot(origin, direction);\n"
           "    float c = dot(origin, origin) - 0.25;\n"
           "    float d = b * b - a * c;\n"
           "    if (d < 0.0) return;\n"
           "    float t = (-b - sqrt(d)) / a;\n"
           "    if (t < gl_RayTminEXT || t > gl_RayTmaxEXT) t = (-b + sqrt(d)) / a;\n"
           "    if (t >= gl_RayTminEXT && t <= gl_RayTmaxEXT) {\n"
           "        hit_attribute = vec2(0.0);\n"
           "        reportIntersectionEXT(t, 0u);\n"
           "    }\n"
           "}\n";
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
            const auto probe_name = parts.stem + "_layout";
            const std::vector<std::pair<std::string, std::string>> wanted{
                {"raygen", rayLayoutRaygenTemplate(probe_name, ray_types.front(), misses.front())},
                {"miss", rayLayoutMissTemplate(probe_name)},
                {"closesthit", rayLayoutClosestHitTemplate(probe_name, true, {1.0, 1.0, 1.0})}};
            auto stem = freshStem(root, parts, "layout", wanted);
            parts.stem = stem;
            parts.extension.clear();
            const auto reference = joinReference(parts);
            const std::vector<std::pair<std::string, std::string>> contents{
                {"raygen", rayLayoutRaygenTemplate(stem, ray_types.front(), misses.front())},
                {"miss", rayLayoutMissTemplate(stem)},
                {"closesthit", rayLayoutClosestHitTemplate(stem, true, {1.0, 1.0, 1.0})}};
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
        Json converted = rt;
        converted.erase("miss");
        converted.erase("closesthit");
        converted["raygen"] = raygen;
        Json traced = Json::array();
        Json hit_shaders = Json::object();
        for (std::size_t k = 0; k < task_hits.size(); ++k) {
            traced.push_back(ray_types[k]);
            hit_shaders[ray_types[k]] = {{group, {{"closesthit", task_hits[k]}}}};
        }
        Json miss_map = Json::object();
        for (std::size_t j = 0; j < misses.size(); ++j)
            miss_map[misses[j]] = task_misses[std::min(j, task_misses.size() - 1)];
        converted["traced_ray_types"] = traced;
        converted["misses"] = miss_map;
        converted["hit_shaders"] = hit_shaders;
        plan.tasks.emplace_back(index, std::move(converted));
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
                             const std::string &name) {
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
    // Each task traces it too, its row copied from the first ray type it
    // traces.
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        auto traced = arrayAt(rt, "traced_ray_types");
        const auto first = traced.empty() ? std::string{} : traced.front().get<std::string>();
        traced.push_back(name);
        rt["traced_ray_types"] = traced;
        rt["hit_shaders"][name] = objectAt(objectAt(rt, "hit_shaders"), first.c_str());
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
    Json cell = nullptr;
    if (mode == RayFillMode::from_template) {
        const bool procedural = type == "procedural";
        const auto stem = templateStem(root, "hit_" + name,
                                       procedural ? std::vector<std::string>{".rchit", ".rint"}
                                                  : std::vector<std::string>{".rchit"});
        const auto tint = rayGroupTint(groups.size() - 1);
        addWrite(plan, root / "shaders" / (stem + ".rchit"),
                 rayLayoutClosestHitTemplate(stem, !procedural && !rayLayoutEngineRecord(layout).empty(), tint));
        cell = {{"closesthit", "project://shaders/" + stem}};
        if (procedural) {
            addWrite(plan, root / "shaders" / (stem + ".rint"), rayLayoutIntersectionTemplate(stem));
            cell["intersection"] = "project://shaders/" + stem;
        }
        plan.notes.push_back("The closest hit of " + name + " is shaders/" + stem + ".rchit.");
    }
    for (const auto index : layoutTasks(tasks)) {
        auto rt = tasks[index].rayTracing();
        for (const auto &traced : arrayAt(rt, "traced_ray_types")) {
            if (!traced.is_string()) continue;
            const auto type_name = traced.get<std::string>();
            auto row = objectAt(objectAt(rt, "hit_shaders"), type_name.c_str());
            row[name] = cell.is_null() ? objectAt(row, source.c_str()) : cell;
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
        addWrite(plan, root / "shaders" / (stem + ".rmiss"), rayLayoutMissTemplate(stem));
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

RayLayoutPlan planAddRecordField(const RayLayoutRead &read, const std::string &name, const std::string &type) {
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
    return plan;
}

RayLayoutPlan planRemoveRecordField(const RayLayoutRead &read, const std::string &name) {
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
    return plan;
}

RayLayoutPlan planSetEngineRecord(const RayLayoutRead &read, const std::vector<std::string> &fields) {
    RayLayoutPlan plan;
    plan.label = "Engine record";
    if (!layoutReady(read, plan)) return plan;
    auto layout = layoutOf(read);
    if (fields.empty())
        layout.erase("engine_record");
    else
        layout["engine_record"] = fields;
    plan.layout = layout;
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
