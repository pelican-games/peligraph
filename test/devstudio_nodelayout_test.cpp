// N-4: the shared automatic placement, judged on the graphs the two canvases
// actually draw. Nothing here links Qt: a placement is arithmetic over boxes
// and a relation, so the whole of it can be measured without a window.
//
// Each fixture is measured twice -- once with the rule Studio used until now
// (longest-path columns, stacked in model order) and once with the placement
// under test -- and the numbers the run produced are written into the file, so
// a change that makes any of them worse has to say so out loud.

#include "nodelayout.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace PelicanStudio;

namespace {

using Json = nlohmann::json;

// The canvas's own geometry rule, so a fixture's boxes are the sizes the user
// would see rather than a constant invented for the test: a heading band over
// one row per port, padded top and bottom.
constexpr double HeaderHeight = 32.0;
constexpr double SectionPadding = 7.0;
constexpr double SocketRowHeight = 21.0;
constexpr double NodeMinimumWidth = 196.0;
// What the old rule advanced a column by: one fixed node width plus the gap.
constexpr double LegacyNodeWidth = 240.0;
constexpr double HorizontalGap = 88.0;
constexpr double VerticalGap = 74.0;
constexpr double StaggerOffset = 132.0;

struct Fixture {
    std::string name;
    std::vector<LayoutNode> nodes;
    std::vector<LayoutEdge> edges;
};

double bodyHeight(std::size_t rows) {
    return HeaderHeight + SectionPadding +
           static_cast<double>(rows) * SocketRowHeight + SectionPadding;
}

Json readJson(const std::string &path) {
    std::ifstream stream{path};
    REQUIRE(stream.good());
    return Json::parse(stream);
}

// One scope of a graph document: the nodes it holds and the port connections
// between them, which is exactly the relation the Graph canvas lays out.
Fixture graphDocument(const std::string &name, const std::string &path,
                      const std::string &definition = {}) {
    Json document = readJson(path);
    if (!definition.empty())
        document = document.at("definitions").at(definition);
    Fixture fixture;
    fixture.name = name;
    std::set<std::string> present;
    for (const auto &node : document.at("nodes")) {
        std::set<std::string> ports;
        if (const auto inputs = node.find("inputs"); inputs != node.end())
            for (const auto &entry : inputs->items()) ports.insert(entry.key());
        if (const auto stored = node.find("parameters"); stored != node.end())
            for (const auto &entry : stored->items()) ports.insert(entry.key());
        std::size_t rows = ports.size();
        if (const auto outputs = node.find("outputs"); outputs != node.end())
            rows += outputs->size();
        const auto id = node.at("id").get<std::string>();
        present.insert(id);
        fixture.nodes.push_back(
            LayoutNode{id, NodeMinimumWidth, bodyHeight(rows)});
    }
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto &node : document.at("nodes")) {
        const auto inputs = node.find("inputs");
        if (inputs == node.end()) continue;
        for (const auto &entry : inputs->items()) {
            if (!entry.value().is_object()) continue;
            const auto source = entry.value().find("node");
            if (source == entry.value().end()) continue;
            const auto from = source->get<std::string>();
            const auto to = node.at("id").get<std::string>();
            if (!present.contains(from) || from == to) continue;
            if (!seen.emplace(from, to).second) continue;
            fixture.edges.push_back(LayoutEdge{from, to});
        }
    }
    return fixture;
}

// The frame plan, whose nodes are passes and whose relation is the execution
// plan's dependencies. The scene draws one line per pair however many
// resources it carries, so the pairs are collapsed here the same way.
Fixture framePlan(const std::string &name, const std::string &path) {
    const Json document = readJson(path);
    Fixture fixture;
    fixture.name = name;
    std::set<std::string> present;
    for (const auto &node : document.at("nodes")) {
        const std::size_t rows = node.value("reads", Json::array()).size() +
                                 node.value("writes", Json::array()).size();
        const auto id = node.at("name").get<std::string>();
        present.insert(id);
        fixture.nodes.push_back(
            LayoutNode{id, NodeMinimumWidth, bodyHeight(rows)});
    }
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto &dependency :
         document.at("execution_plan").at("dependencies")) {
        const auto from = dependency.at("from").get<std::string>();
        const auto to = dependency.at("to").get<std::string>();
        if (!present.contains(from) || !present.contains(to) || from == to)
            continue;
        if (!seen.emplace(from, to).second) continue;
        fixture.edges.push_back(LayoutEdge{from, to});
    }
    return fixture;
}

// The rule Studio used until N-4, spelled out here so "today's number" is
// measured rather than remembered: the same longest-path columns, each column
// stacked in the order the model listed its nodes, every other column nudged
// down by a quarter of the stagger.
LayoutBoxes stackedColumns(const Fixture &fixture) {
    const NodeGraphLayout columns =
        layoutNodeGraph(fixture.nodes, fixture.edges, LayoutOptions{});
    LayoutBoxes boxes;
    std::map<int, double> cursor;
    for (const LayoutNode &node : fixture.nodes) {
        const int column = columns.columns.at(node.id);
        if (!cursor.contains(column))
            cursor[column] = column % 2 == 1 ? StaggerOffset * 0.25 : 0.0;
        boxes.emplace(node.id,
                      LayoutBox{column * (LegacyNodeWidth + HorizontalGap),
                                cursor[column], node.width, node.height});
        cursor[column] += node.height + VerticalGap;
    }
    return boxes;
}

struct Measured {
    std::size_t crossings = 0;
    std::size_t overlaps = 0;
    std::size_t edge_node_hits = 0;
    double edge_span = 0.0;
};

Measured measure(const LayoutBoxes &boxes, const std::vector<LayoutEdge> &edges,
                 const std::vector<LayoutRoute> &bends) {
    const std::vector<LayoutRoute> routes = layoutRoutes(boxes, edges, bends);
    return Measured{layoutCrossings(edges, routes), layoutOverlaps(boxes),
                    layoutEdgeNodeHits(boxes, edges, routes),
                    layoutEdgeSpan(boxes, edges)};
}

Fixture posegraphRoot() {
    return graphDocument("posegraph_demo root", PELICAN_TEST_POSEGRAPH_GRAPH);
}
Fixture posegraphWalkRun() {
    return graphDocument("posegraph_demo walk_run", PELICAN_TEST_POSEGRAPH_GRAPH,
                         "walk_run");
}
Fixture nodeVisuals() {
    return graphDocument("graph_node_visuals", PELICAN_TEST_NODE_VISUALS_GRAPH);
}
Fixture examplePlan() {
    return framePlan("example frame plan", PELICAN_TEST_FRAME_PLAN_FIXTURE);
}

// What each fixture measured on the day N-4 landed. `legacy` is what the old
// rule produced on the same numbers; `chosen` is what the placement under test
// produced. Both are asserted: the first is the promise the spec made, the
// second is what makes a quiet regression loud.
struct Expectation {
    Fixture (*build)();
    std::size_t legacy_crossings;
    std::size_t chosen_crossings;
    std::size_t legacy_edge_node_hits;
};

const Expectation Expectations[] = {
    {&posegraphRoot, 0, 0, 0},
    {&posegraphWalkRun, 0, 0, 1},
    {&nodeVisuals, 0, 0, 0},
    {&examplePlan, 133, 44, 41},
};

} // namespace

TEST_CASE("The automatic placement crosses no more edges than the old rule did",
          "[devstudio][node-layout]") {
    for (const Expectation &expected : Expectations) {
        const Fixture fixture = expected.build();
        INFO("fixture: " << fixture.name);
        REQUIRE_FALSE(fixture.nodes.empty());

        const LayoutBoxes legacy = stackedColumns(fixture);
        const Measured before = measure(legacy, fixture.edges, {});
        // The old numbers are what they were: if the fixture itself changes,
        // this is the assertion that says so before the comparison below is
        // read as a win.
        CHECK(before.crossings == expected.legacy_crossings);
        CHECK(before.edge_node_hits == expected.legacy_edge_node_hits);

        const NodeGraphLayout layout =
            layoutNodeGraph(fixture.nodes, fixture.edges, LayoutOptions{});
        const Measured after =
            measure(layout.boxes, fixture.edges, layout.bends);
        INFO("crossings " << before.crossings << " -> " << after.crossings);
        CHECK(after.crossings <= before.crossings);
        CHECK(after.crossings == expected.chosen_crossings);
        CHECK(std::isfinite(after.edge_span));
    }
}

TEST_CASE("Nothing overlaps and no edge is drawn across a node",
          "[devstudio][node-layout]") {
    for (const Expectation &expected : Expectations) {
        const Fixture fixture = expected.build();
        INFO("fixture: " << fixture.name);
        const NodeGraphLayout layout =
            layoutNodeGraph(fixture.nodes, fixture.edges, LayoutOptions{});
        const Measured after =
            measure(layout.boxes, fixture.edges, layout.bends);
        // The push-apart: two nodes never share a pixel.
        CHECK(after.overlaps == 0);
        // The dummy chain: an edge that flies over a column goes between the
        // nodes there, never through one.
        CHECK(after.edge_node_hits == 0);
        CHECK(layout.boxes.size() == fixture.nodes.size());
    }
}

TEST_CASE("Every edge points left to right", "[devstudio][node-layout]") {
    for (const Expectation &expected : Expectations) {
        const Fixture fixture = expected.build();
        INFO("fixture: " << fixture.name);
        const NodeGraphLayout layout =
            layoutNodeGraph(fixture.nodes, fixture.edges, LayoutOptions{});
        // These three documents are acyclic, so nothing had to be turned
        // around: an edge drawn right to left would be a fact about the
        // document rather than about the placement, and there is none here.
        CHECK(layout.reversed_edges.empty());
        for (const LayoutEdge &edge : fixture.edges) {
            INFO(edge.from << " -> " << edge.to);
            REQUIRE(layout.columns.contains(edge.from));
            REQUIRE(layout.columns.contains(edge.to));
            CHECK(layout.columns.at(edge.from) < layout.columns.at(edge.to));
            const LayoutBox &from = layout.boxes.at(edge.from);
            const LayoutBox &to = layout.boxes.at(edge.to);
            CHECK(from.right() <= to.x);
        }
    }
}

TEST_CASE("The same input always places the same way",
          "[devstudio][node-layout]") {
    for (const Expectation &expected : Expectations) {
        const Fixture fixture = expected.build();
        INFO("fixture: " << fixture.name);
        const NodeGraphLayout once =
            layoutNodeGraph(fixture.nodes, fixture.edges, LayoutOptions{});
        const NodeGraphLayout twice =
            layoutNodeGraph(fixture.nodes, fixture.edges, LayoutOptions{});
        CHECK(once.boxes == twice.boxes);
        CHECK(once.columns == twice.columns);
        CHECK(once.bends == twice.bends);
        CHECK(once.crossings == twice.crossings);
    }
}

TEST_CASE("The barycenter sweeps are what untangle the drawing",
          "[devstudio][node-layout]") {
    // The control, standing: with the sweeps switched off, the same fixture
    // comes out of the same code far more tangled. This is the measurement
    // that says the sweeps are doing the work the number above credits them
    // with -- not the layering, and not the coordinates.
    const Fixture fixture = examplePlan();
    LayoutOptions none;
    none.ordering_sweeps = 0;
    const NodeGraphLayout unswept =
        layoutNodeGraph(fixture.nodes, fixture.edges, none);
    const NodeGraphLayout swept =
        layoutNodeGraph(fixture.nodes, fixture.edges, LayoutOptions{});
    const Measured without =
        measure(unswept.boxes, fixture.edges, unswept.bends);
    const Measured with = measure(swept.boxes, fixture.edges, swept.bends);
    INFO("unswept " << without.crossings << " vs swept " << with.crossings);
    CHECK(with.crossings < without.crossings);
    CHECK(without.crossings > 133);
}

TEST_CASE("A caller with a column rule of its own keeps it",
          "[devstudio][node-layout]") {
    // The Frame Plan has one: a collapsed group sits at the minimum column of
    // its members and a boundary stub one column outside the window. The
    // layout orders, aligns and routes; it does not re-layer.
    const std::vector<LayoutNode> nodes{{"stub", 120.0, 40.0},
                                        {"first", 200.0, 90.0},
                                        {"second", 200.0, 90.0}};
    const std::vector<LayoutEdge> edges{{"stub", "first"}, {"first", "second"}};
    LayoutOptions options;
    options.columns = LayoutColumns{{"stub", -1}, {"first", 0}, {"second", 1}};
    const NodeGraphLayout layout = layoutNodeGraph(nodes, edges, options);
    CHECK(layout.columns.at("stub") == -1);
    CHECK(layout.columns.at("first") == 0);
    CHECK(layout.columns.at("second") == 1);
    CHECK(layout.boxes.at("stub").x < layout.boxes.at("first").x);
    CHECK(layout.boxes.at("first").x < layout.boxes.at("second").x);
    CHECK(layoutOverlaps(layout.boxes) == 0);
}

TEST_CASE("A cycle is drawn, and the edge that had to be turned around is named",
          "[devstudio][node-layout]") {
    const std::vector<LayoutNode> nodes{
        {"a", 100.0, 40.0}, {"b", 100.0, 40.0}, {"c", 100.0, 40.0}};
    const std::vector<LayoutEdge> edges{{"a", "b"}, {"b", "c"}, {"c", "a"}};
    const NodeGraphLayout layout = layoutNodeGraph(nodes, edges, LayoutOptions{});
    CHECK(layout.boxes.size() == 3);
    CHECK(layout.reversed_edges.size() == 1);
    CHECK(layoutOverlaps(layout.boxes) == 0);
}

TEST_CASE("An edge that flies over a column is given a lane there",
          "[devstudio][node-layout]") {
    // Three columns, and one edge that skips the middle one. Without the bend
    // the line would be drawn straight across whatever the middle column
    // holds; with it the line runs level through a lane of its own.
    const std::vector<LayoutNode> nodes{{"source", 200.0, 60.0},
                                        {"middle", 200.0, 60.0},
                                        {"sink", 200.0, 60.0}};
    const std::vector<LayoutEdge> edges{
        {"source", "middle"}, {"middle", "sink"}, {"source", "sink"}};
    const NodeGraphLayout layout = layoutNodeGraph(nodes, edges, LayoutOptions{});
    REQUIRE(layout.bends.size() == 3);
    CHECK(layout.bends[0].empty());
    CHECK(layout.bends[1].empty());
    // Two points: the lane enters the column and leaves it at the same height,
    // so only the gaps between columns slope.
    REQUIRE(layout.bends[2].size() == 2);
    CHECK(layout.bends[2][0].y == layout.bends[2][1].y);
    CHECK(layout.bends[2][0].x < layout.bends[2][1].x);
    const std::vector<LayoutRoute> routes =
        layoutRoutes(layout.boxes, edges, layout.bends);
    CHECK(layoutEdgeNodeHits(layout.boxes, edges, routes) == 0);
}

TEST_CASE("Duplicate and self edges do not move anything",
          "[devstudio][node-layout]") {
    // A frame plan holds the same pair once per resource, and the scene draws
    // one line for the bundle. Feeding the pair twice must place the same way.
    const std::vector<LayoutNode> nodes{{"a", 120.0, 50.0}, {"b", 120.0, 50.0}};
    const NodeGraphLayout once =
        layoutNodeGraph(nodes, {{"a", "b"}}, LayoutOptions{});
    const NodeGraphLayout twice = layoutNodeGraph(
        nodes, {{"a", "b"}, {"a", "b"}, {"a", "a"}}, LayoutOptions{});
    CHECK(once.boxes == twice.boxes);
    CHECK(twice.bends.size() == 3);
}
