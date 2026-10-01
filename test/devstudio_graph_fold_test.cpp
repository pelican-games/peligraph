// GL-3a: what the Graph tab reads and writes for a fold group, without Qt --
// the name it proposes, the subjects it offers, the state's words, the
// refusals' sentences, the frame plan's fold marks, and the Frame Plan's
// graph-wide decisions (EL-2).
#include "frameplanmodel.hpp"
#include "graphcanvasmodel.hpp"
#include "graphfoldgroups.hpp"
#include "inspectormodel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace PelicanStudio;

GraphCanvasNode node(std::string id, std::string op) {
    GraphCanvasNode made;
    made.id = std::move(id);
    made.op = std::move(op);
    made.loop = made.op == "graph.loop";
    return made;
}

GraphCanvasPort input(std::string port, std::string source) {
    GraphCanvasPort made;
    made.port = std::move(port);
    made.connected = true;
    made.source_node = std::move(source);
    made.source_port = "image";
    return made;
}

} // namespace

TEST_CASE("GL-3a decision 1: Fold into group takes root nodes, proposes a free name and "
          "sends a group with no decisions",
          "[studio][graph-fold][gl3a]") {
    std::vector<GraphCanvasNode> root{node("blur_loop", "graph.loop"),
                                      node("post_a", "render.fullscreen"),
                                      node("post_b", "render.fullscreen")};
    std::vector<GraphFoldGroup> groups;
    // Rule 8: fold_1 ... the first free one; `<loop>_fold` when a loop is in.
    CHECK(suggestGraphFoldGroupName(groups, {"post_a", "post_b"}, root) == "fold_1");
    groups.push_back(GraphFoldGroup{.group = "fold_1"});
    CHECK(suggestGraphFoldGroupName(groups, {"post_a"}, root) == "fold_2");
    CHECK(suggestGraphFoldGroupName(groups, {"post_a", "blur_loop"}, root) == "blur_loop_fold");
    groups.push_back(GraphFoldGroup{.group = "blur_loop_fold"});
    CHECK(suggestGraphFoldGroupName(groups, {"blur_loop"}, root) == "blur_loop_fold_2");

    // A node of a zone's body or a folded feature is no root node.
    CHECK(graphFoldPickRefusal({{"post_a", "", false}, {"post_b", "", false}}).empty());
    CHECK(graphFoldPickRefusal({}) == "Pick the root nodes to fold first.");
    CHECK(graphFoldPickRefusal({{"post_a", "", false}, {"blur", "blur_body", false}}) ==
          "blur is inside a loop's zone (blur_body): fold the loop itself.");
    CHECK(graphFoldPickRefusal({{"feature:bloom", "", true}}).starts_with("feature:bloom is a "
                                                                          "folded feature"));

    // fold_nodes: no confirmation, no decisions, never `verified`.
    CHECK(graphFoldNodesParams(std::string(64, 'a'), "fold_1", {"post_a", "post_b"},
                               graphFoldNewFolded()) ==
          Json{{"base_source_digest", std::string(64, 'a')},
               {"group", "fold_1"},
               {"nodes", {"post_a", "post_b"}},
               {"folded", {{"kind", "decisions"}, {"decisions", Json::object()}}}});
    // Decision 5: Unfold is folded null, with no nodes.
    CHECK(graphFoldNodesParams("d", "fold_1", {"post_a"}, Json(nullptr)) ==
          Json{{"base_source_digest", "d"}, {"group", "fold_1"}, {"folded", nullptr}});
}

TEST_CASE("GL-3a decision 3: get_fold_groups' state reads as the heading's words, and the "
          "root's decisions are laid over it",
          "[studio][graph-fold][gl3a]") {
    const auto digest = std::string(64, 'c');
    const Json report{
        {"source_digest", {{"hex", "x"}}},
        {"fold_groups",
         Json::array({{{"group", "fresh"}, {"nodes", {"a", "b"}}, {"kind", "decisions"},
                       {"digest", digest}, {"state", "unverified"}},
                      {{"group", "moved"}, {"nodes", {"c"}}, {"kind", "decisions"},
                       {"digest", digest},
                       {"verified", {{"digest", std::string(64, 'f')}, {"at", "t"}}},
                       {"state", "stale"}},
                      {{"group", "good"}, {"nodes", {"d"}}, {"kind", "decisions"},
                       {"digest", digest},
                       {"verified", {{"digest", digest}, {"at", "t"}}},
                       {"state", "verified"}}})}};
    auto groups = parseGraphFoldGroupsReport(report);
    REQUIRE(groups.size() == 3);
    CHECK(groups[0].nodes == std::vector<std::string>{"a", "b"});
    // FG-2 is not there yet: every group says its decisions are not applied.
    CHECK(groups[0].stateLine() == "not verified \xE2\x80\x94 decisions are not applied yet");
    CHECK(groups[1].stateLine() == "changed since verified \xE2\x80\x94 runs the open form");
    CHECK(groups[1].stale());
    CHECK(groups[1].stateTooltip().find(std::string(64, 'f')) != std::string::npos);
    CHECK(groups[1].stateTooltip().find(digest) != std::string::npos);
    // Rule 5: nothing when it is as it should be.
    CHECK(groups[2].stateLine().empty());

    const Json root{{"fold_groups",
                     {{"fresh",
                       {{"nodes", {"a", "b"}},
                        {"folded",
                         {{"kind", "decisions"},
                          {"decisions",
                           {{"isolate", Json::array({{{"node", "a"}}})},
                            {"serial", Json::array({{{"node", "b"}, {"element", 2}}})}}}}}}}}}};
    readGraphFoldDecisions(groups, root);
    CHECK(groups[0].isolate == std::vector<GraphFoldSubject>{{"a", std::nullopt, ""}});
    CHECK(groups[0].serial == std::vector<GraphFoldSubject>{{"b", 2, ""}});
    CHECK(groups[1].isolate.empty());
    CHECK(groups[0].folded() ==
          Json{{"kind", "decisions"},
               {"decisions",
                {{"serial", Json::array({{{"node", "b"}, {"element", 2}}})},
                 {"isolate", Json::array({{{"node", "a"}}})}}}});
}

TEST_CASE("GL-3a decision 4: the Add row offers the group's nodes, a loop's elements and a "
          "writer's ports, nothing typed",
          "[studio][graph-fold][gl3a]") {
    auto writer = node("post", "render.fullscreen");
    writer.inputs.push_back(input("color", "post_image"));
    writer.inputs.push_back(input("source", "lit"));
    std::vector<GraphCanvasNode> root{node("blur_loop", "graph.loop"), writer,
                                      node("post_image", "render.image"),
                                      node("lit", "render.target_ref"),
                                      node("time", "value.input")};
    GraphFoldGroup group{.group = "g", .nodes = {"blur_loop", "post", "post_image", "time"}};
    const std::map<std::string, std::int64_t, std::less<>> counts{{"blur_loop", 4}};

    const auto serial = graphFoldNodeChoices(group, GraphFoldField::serial, root, counts);
    REQUIRE(serial.size() == 4);
    CHECK(serial[0].loop);
    CHECK(serial[0].count == 4);
    CHECK_FALSE(serial[1].needs_port);

    // No alias: the image, the loop, and the writer through its ports --
    // not a value node.
    const auto no_alias = graphFoldNodeChoices(group, GraphFoldField::no_alias, root, counts);
    REQUIRE(no_alias.size() == 3);
    CHECK(no_alias[0].node == "blur_loop");
    CHECK(no_alias[1].node == "post");
    CHECK(no_alias[1].needs_port);
    CHECK(no_alias[1].ports == std::vector<std::string>{"color", "source"});
    CHECK(no_alias[2].node == "post_image");
    CHECK_FALSE(no_alias[2].needs_port);

    CHECK(GraphFoldSubject{"blur_loop", 1, ""}.text() == "blur_loop [1]");
    CHECK(GraphFoldSubject{"post", std::nullopt, "color"}.toJson() ==
          Json{{"node", "post"}, {"port", "color"}});
}

TEST_CASE("GL-3a decision 6: a decision a fold group made is marked on the Frame Plan's row "
          "and on the node's band",
          "[studio][graph-fold][gl3a]") {
    // The Frame Plan's Decisions table: from_fold, on a plan the producer
    // made, one of its decisions marked as a fold group's.
    std::ifstream stream{std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
                             "devstudio" / "example_frame_plan.json",
                         std::ios::binary};
    REQUIRE(stream.good());
    auto plan = Json::parse(stream);
    // As the frame plan tests read the capture: a runtime plan.
    plan["profile"] = "runtime";
    for (auto &entry : plan.at("nodes")) entry["shader_resolution"] = {{"state", "not_applicable"}};
    auto &decisions = plan.at("physical_target_plan").at("decisions");
    REQUIRE_FALSE(decisions.empty());
    decisions[0]["from_fold"] = "g";
    const auto model = buildFramePlanModel(plan.dump());
    std::vector<std::string> folds;
    for (const auto &group : model.decision_groups)
        for (const auto &entry : group.decisions)
            if (!entry.from_fold.empty()) folds.push_back(entry.id + "|" + entry.from_fold);
    CHECK(folds == std::vector<std::string>{decisions[0].at("id").get<std::string>() + "|g"});

    // The Graph's band: `isolate (fold g)` on the node whose pass it is, and
    // the image's own no_alias on the image node.
    GraphCanvasModel graph;
    graph.setConnected(true);
    const Json document{
        {"source_reference", "passes/main.json"},
        {"root_schema", "pelican.graph"},
        {"source_digest", {{"hex", "d"}}},
        {"nodes", Json::array({{{"id", "lit"}, {"op", "render.image"},
                                {"parameters", Json::object()},
                                {"outputs", Json::array({"resource"})}},
                               {{"id", "tint"}, {"op", "render.fullscreen"},
                                {"parameters", Json::object()},
                                {"outputs", Json::array({"color"})}}})},
        {"definitions", Json::object()}};
    for (const auto &request : graph.takeRpcRequests())
        if (request.method == "get_graph_instances")
            graph.receiveRpcResult(request.request_id, document.dump());
    const auto pass = [](const std::string &name, std::size_t order, const Json &writes) {
        return Json{{"name", name},   {"kind", "render"},   {"order", order},
                    {"level", order}, {"source", "engine"}, {"reads", Json::array()},
                    {"writes", writes}, {"reads_history", Json::array()},
                    {"declaration_index", order}};
    };
    auto tint = pass("tint", 0, Json::array({"lit"}));
    tint["origin"] = {{"graph", "passes/main.json"}, {"node", "tint"}};
    Json frame_plan{{"schema", "pelican.frame_plan"},
                    {"version", 1},
                    {"graph", "main_render"},
                    {"loops", Json::array()},
                    {"nodes", Json::array({tint, pass("present", 1, Json::array())})},
                    {"diagnostics", Json::array()},
                    {"deactivated_graph_nodes", Json::array()}};
    for (const auto &request : graph.takeRpcRequests())
        if (request.method == "get_frame_plan")
            graph.receiveRpcResult(request.request_id, frame_plan.dump());
    for (const auto &domain : GraphCanvasModel::domainNames())
        graph.setDomainShown(domain, domain == "render");
    REQUIRE(graph.layersShown());
    // Not verified: the plan names none, and nothing is said.
    CHECK(graph.layerBand("tint").find("fold") == std::string::npos);

    frame_plan["fold_decisions"] =
        Json::array({{{"group", "g"}, {"kind", "node"}, {"name", "tint"}, {"field", "isolate"}},
                     {{"group", "g"}, {"kind", "resource"}, {"name", "lit"}, {"field", "no_alias"}}});
    graph.refresh();
    for (const auto &request : graph.takeRpcRequests())
        if (request.method == "get_graph_instances")
            graph.receiveRpcResult(request.request_id, document.dump());
    for (const auto &request : graph.takeRpcRequests())
        if (request.method == "get_frame_plan")
            graph.receiveRpcResult(request.request_id, frame_plan.dump());
    CHECK(graph.foldDecisionParts("tint") == std::vector<std::string>{"isolate (fold g)"});
    CHECK(graph.layerBand("tint").find("isolate (fold g)") != std::string::npos);
    CHECK(graph.foldDecisionParts("lit") == std::vector<std::string>{"no_alias (fold g)"});
}

TEST_CASE("GL-3a decision 7: the fold refusals read as sentences", "[studio][graph-fold][gl3a]") {
    CHECK(graphLoopRefusalDescription(Json{{"code", "graph.fold_decision_conflict@1"},
                                           {"message", "target_planning.main_render: fold group "
                                                       "g decides isolate for tint"},
                                           {"group", "g"},
                                           {"name", "tint"},
                                           {"field", "isolate"}}) ==
          "g asks isolate for tint, but the graph-wide setting says otherwise "
          "(graph.fold_decision_conflict@1).");
    CHECK(graphLoopRefusalDescription(
              Json{{"code", "graph.fold_subject_outside@1"},
                   {"message", "graph.fold_subject_outside@1"},
                   {"group", "g"},
                   {"subject", {{"node", "far"}, {"element", 2}}}}) ==
          "Fold group g names far [2], which is not one of its nodes "
          "(graph.fold_subject_outside@1).");
    CHECK(graphLoopRefusalDescription(Json{{"code", "graph.fold_kind_unsupported@1"},
                                           {"message", "x"},
                                           {"group", "g"},
                                           {"kind", "replacement"}}) ==
          "Fold group g is folded as replacement; only decisions can be declared for now "
          "(graph.fold_kind_unsupported@1).");
}

TEST_CASE("GL-3a decision 8: the Frame Plan's Serial / Isolate / No alias read the root's "
          "target_planning and write edit_target_planning",
          "[studio][graph-fold][gl3a]") {
    const Json root{
        {"schema", "pelican.graph"},
        {"rendering",
         {{"target_planning",
           {{"graphs",
             {{"main_render",
               {{"nodes", {{"tint", {{"isolate", true}}}}},
                {"resources", {{"lit", {{"no_alias", true}}}}}}}}}}}}}};
    const auto planning = GraphTargetPlanning::fromRoot(root, "main_render");
    CHECK(planning.flag("node", "tint", "isolate"));
    CHECK_FALSE(planning.flag("node", "tint", "serial"));
    CHECK(planning.flag("resource", "lit", "no_alias"));
    CHECK_FALSE(GraphTargetPlanning::fromRoot(root, "other").flag("node", "tint", "isolate"));

    CHECK(graphTargetPlanningParams("d", "main_render", "resource", "lit", "no_alias", true) ==
          Json{{"base_source_digest", "d"},
               {"graph", "main_render"},
               {"resources", {{"lit", {{"no_alias", true}}}}}});
    // Off is the field erased (null), not false.
    CHECK(graphTargetPlanningParams("d", "main_render", "node", "tint", "isolate", false) ==
          Json{{"base_source_digest", "d"},
               {"graph", "main_render"},
               {"nodes", {{"tint", {{"isolate", nullptr}}}}}});
}

// --- GL-4a: Verify (FG-2's verify_fold) and what it found.

TEST_CASE("GL-4a decision 1: Verify sends the engine's defaults, or 2 frames after 1 s with "
          "Quick check, never a tolerance",
          "[studio][graph-fold][gl4a]") {
    CHECK(graphFoldVerifyParams("d", "g", false) ==
          Json{{"base_source_digest", "d"}, {"group", "g"}});
    CHECK(graphFoldVerifyParams("d", "g", true) ==
          Json{{"base_source_digest", "d"}, {"group", "g"}, {"frames", 2}, {"warmup_seconds", 1}});
    // Decision 2: the heading while it runs.
    CHECK(graphFoldVerifyingLine(12) == "verifying\xE2\x80\xA6 (12 s)");
}

TEST_CASE("GL-4a decision 3: what a finished verify_fold says, read from get_edit_result",
          "[studio][graph-fold][gl4a]") {
    using Kind = GraphFoldVerifyOutcome::Kind;
    const auto passed = readGraphFoldVerifyOutcome(
        Json{{"status", "committed"}, {"committed", true}, {"operation", "verify_fold"},
             {"group", "g"}, {"verified", {{"digest", std::string(64, 'a')}, {"at", "t"}, {"frames", 2}}}},
        "g");
    CHECK(passed.kind == Kind::passed);
    CHECK(passed.notice == "g verified \xE2\x80\x94 its decisions now apply");
    CHECK(passed.heading.empty());

    const auto rejected = [](Json error) {
        return Json{{"status", "rejected"}, {"committed", false}, {"error", std::move(error)}};
    };
    const auto pixels = readGraphFoldVerifyOutcome(
        rejected({{"code", "graph.fold_mismatch@1"}, {"message", "x"}, {"group", "g"},
                  {"what", "pixels"}, {"detail", {{"frame", 2}, {"differing_pixels", 14}}}}),
        "g");
    CHECK(pixels.kind == Kind::mismatch);
    CHECK(pixels.heading == "frame 2 differs in 14 pixels");
    CHECK(readGraphFoldVerifyOutcome(
              rejected({{"code", "graph.fold_mismatch@1"}, {"what", "validation"},
                        {"detail", {{"validation_errors", 3}, {"control_validation_errors", 1}}}}),
              "g")
              .heading == "3 validation errors (open form: 1)");
    CHECK(readGraphFoldVerifyOutcome(
              rejected({{"code", "graph.fold_mismatch@1"}, {"what", "compile"},
                        {"detail", {{"exit_code", 3}, {"message", "m"}}}}),
              "g")
              .heading == "the decided form exited with 3");

    const auto unstable = readGraphFoldVerifyOutcome(
        rejected({{"code", "graph.fold_verify_unstable@1"}, {"group", "g"},
                  {"detail", {{"frame", 1}, {"differing_pixels", 900}}}}),
        "g");
    CHECK(unstable.kind == Kind::unstable);
    CHECK(unstable.heading ==
          "the open form itself is not stable (frame 1, 900 pixels) \xE2\x80\x94 not the "
          "decisions' fault");
    CHECK(unstable.notice.find("Clear Quick check") != std::string::npos);

    // One line on the notice, no mark.
    const auto moved = readGraphFoldVerifyOutcome(
        rejected({{"code", "external_modification"}, {"message", "moved"}}), "g");
    CHECK(moved.kind == Kind::failed);
    CHECK(moved.notice == "g: the graph changed while verifying; verify again.");
    CHECK(readGraphFoldVerifyOutcome(
              rejected({{"code", "fold_verify_failed"}, {"message", "m"}, {"group", "g"},
                        {"run", "open"}, {"detail", {{"exit_code", 1}}}}),
              "g")
              .notice == "g could not be verified: its open form could not be drawn (open run) "
                         "(fold_verify_failed).");
    CHECK(readGraphFoldVerifyOutcome(
              rejected({{"code", "fold_verify_unavailable"}, {"message", "m"}, {"reason", "headless"}}),
              "g")
              .notice == "This engine cannot verify a fold group now (headless) "
                         "(fold_verify_unavailable).");
    CHECK(readGraphFoldVerifyOutcome(
              Json{{"status", "rejected"},
                   {"error", {{"code", "fold_verify_busy"}, {"message", "m"}, {"group", "g"},
                              {"ticket", "t"}}}},
              "g")
              .notice == "g is being verified already; wait for it to finish (fold_verify_busy).");
}

TEST_CASE("GL-4a decision 5: a verified decision names the expanded names it landed on, or "
          "nothing once its subject matches nothing",
          "[studio][graph-fold][gl4a]") {
    GraphCanvasPlanPass copy0;
    copy0.name = "blur/0/pass";
    copy0.origin = GraphCanvasPlanOrigin{.graph = "r", .node = "pass", .loop = "blur", .element = 0};
    GraphCanvasPlanPass copy1 = copy0;
    copy1.name = "blur/1/pass";
    copy1.origin->element = 1;
    GraphCanvasPlanPass tint;
    tint.name = "tint";
    tint.origin = GraphCanvasPlanOrigin{.graph = "r", .node = "tint"};
    const std::vector<GraphCanvasPlanPass> passes{copy0, copy1, tint};
    const std::vector<GraphCanvasFoldDecision> decisions{
        {"g", "node", "blur/0/pass", "serial"},
        {"g", "node", "blur/1/pass", "serial"},
        {"g", "node", "tint", "isolate"},
        {"g", "resource", "blur/1/img", "no_alias"},
        {"g", "resource", "tint__color", "no_alias"},
        {"other", "node", "tint", "serial"}};
    const auto resource = [](const std::string &node, const std::string &port) {
        return node == "tint" && port == "color" ? std::string{"tint__color"} : std::string{};
    };
    CHECK(graphFoldAppliedNames("g", GraphFoldField::serial, {"blur", std::nullopt, ""}, decisions,
                                passes, resource) ==
          std::vector<std::string>{"blur/0/pass", "blur/1/pass"});
    CHECK(graphFoldAppliedNames("g", GraphFoldField::serial, {"blur", 1, ""}, decisions, passes,
                                resource) == std::vector<std::string>{"blur/1/pass"});
    CHECK(graphFoldAppliedNames("g", GraphFoldField::isolate, {"tint", std::nullopt, ""}, decisions,
                                passes, resource) == std::vector<std::string>{"tint"});
    // A loop's resource by its expanded name; a writer's port by the
    // resource it is connected to.
    CHECK(graphFoldAppliedNames("g", GraphFoldField::no_alias, {"blur", 1, ""}, decisions, passes,
                                resource) == std::vector<std::string>{"blur/1/img"});
    CHECK(graphFoldAppliedNames("g", GraphFoldField::no_alias, {"tint", std::nullopt, "color"},
                                decisions, passes, resource) ==
          std::vector<std::string>{"tint__color"});
    // An element the loop no longer has.
    CHECK(graphFoldAppliedNames("g", GraphFoldField::serial, {"blur", 5, ""}, decisions, passes,
                                resource)
              .empty());
}
