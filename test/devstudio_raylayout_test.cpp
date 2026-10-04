// RTL-2s (a): the Ray layout section's model (engine RTL-1a / RTL-1b) --
// get_ray_layout as the section reads it, Enable hit groups' plan (the default
// layout, every ray tracing task in the layout's form and the shaders beside
// the ones before), the tables' changes reaching every task, the bindings and
// the shaders, the Hit group assignments, and the Undo of an edit that
// rewrote procedural records.

#include "../src/devstudio/model/raylayoutmodel.hpp"
#include "../src/devstudio/model/renderedithistory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace PelicanStudio;
using Json = nlohmann::json;

namespace {

void write(const std::filesystem::path &path, const std::string &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << bytes;
}

std::string readFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

struct TempProject {
    std::filesystem::path root;
    TempProject() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_rtl2s_a_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "shaders");
    }
    ~TempProject() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

// Studio's CT-6s starting set, as far as Enable recognises it.
const std::string studio_raygen =
    "#version 460\n// ray_tracing.rgen: a ray tracing set Studio wrote (raygen + miss + closest hit).\n"
    "void main() { traceRayEXT(s, 0, 0xff, 0, 0, 0, o, 0.001, d, 1000.0, 0); }\n";
// Somebody's own raygen: two hit offsets, two misses, literal routing.
const std::string own_raygen = "#version 460\n#extension GL_EXT_ray_tracing : require\n"
                               "void main() {\n"
                               "    traceRayEXT(s, 0, 0xff, 0, 0, 0, o, 0.001, d, 1000.0, 0);\n"
                               "    traceRayEXT(s, 0, 0xff, 1u, 0, 1, o, 0.001, d, 1000.0, 1);\n"
                               "}\n";

Json renderDocuments() {
    Json root{{"rendering", Json::object()},
              {"nodes",
               Json::array({{{"id", "trace"},
                             {"op", "render.compute"},
                             {"parameters",
                              {{"ray_tracing",
                                {{"raygen", "project://shaders/ray_tracing"},
                                 {"miss", "project://shaders/ray_tracing"},
                                 {"closesthit", "project://shaders/ray_tracing"}}},
                               {"dispatch", {{"rays_from", {{"port", "color"}}}}}}}}})}};
    Json feature{{"compute_tasks",
                  Json::array({{{"name", "own_trace"},
                                {"ray_tracing",
                                 {{"raygen", "project://shaders/own"},
                                  {"miss", Json::array({"project://shaders/sky", "project://shaders/shadow"})},
                                  {"closesthit", Json::array({"project://shaders/hit", "project://shaders/hit"})}}}}})}};
    return {{"documents",
             Json::array({{{"reference", "passes/main_rendering_config.json"},
                           {"kind", "root"},
                           {"digest", std::string(64, 'a')},
                           {"content", root.dump()}},
                          {{"reference", "project://features/own.json"},
                           {"kind", "feature"},
                           {"digest", std::string(64, 'b')},
                           {"content", feature.dump()}}})}};
}

Json layoutAnswer(const Json &layout, const Json &bindings, const char *mode) {
    Json answer{{"source_digest", {{"algorithm", "sha256"}, {"hex", std::string(64, 'c')}}},
                {"mode", mode},
                {"layout", layout.is_null() ? Json(nullptr)
                                            : Json{{"reference", "project://passes/ray_layout.json"},
                                                   {"digest", std::string(64, 'd')},
                                                   {"document", layout}}},
                {"bindings", bindings.is_null() ? Json(nullptr)
                                                : Json{{"reference", "project://passes/ray_bindings.json"},
                                                       {"digest", std::string(64, 'e')},
                                                       {"document", bindings}}},
                {"selectable_materials",
                 Json::array({{{"label", "material"}, {"name", "material"}, {"selector", {{"document", "materials/material.material.json"}}}},
                              {{"label", "material_2"}, {"name", "material_2"}, {"selector", {{"document", "materials/material_2.material.json"}}}}})},
                {"selectable_procedural_assets", Json::array({{{"asset", "ball"}, {"record_fields", Json::object()}}})},
                {"unresolved_bindings", Json::array()}};
    return answer;
}

// The layout after Enable, with the tasks it put in its form.
Json enabledRoot() {
    auto documents = renderDocuments();
    auto root = Json::parse(documents["documents"][0]["content"].get<std::string>());
    root["nodes"][0]["parameters"]["ray_tracing"] = {
        {"raygen", "project://shaders/ray_tracing_layout"},
        {"traced_ray_types", Json::array({"primary"})},
        {"misses", {{"miss", "project://shaders/ray_tracing_layout"}}},
        {"hit_shaders", {{"primary", {{"default", {{"closesthit", "project://shaders/ray_tracing_layout"}}}}}}}};
    documents["documents"][0]["content"] = root.dump();
    documents["documents"].erase(1);
    return documents;
}

} // namespace

TEST_CASE("RTL-2s decision 1: get_ray_layout as the Ray layout section reads it", "[rtl2s][ray-layout]") {
    const auto none = parseRayLayoutRead(layoutAnswer(nullptr, nullptr, "none"));
    CHECK_FALSE(none.layout.named);
    CHECK_FALSE(none.restartPending());
    CHECK(none.source_digest == std::string(64, 'c'));
    REQUIRE(none.materials.size() == 2);
    CHECK(none.materials[1].selector == Json{{"document", "materials/material_2.material.json"}});
    // Written by Enable, the process still without one: waiting for the restart.
    const auto written = parseRayLayoutRead(layoutAnswer(defaultRayLayout(), nullptr, "none"));
    CHECK(written.layout.usable());
    CHECK(written.restartPending());
    CHECK(parseRayLayoutRead(layoutAnswer(defaultRayLayout(), nullptr, "layout")).restartPending() == false);
    // Decision 6: a layout that is not JSON keeps its error and text.
    auto broken = layoutAnswer(nullptr, nullptr, "layout");
    broken["layout"] = {{"reference", "project://passes/ray_layout.json"},
                        {"digest", std::string(64, 'd')},
                        {"document", nullptr},
                        {"text", "{ nope"},
                        {"error", {{"code", "render_document_syntax@1"}, {"message", "not JSON"}}}};
    const auto read_broken = parseRayLayoutRead(broken);
    CHECK(read_broken.layout.named);
    CHECK_FALSE(read_broken.layout.usable());
    CHECK(read_broken.layout.error_code == "render_document_syntax@1");
    CHECK(read_broken.layout.text == "{ nope");
    // The names' rule, red as typed.
    CHECK_FALSE(rayNameProblem(RayNameKind::group, "group_2", {"default"}));
    CHECK(rayNameProblem(RayNameKind::group, "default", {"default"}));
    CHECK(rayNameProblem(RayNameKind::miss, "a b", {}));
    CHECK(rayNameProblem(RayNameKind::ray_type, std::string(65, 'a'), {}));
    CHECK(rayNameProblem(RayNameKind::record_field, "geometry_index", {}));
    CHECK(rayFreeName("group", {"group", "group_2"}) == "group_3");
    // A refusal's document matched to the root's spelling.
    CHECK(sameRenderDocument("passes/main_rendering_config.json", "project://passes/main_rendering_config.json"));
}

TEST_CASE("RTL-2s decision 2: Enable hit groups writes the layout and every ray tracing task in its form in one "
          "edit_batch, beside the shaders before",
          "[rtl2s][ray-layout]") {
    TempProject project;
    write(project.root / "shaders/ray_tracing.rgen", studio_raygen);
    write(project.root / "shaders/ray_tracing.rmiss", "#version 460\nvoid main() {}\n");
    write(project.root / "shaders/ray_tracing.rchit", "#version 460\nvoid main() {}\n");
    write(project.root / "shaders/own.rgen", own_raygen);
    const auto read = parseRayLayoutRead(layoutAnswer(nullptr, nullptr, "none"));
    const auto tasks = rayTracingTasks(renderDocuments());
    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].kind == RayTracingTask::Kind::graph_node);
    CHECK(tasks[1].kind == RayTracingTask::Kind::feature);

    const auto plan = planEnableHitGroups(read, tasks, project.root);
    REQUIRE_FALSE(plan.problem);
    // Two closest hits and two misses in the feature's task: two ray types,
    // two misses; the default group and the engine record that reads normals.
    CHECK(plan.layout.at("ray_types") == Json::array({"primary", "ray_2"}));
    CHECK(rayLayoutMisses(plan.layout) == std::vector<std::string>{"miss", "miss_2"});
    CHECK(rayLayoutDefaultGroup(plan.layout) == "default");
    CHECK(rayLayoutEngineRecord(plan.layout) == std::vector<std::string>{"material_index", "geometry_index"});
    REQUIRE(plan.tasks.size() == 2);
    const auto &graph_rt = plan.tasks[0].second;
    CHECK(graph_rt.at("raygen") == "project://shaders/ray_tracing_layout");
    CHECK(graph_rt.at("hit_shaders") ==
          Json{{"primary", {{"default", {{"closesthit", "project://shaders/ray_tracing_layout"}}}}}});
    CHECK_FALSE(graph_rt.contains("closesthit"));
    const auto &feature_rt = plan.tasks[1].second;
    CHECK(feature_rt.at("traced_ray_types") == Json::array({"primary", "ray_2"}));
    CHECK(feature_rt.at("misses") == Json{{"miss", "project://shaders/sky"}, {"miss_2", "project://shaders/shadow"}});
    CHECK(feature_rt.at("hit_shaders").at("ray_2").at("default").at("closesthit") == "project://shaders/hit");
    CHECK(feature_rt.at("raygen") == "project://shaders/own_layout");
    // Studio's set replaced by the layout's (three new files), the own
    // raygen's literal routing rewritten into a file beside it.
    std::map<std::string, std::string> written;
    for (const auto &write_entry : plan.shaders) {
        CHECK_FALSE(write_entry.previous);
        written[write_entry.path.filename().string()] = write_entry.content;
    }
    CHECK(written.contains("ray_tracing_layout.rgen"));
    CHECK(written.contains("ray_tracing_layout.rmiss"));
    CHECK(written.at("ray_tracing_layout.rchit").find("pelicanRayFetchNormal") != std::string::npos);
    const auto &own = written.at("own_layout.rgen");
    CHECK(own.find("PELICAN_RAY_TYPE_primary, pelican_ray_type_count, PELICAN_MISS_miss,") != std::string::npos);
    CHECK(own.find("PELICAN_RAY_TYPE_ray_2, pelican_ray_type_count, PELICAN_MISS_miss_2,") != std::string::npos);
    CHECK(own.find("#include \"__pelican_ray_group_layout.glsl\"") != std::string::npos);
    // The files before are not written: the render history's Undo of the
    // batch puts the tasks back on them.
    CHECK(readFile(project.root / "shaders/own.rgen") == own_raygen);

    // One edit_batch: the layout, the graph's node, the feature's task.
    const auto request = rayLayoutRequest(read, tasks, plan);
    CHECK(request.method == "edit_batch");
    CHECK(request.params.at("base_source_digest") == std::string(64, 'c'));
    const auto &edits = request.params.at("edits");
    REQUIRE(edits.size() == 3);
    CHECK(edits[0].at("method") == "edit_ray_layout");
    CHECK_FALSE(edits[0].at("params").contains("base_layout_digest"));
    CHECK(edits[1].at("method") == "edit_compute_task");
    CHECK(edits[1].at("params").at("base_task_source_digest") == std::string(64, 'b'));
    CHECK(edits[2].at("method") == "edit_graph_document");
    const auto &op = edits[2].at("params").at("ops").at(0);
    CHECK(op.at("op") == "set_node_parameters");
    CHECK(op.at("node") == "trace");
    CHECK(op.at("parameters").at("dispatch") == Json{{"rays_from", {{"port", "color"}}}});
    // Not twice.
    CHECK(planEnableHitGroups(parseRayLayoutRead(layoutAnswer(plan.layout, nullptr, "layout")), tasks, project.root)
              .problem);

    // Disable: the layout's starting set goes back to the files it was
    // made from.
    for (const auto &write_entry : plan.shaders) write(write_entry.path, write_entry.content);
    const auto enabled = parseRayLayoutRead(layoutAnswer(defaultRayLayout(), nullptr, "layout"));
    const auto back = planDisableHitGroups(enabled, rayTracingTasks(enabledRoot()), project.root);
    REQUIRE_FALSE(back.problem);
    CHECK(back.remove_layout);
    REQUIRE(back.tasks.size() == 1);
    CHECK(back.tasks[0].second == Json{{"raygen", "project://shaders/ray_tracing"},
                                      {"miss", "project://shaders/ray_tracing"},
                                      {"closesthit", "project://shaders/ray_tracing"}});
    // With two hit groups it says what stays.
    auto two = defaultRayLayout();
    two["groups"].push_back({{"name", "group_2"}, {"type", "triangles"}});
    CHECK(planDisableHitGroups(parseRayLayoutRead(layoutAnswer(two, nullptr, "layout")), rayTracingTasks(enabledRoot()),
                               project.root)
              .problem);
}

TEST_CASE("RTL-2s decision 3: a hit group, a ray type, a miss and a record field reach every task, the bindings "
          "and the shaders",
          "[rtl2s][ray-layout]") {
    TempProject project;
    const auto tasks = rayTracingTasks(enabledRoot());
    const Json bindings{{"schema", "pelican.ray_bindings"},
                        {"version", 2},
                        {"bindings", Json::array({{{"material", {{"document", "materials/material_2.material.json"}}},
                                                   {"group", "default"},
                                                   {"record", Json::object()}}})}};
    const auto read = parseRayLayoutRead(layoutAnswer(defaultRayLayout(), bindings, "layout"));

    // + hit group: copied from the default group's column, or a closest hit
    // from the template.
    const auto copied = planAddGroup(read, tasks, project.root, "group_2", "triangles", RayFillMode::copy);
    REQUIRE_FALSE(copied.problem);
    CHECK(copied.tasks.at(0).second.at("hit_shaders").at("primary").at("group_2") ==
          Json{{"closesthit", "project://shaders/ray_tracing_layout"}});
    CHECK(copied.shaders.empty());
    const auto made = planAddGroup(read, tasks, project.root, "group_2", "triangles", RayFillMode::from_template);
    CHECK(made.tasks.at(0).second.at("hit_shaders").at("primary").at("group_2") ==
          Json{{"closesthit", "project://shaders/hit_group_2"}});
    REQUIRE(made.shaders.size() == 1);
    CHECK(made.shaders[0].path.filename() == "hit_group_2.rchit");
    const auto request = rayLayoutRequest(read, tasks, made);
    CHECK(request.method == "edit_batch");
    CHECK(request.params.at("edits").at(0).at("params").at("base_layout_digest") == std::string(64, 'd'));
    const auto procedural = planAddGroup(read, tasks, project.root, "balls", "procedural", RayFillMode::copy);
    CHECK(procedural.tasks.at(0).second.at("hit_shaders").at("primary").at("balls").contains("intersection"));
    CHECK(planAddGroup(read, tasks, project.root, "default", "triangles", RayFillMode::copy).problem);

    // + ray type: the row copied; + miss: copied.
    const auto shadow = planAddRayType(read, tasks, "shadow");
    CHECK(shadow.tasks.at(0).second.at("traced_ray_types") == Json::array({"primary", "shadow"}));
    CHECK(shadow.tasks.at(0).second.at("hit_shaders").at("shadow") ==
          shadow.tasks.at(0).second.at("hit_shaders").at("primary"));
    const auto miss = planAddMiss(read, tasks, project.root, "shadow_miss", RayFillMode::copy);
    CHECK(miss.tasks.at(0).second.at("misses").at("shadow_miss") == "project://shaders/ray_tracing_layout");

    // - hit group: its cells, and the assignments that pick it, counted.
    auto with_two = defaultRayLayout();
    with_two["groups"].push_back({{"name", "group_2"}, {"type", "triangles"}});
    auto bound = bindings;
    bound["bindings"][0]["group"] = "group_2";
    auto two_tasks_root = enabledRoot();
    {
        auto root = Json::parse(two_tasks_root["documents"][0]["content"].get<std::string>());
        root["nodes"][0]["parameters"]["ray_tracing"]["hit_shaders"]["primary"]["group_2"] = {
            {"closesthit", "project://shaders/hit_group_2"}};
        two_tasks_root["documents"][0]["content"] = root.dump();
    }
    const auto read_two = parseRayLayoutRead(layoutAnswer(with_two, bound, "layout"));
    const auto removed = planRemoveGroup(read_two, rayTracingTasks(two_tasks_root), "group_2");
    REQUIRE_FALSE(removed.problem);
    CHECK_FALSE(removed.tasks.at(0).second.at("hit_shaders").at("primary").contains("group_2"));
    CHECK(removed.bindings.at("bindings").empty());
    CHECK(removed.confirm.find("1 assignment(s)") != std::string::npos);
    CHECK(planRemoveGroup(read_two, tasks, "default").problem);
    CHECK(planRemoveRayType(read, tasks, "primary").problem);

    // Rename a ray type: the layout, the task and the shaders that name it.
    write(project.root / "shaders/a.rgen", "x = PELICAN_RAY_TYPE_primary; y = PELICAN_RAY_TYPE_primary_x;\n");
    const auto renamed = planRename(read, tasks, RayEntryKind::ray_type, "primary", "camera",
                                    rayProjectShaderFiles(project.root));
    REQUIRE_FALSE(renamed.problem);
    CHECK(renamed.layout.at("ray_types") == Json::array({"camera"}));
    CHECK(renamed.tasks.at(0).second.at("traced_ray_types") == Json::array({"camera"}));
    CHECK(renamed.tasks.at(0).second.at("hit_shaders").contains("camera"));
    REQUIRE(renamed.shaders.size() == 1);
    CHECK(renamed.shaders[0].content == "x = PELICAN_RAY_TYPE_camera; y = PELICAN_RAY_TYPE_primary_x;\n");
    CHECK(renamed.confirm.find("a.rgen (1)") != std::string::npos);
    // Rename a group: the bindings follow.
    const auto group_renamed = planRename(read_two, rayTracingTasks(two_tasks_root), RayEntryKind::group, "group_2",
                                          "glass", {});
    CHECK(group_renamed.bindings.at("bindings").at(0).at("group") == "glass");

    // Record fields: every material row and procedural asset, at 0.
    const auto tint = planAddRecordField(read, "tint", "vec3");
    REQUIRE_FALSE(tint.problem);
    CHECK(tint.bindings.at("bindings").at(0).at("record") == Json{{"tint", Json::array({0.0, 0.0, 0.0})}});
    CHECK(tint.procedural_records == Json{{"ball", {{"tint", Json::array({0.0, 0.0, 0.0})}}}});
    CHECK(rayLayoutRequest(read, tasks, tint).method == "edit_ray_layout");
}

TEST_CASE("RTL-2s decision 4: Hit group assignments from the selectable materials", "[rtl2s][ray-layout]") {
    const auto read = parseRayLayoutRead(layoutAnswer(defaultRayLayout(), nullptr, "layout"));
    const Json selector{{"document", "materials/material_2.material.json"}};
    const auto assigned = planAssignMaterial(read, selector, "default");
    REQUIRE_FALSE(assigned.problem);
    CHECK(assigned.bindings ==
          Json{{"schema", "pelican.ray_bindings"},
               {"version", 2},
               {"bindings", Json::array({{{"material", selector}, {"group", "default"}, {"record", Json::object()}}})}});
    // No bindings document yet: edit_ray_layout makes it beside the root
    // (no base digest).
    const auto request = rayLayoutRequest(read, {}, assigned);
    CHECK(request.method == "edit_ray_layout");
    CHECK_FALSE(request.params.contains("base_bindings_digest"));
    CHECK(planAssignMaterial(read, selector, "nothing").problem);
    // The default entry takes the row out.
    const auto read_bound = parseRayLayoutRead(layoutAnswer(defaultRayLayout(), assigned.bindings, "layout"));
    CHECK(planAssignMaterial(read_bound, selector, "").bindings.at("bindings").empty());
    CHECK(rayLayoutRequest(read_bound, {}, planAssignMaterial(read_bound, selector, ""))
              .params.at("base_bindings_digest") == std::string(64, 'e'));
    // A procedural asset's row carries no record.
    CHECK(planAssignProcedural(read_bound, "ball", "default").bindings.at("bindings").at(1) ==
          Json{{"procedural", "ball"}, {"group", "default"}});
    CHECK(raySelectorLabel(read, selector) == "material_2");
}

TEST_CASE("RTL-2s decision 7: an edit that rewrites procedural records is taken back by sending them again",
          "[rtl2s][ray-layout]") {
    auto layout = defaultRayLayout();
    const auto before = parseRayLayoutRead(layoutAnswer(layout, nullptr, "layout"));
    const auto plan = planAddRecordField(before, "radius", "f32");
    const auto request = rayLayoutRequest(before, {}, plan);
    // Not the render history's: its restore cannot take asset data back.
    CHECK(RenderEditHistory::rewritesProceduralRecords(request.method, request.params));
    CHECK(RenderEditHistory::rewritesProceduralRecords(
        "edit_batch", {{"edits", Json::array({{{"method", "edit_ray_layout"}, {"params", request.params}}})}}));
    CHECK_FALSE(RenderEditHistory::rewritesProceduralRecords("edit_ray_layout", {{"layout", layout}}));
    CHECK(RenderEditHistory::recordsMethod("edit_ray_layout"));
    auto now_answer = layoutAnswer(plan.layout, nullptr, "layout");
    now_answer["selectable_procedural_assets"][0]["record_fields"] = {{"radius", 0.0}};
    const auto now = parseRayLayoutRead(now_answer);
    const auto back = planPutBack(before, now);
    CHECK(back.layout == layout);
    CHECK(back.procedural_records == Json{{"ball", Json::object()}});
    CHECK(rayLayoutRequest(now, {}, back).method == "edit_ray_layout");
}
