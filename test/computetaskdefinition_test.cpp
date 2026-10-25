// RC-3c2b1: a compute task's execution values are read once, by
// parseRenderGraph, into the IR's ComputeNodeInfo; core makes the
// execution's ComputeTaskDefinition from the final IR
// (computeTaskDefinitionsOf). D2: what the old compute task parser refused is
// refused on the new path, by name; what it accepted by mistake (an unknown
// dispatch key, a count that wrapped or was truncated, an unknown axis) is
// refused now; what it rightly accepted is accepted with the same values.
#include "compute_task_test_support.hpp"
#include "render_graph_ir_test_support.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/project/raytracingrefusal.hpp"
#include "../src/project/rayrecordabi.hpp"
#include "../src/project/rendergraphir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

Json taskConfig(Json task) {
    return Json{{"render_targets",
                 Json::array({Json{{"name", "image"},
                                   {"extent_scale", 1.0},
                                   {"format", "R16G16B16A16_SFLOAT"},
                                   {"usage", Json::array({"STORAGE", "SAMPLED"})}}})},
                {"buffers", Json::array({Json{{"name", "args"}, {"size", 32}, {"command_layout", "compute_dispatch"}},
                                         Json{{"name", "values"}, {"size", 64}}})},
                {"compute_tasks", Json::array({std::move(task)})}};
}

Json computeTask(Json dispatch) {
    Json task{{"name", "task"},
              {"shader", "shaders/task"},
              {"reads", Json::array({"values"})},
              {"writes", Json::array({"image"})},
              {"resource_ports",
               {{"target", {{"resource", "image"}, {"access", "storage"}}},
                {"data", {{"resource", "values"}, {"kind", "buffer"}, {"element", "uint"}, {"access", "storage"}}}}}};
    if (!dispatch.is_null()) task["dispatch"] = std::move(dispatch);
    return task;
}

Json rayTask(Json dispatch) {
    Json task{{"name", "trace"},
              {"ray_tracing", {{"raygen", "shaders/trace"}, {"miss", "shaders/trace"}, {"closesthit", "shaders/trace"}}},
              {"writes", Json::array({"image"})},
              {"resource_ports", {{"target", {{"resource", "image"}, {"access", "storage"}}}}}};
    if (!dispatch.is_null()) task["dispatch"] = std::move(dispatch);
    return task;
}

ComputeDispatchDefinition dispatchOf(const Json &task) {
    const auto tasks = TestSupport::computeTaskDefinitionsOf(taskConfig(task));
    REQUIRE(tasks.size() == 1);
    return tasks.front().dispatch;
}

// Records as hex (Catch prints them).
std::vector<std::string> hexOf(const std::vector<std::vector<std::byte>> &records) {
    static constexpr char digits[] = "0123456789abcdef";
    std::vector<std::string> result;
    for (const auto &record : records) {
        std::string text;
        for (const auto byte : record) {
            text.push_back(digits[(static_cast<unsigned>(byte) >> 4) & 15]);
            text.push_back(digits[static_cast<unsigned>(byte) & 15]);
        }
        result.push_back(std::move(text));
    }
    return result;
}

void requireRayRefusal(const std::function<void()> &call, std::string_view code, const Json &fields) {
    try {
        call();
    } catch (const RayTracingRefusal &refusal) {
        INFO(refusal.what());
        CHECK(refusal.code() == std::string{code});
        CHECK(refusal.fields() == fields);
        return;
    } catch (const std::exception &error) {
        FAIL("refused otherwise: " << error.what());
    }
    FAIL("expected " << code);
}

} // namespace

TEST_CASE("RC-3c2b1 D2 the dispatch: every form's keys, exclusions and integer ranges",
          "[rc3c2b1][compute][dispatch][refusal]") {
    SECTION("accepted, with the values the task parser gave") {
        CHECK(dispatchOf(computeTask(nullptr)) == ComputeDispatchDefinition{});
        const auto groups = dispatchOf(computeTask({{"groups", Json::array({2, 3, 4})}}));
        CHECK(std::array{groups.groups_x, groups.groups_y, groups.groups_z} == std::array<std::uint32_t, 3>{2, 3, 4});
        const auto axes = dispatchOf(computeTask({{"groups", {{"x", 2}, {"z", 3}}}}));
        CHECK(std::array{axes.groups_x, axes.groups_y, axes.groups_z} == std::array<std::uint32_t, 3>{2, 1, 3});
        const auto largest = dispatchOf(computeTask({{"groups", Json::array({4294967295ULL, 1, 1})}}));
        CHECK(largest.groups_x == 4294967295U);
        CHECK(dispatchOf(computeTask({{"groups_from", {{"port", "target"}}}})).groups_from ==
              std::optional{ComputeImageExtentDispatchDefinition{"target"}});
        CHECK(dispatchOf(computeTask({{"indirect", {{"buffer", "args"}, {"offset", 12}}}})).indirect == std::optional{ComputeIndirectDispatchDefinition{"args", 12}});
        CHECK(dispatchOf(computeTask({{"indirect", {{"buffer", "args"}}}})).indirect ==
              std::optional{ComputeIndirectDispatchDefinition{"args", 0}});
        CHECK(dispatchOf(rayTask({{"rays", Json::array({4, 5, 6})}})).rays ==
              std::optional{std::array<std::uint32_t, 3>{4, 5, 6}});
        CHECK(dispatchOf(rayTask({{"rays_from", {{"port", "target"}}}})).rays_from ==
              std::optional{ComputeImageExtentDispatchDefinition{"target"}});
    }
    SECTION("refused by name (the old parser's sentence, or what it accepted by mistake)") {
        struct Case {
            Json dispatch;
            std::string field;
            std::string reason;
        };
        const std::vector<Case> cases{
            // Accepted by mistake before: an unknown key fell back to one
            // group; a count wrapped (negative) or was truncated (past
            // uint32); an unknown axis and an extra key were ignored.
            {{{"gropus", Json::array({8, 1, 1})}}, "dispatch.gropus", "unknown_field"},
            {{{"groups", Json::array({-1, 1, 1})}}, "dispatch.groups", "requires_positive_uint32"},
            {{{"groups", Json::array({4294967296ULL, 1, 1})}}, "dispatch.groups", "requires_positive_uint32"},
            {{{"groups", {{"w", 2}}}}, "dispatch.groups.w", "unknown_axis"},
            {{{"groups", {{"x", -2}}}}, "dispatch.groups.x", "requires_positive_uint32"},
            {{{"groups", {{"x", "2"}}}}, "dispatch.groups.x", "requires_positive_uint32"},
            {{{"groups", Json::array({1, 1, 1})}, {"extra", 1}}, "dispatch.extra", "unknown_field"},
            {{{"indirect", {{"buffer", "args"}}}, {"extra", 1}}, "dispatch.extra", "unknown_field"},
            {{{"indirect", {{"buffer", "args"}, {"offset", -4}}}}, "dispatch.indirect.offset",
             "requires_unsigned_integer"},
            {{{"indirect", {{"buffer", "args"}, {"stride", 4}}}}, "dispatch.indirect.stride", "unknown_field"},
            // Refused before, without a name.
            {{{"groups", Json::array({0, 1, 1})}}, "dispatch.groups", "requires_positive_uint32"},
            {{{"groups", Json::array({1.5, 1, 1})}}, "dispatch.groups", "requires_positive_uint32"},
            {{{"groups", Json::array({1, 1})}}, "dispatch.groups", "requires_three_entries"},
            {{{"groups", "8"}}, "dispatch.groups", "not_an_array_or_object"},
            {{{"groups", Json::array({1, 1, 1})}, {"groups_from", {{"port", "target"}}}}, "dispatch.groups",
             "combined_with_groups_from"},
            {{{"local_size", 64}}, "dispatch.local_size", "declared_by_the_shader"},
            {{{"groups_from", "target"}}, "dispatch.groups_from", "not_an_object"},
            {{{"groups_from", {{"port", "target"}, {"axis", "x"}}}}, "dispatch.groups_from.axis", "unknown_field"},
            {{{"groups_from", Json::object()}}, "dispatch.groups_from.port", "requires_string"},
            {{{"groups_from", {{"port", ""}}}}, "dispatch.groups_from.port", "empty"},
            {{{"groups_from", {{"port", "missing"}}}}, "dispatch.groups_from.port", "unknown_resource_port"},
            {{{"groups_from", {{"port", "data"}}}}, "dispatch.groups_from.port", "not_an_image"},
            {{{"indirect", {{"buffer", "args"}}}, {"groups", Json::array({1, 1, 1})}}, "dispatch.indirect",
             "combined_with_groups"},
            {{{"indirect", {{"buffer", "args"}, {"offset", 2}}}}, "dispatch.indirect.offset", "not_4_byte_aligned"},
            {{{"indirect", {{"buffer", ""}}}}, "dispatch.indirect.buffer", "empty"},
            {"8", "dispatch", "not_an_object"},
        };
        for (const auto &entry : cases) {
            INFO(entry.dispatch.dump());
            TestSupport::requireNamedRefusal(
                [&] { (void)TestSupport::renderGraphOf(taskConfig(computeTask(entry.dispatch))); },
                "render_graph.compute_task_invalid@1",
                {{"node", "task"}, {"field", entry.field}, {"reason", entry.reason}});
        }
        // A task without its shader plans; the execution refuses it.
        auto shaderless = computeTask(nullptr);
        shaderless.erase("shader");
        CHECK_NOTHROW(TestSupport::renderGraphOf(taskConfig(shaderless)));
        TestSupport::requireNamedRefusal(
            [&] { (void)TestSupport::computeTaskDefinitionsOf(taskConfig(shaderless)); },
            "render_graph.compute_task_invalid@1",
            {{"node", "task"}, {"field", "shader"}, {"reason", "requires_string"}});
        shaderless["shader"] = 3;
        TestSupport::requireNamedRefusal([&] { (void)TestSupport::renderGraphOf(taskConfig(shaderless)); },
                                         "render_graph.compute_task_invalid@1",
                                         {{"node", "task"}, {"field", "shader"}, {"reason", "requires_string"}});
    }
    SECTION("a ray tracing task's launch keeps its ray tracing refusals") {
        const auto refused = [](Json dispatch, std::string_view code) {
            INFO(dispatch.dump());
            requireRayRefusal([&] { (void)TestSupport::renderGraphOf(taskConfig(rayTask(std::move(dispatch)))); },
                              code, {{"task", "trace"}});
        };
        // Without a launch: the execution's refusal (a planning revision
        // may leave it out).
        for (const auto &launchless : std::vector<Json>{nullptr, Json::object(), {{"groups", Json::array({1, 1, 1})}}}) {
            INFO(launchless.dump());
            CHECK_NOTHROW(TestSupport::renderGraphOf(taskConfig(rayTask(launchless))));
            requireRayRefusal([&] { (void)TestSupport::computeTaskDefinitionsOf(taskConfig(rayTask(launchless))); },
                              "pelican.ray_tracing.trace_extent_required@1", {{"task", "trace"}});
        }
        for (const auto &bad : std::vector<Json>{Json::array({0, 1, 1}), Json::array({-1, 1, 1}),
                                                 Json::array({1.5, 1, 1}), Json::array({4294967296ULL, 1, 1}),
                                                 Json::array({1, 1}), "2,3,4"}) {
            refused({{"rays", bad}}, "pelican.ray_tracing.invalid_trace_dispatch@1");
        }
        refused({{"rays", Json::array({1, 1, 1})}, {"rays_from", {{"port", "target"}}}},
                "pelican.ray_tracing.invalid_trace_dispatch@1");
        refused({{"rays_from", {{"port", 3}}}}, "pelican.ray_tracing.invalid_trace_dispatch@1");
        refused({{"rays_from", {{"port", ""}}}}, "pelican.ray_tracing.invalid_trace_dispatch@1");
        refused({{"indirect", {{"buffer", "args"}}}, {"rays", Json::array({1, 1, 1})}},
                "pelican.ray_tracing.invalid_trace_dispatch@1");
        requireRayRefusal(
            [] { (void)TestSupport::renderGraphOf(taskConfig(computeTask({{"rays_from", {{"port", "target"}}}}))); },
            "pelican.ray_tracing.rays_from_requires_pipeline@1", {{"task", "task"}});
        requireRayRefusal(
            [] { (void)TestSupport::renderGraphOf(taskConfig(computeTask({{"rays", Json::array({1, 1, 1})}}))); },
            "pelican.ray_tracing.invalid_trace_dispatch@1", {{"task", "task"}});
    }
}

TEST_CASE("RC-3c2b1 a graph declares no compute tasks of its own",
          "[rc3c2b1][compute][refusal]") {
    // Compose, a loop and Studio write the config's tasks; a graph's own were
    // never registered. Refused by name, as a graph's images and buffers.
    const Json config{
        {"render_targets", Json::array({Json{{"name", "image"},
                                             {"extent_scale", 1.0},
                                             {"format", "R16G16B16A16_SFLOAT"},
                                             {"usage", Json::array({"STORAGE", "SAMPLED"})}}})},
        {"rendering_passes",
         Json::array({Json{{"name", "main"},
                           {"passes", Json::array()},
                           {"compute_tasks", Json::array({computeTask({{"groups", Json::array({1, 1, 1})}})})}}})}};
    TestSupport::requireNamedRefusal([&] { (void)TestSupport::renderGraphOf(config); },
                                     "render_graph.graph_declaration_unsupported@1",
                                     {{"graph", "main"}, {"field", "compute_tasks"}});
}

TEST_CASE("RC-3c2b1 callable records: the inline and the table's are each checked, the table's applied once",
          "[rc3c2b1][compute][ray-tracing][callable]") {
    auto task = rayTask({{"rays", Json::array({1, 1, 1})}});
    task["ray_tracing"]["callable"] = Json::array({"shaders/a", "shaders/b"});
    task["ray_tracing"]["callable_record"] = Json::array({Json{{"name", "value"}, {"type", "u32"}}});
    task["ray_tracing"]["callable_records"] = Json::array({Json{{"value", 1}}, Json{{"value", 2}}});
    auto config = taskConfig(task);
    const auto packed = [](std::uint32_t value) {
        std::vector<std::byte> bytes(4);
        writeRayRecordU32(bytes, value);
        return bytes;
    };
    const auto records_of = [](const RenderGraphIR &ir) {
        return std::get<ComputeNodeInfo>(sharedComputeTasksOf(ir).front().node->info).ray_tracing->callable_records;
    };
    // Inline: packed by the parse.
    CHECK(hexOf(records_of(TestSupport::renderGraphOf(config))) == hexOf({packed(1), packed(2)}));
    // The table replaces them, applied once (every graph's copy).
    config["ray_task_callable_records"] = {{"trace", Json::array({Json{{"value", 7}}, Json{{"value", 8}}})}};
    const auto parsed = TestSupport::renderGraphOf(config);
    CHECK(hexOf(records_of(parsed)) == hexOf({packed(1), packed(2)}));
    const auto before = renderGraphApplicationCounts();
    const auto applied = applyRayTaskTables(parsed);
    CHECK(renderGraphApplicationCounts().callable_record_entries - before.callable_record_entries == 1);
    CHECK(hexOf(records_of(applied)) == hexOf({packed(7), packed(8)}));
    const auto tasks = Pelican::computeTaskDefinitionsOf(applied, nullptr);
    REQUIRE(tasks.front().ray_tracing->callable_record_layout);
    CHECK(hexOf(tasks.front().ray_tracing->callable_records) == hexOf(records_of(applied)));
    // An invalid inline record is refused although the table replaces it.
    auto invalid_inline = config;
    invalid_inline["compute_tasks"][0]["ray_tracing"]["callable_records"][1] = {{"value", -1}};
    CHECK_THROWS_WITH(TestSupport::renderGraphOf(invalid_inline),
                      Catch::Matchers::ContainsSubstring("pelican.ray_binding_record_invalid@1") &&
                          Catch::Matchers::ContainsSubstring("requires u32"));
    // The table's own: not an object (the parse), a task it cannot name, a
    // task without a schema, a record set that does not fit the schema.
    auto table = config;
    table["ray_task_callable_records"] = Json::array();
    TestSupport::requireNamedRefusal(
        [&] { (void)TestSupport::renderGraphOf(table); }, "render_graph.compute_task_invalid@1",
        {{"field", "ray_task_callable_records"}, {"reason", "not_an_object"}});
    table["ray_task_callable_records"] = {{"ghost", Json::array({Json{{"value", 1}}})}};
    TestSupport::requireNamedRefusal(
        [&] { (void)applyRayTaskTables(TestSupport::renderGraphOf(table)); }, "render_graph.compute_task_invalid@1",
        {{"node", "ghost"}, {"field", "ray_task_callable_records"}, {"reason", "unknown_task"}});
    auto schemaless = taskConfig(rayTask({{"rays", Json::array({1, 1, 1})}}));
    schemaless["ray_task_callable_records"] = {{"trace", Json::array({Json{{"value", 1}}})}};
    TestSupport::requireNamedRefusal(
        [&] { (void)applyRayTaskTables(TestSupport::renderGraphOf(schemaless)); },
        "render_graph.compute_task_invalid@1",
        {{"node", "trace"}, {"field", "ray_task_callable_records"}, {"reason", "task_declares_no_callable_record"}});
    for (const auto &bad : std::vector<Json>{Json::array(), Json::array({Json{{"value", 1}}}),
                                             Json::array({Json{{"value", 1}}, Json{{"value", -1}}}),
                                             Json::array({Json{{"value", 1}}, Json{{"other", 1}}})}) {
        INFO(bad.dump());
        table["ray_task_callable_records"] = {{"trace", bad}};
        CHECK_THROWS_WITH(applyRayTaskTables(TestSupport::renderGraphOf(table)),
                          Catch::Matchers::ContainsSubstring("pelican.ray_tracing.callable_records_invalid@1") ||
                              Catch::Matchers::ContainsSubstring("pelican.ray_binding_record_invalid@1"));
    }
}

TEST_CASE("RC-3c2b1 the definitions are the final IR's, the ray tracing refusals name the authored task",
          "[rc3c2b1][compute][xr]") {
    auto task = rayTask({{"rays", Json::array({1, 1, 1})}});
    task["after"] = Json::array({"first"});
    auto config = taskConfig(task);
    config["compute_tasks"].insert(config["compute_tasks"].begin(),
                                   Json{{"name", "first"}, {"shader", "shaders/first"}, {"writes", Json::array({"values"})}});
    const auto ir = namespaceSharedComputeTasks(TestSupport::renderGraphOf(config), "#xr");
    CHECK(ir.authored_shared_compute_tasks == std::vector<std::string>{"first", "trace"});
    const auto tasks = Pelican::computeTaskDefinitionsOf(ir, nullptr);
    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].name == "first#xr");
    CHECK(tasks[1].name == "trace#xr");
    // The final edges (renamed); reads, writes and ports from the uses.
    CHECK(tasks[1].after == std::vector<std::string>{"first#xr"});
    CHECK(tasks[1].writes == std::vector<std::string>{"image"});
    REQUIRE(tasks[1].resource_ports.size() == 1);
    CHECK(tasks[1].resource_ports.front().name == "target");
    CHECK(tasks[0].writes == std::vector<std::string>{"values"});
    // The form without a ray group layout misses its miss: refused by the
    // name the author gave the task.
    auto missless = config;
    missless["compute_tasks"][1]["ray_tracing"].erase("miss");
    requireRayRefusal(
        [&] {
            (void)Pelican::computeTaskDefinitionsOf(
                namespaceSharedComputeTasks(TestSupport::renderGraphOf(missless), "#xr"), nullptr);
        },
        "pelican.ray_tracing.shader_stage_required@1", {{"task", "trace"}, {"stage", "miss"}});
    auto raygenless = config;
    raygenless["compute_tasks"][1]["ray_tracing"]["raygen"] = 3;
    TestSupport::requireNamedRefusal(
        [&] { (void)TestSupport::computeTaskDefinitionsOf(raygenless); }, "render_graph.compute_task_invalid@1",
        {{"node", "trace"}, {"field", "ray_tracing.raygen"}, {"reason", "requires_string"}});
}

} // namespace Pelican
