#include "graphcanvasmodel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using PelicanStudio::GraphCanvasModel;
using PelicanStudio::GraphCanvasNode;
using PelicanStudio::InspectorNoticeKind;

// The real shipped-form document, not a hand-written miniature: the canvas has
// to read what the engine actually publishes for a `pelican.graph` project.
Json fixtureDocument() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" /
                      "fixtures" / "graph_instance_inputs" / "passes" /
                      "main.json";
    std::ifstream stream{path};
    REQUIRE(stream.good());
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return Json::parse(buffer.str());
}

// What get_graph_instances answers for that document: the root nodes, the
// definition bodies, and the instances among the root nodes.
Json context(bool with_edge = true) {
    const auto document = fixtureDocument();
    auto nodes = document.at("nodes");
    if (with_edge) {
        // The fixture's `left` stores both values and connects neither. One
        // authored edge from the scene clock makes the root scope a graph with
        // an edge rather than two loose nodes.
        for (auto &node : nodes)
            if (node.at("id") == "left")
                node["inputs"] = {{"gain", {{"node", "clock"}, {"port", "value"}}}};
    }
    auto instances = Json::array();
    for (const auto &node : nodes)
        if (node.at("op") == "graph.instance") instances.push_back(node);
    return {{"source_reference", "fixture"},
            {"source_digest", {{"hex", "source-a"}}},
            {"instances", std::move(instances)},
            {"nodes", std::move(nodes)},
            {"definitions", document.at("definitions")}};
}

Json operation(const std::string &op, const std::string &domain,
               const Json &inputs, const Json &outputs, bool sink = false) {
    return {{"op", op}, {"domain", domain}, {"evaluation", "direct"},
            {"sink", sink}, {"ports", "declared"},
            {"inputs", inputs}, {"outputs", outputs}};
}

// The shapes the engine really publishes: a logical type pattern and a logical
// type both name the semantic type under "semantic".
Json declaredInput(const std::string &port, bool required = true,
                   const std::string &type = "pelican.value.scalar@1") {
    return {{"port", port}, {"required", required},
            {"accepted_type", {{"semantic", type},
                               {"required_traits", Json::array()},
                               {"predicates", Json::array()}}}};
}

Json declaredOutput(const std::string &port,
                    const std::string &type = "pelican.value.scalar@1") {
    return {{"port", port},
            {"type", {{"schema", "pelican.logical_type"}, {"version", 1},
                      {"constructor", "value"}, {"semantic", type},
                      {"arguments", Json::array()}}}};
}

Json typeSchema(const std::string &id, const Json &traits,
                const Json &display = Json{}) {
    Json schema{{"id", id}, {"constructor", "value"}, {"traits", traits},
                {"parameters", Json::array()}};
    if (!display.is_null()) schema["display"] = display;
    return schema;
}

// The catalog the builtin registry publishes for the operations the fixture
// uses. graph.instance is deliberately parameter_dependent: its ports follow
// the definition, so the catalog cannot name them.
Json catalog() {
    return {{"schema", "pelican.graph_catalog"}, {"version", 1},
        {"modules", Json::array({
            {{"namespace", "value"}, {"version", 1}, {"description", ""},
             {"types", Json::array({
                 typeSchema("pelican.value.scalar@1", Json::array({"numeric"}))})},
             {"sinks", Json::array()},
             {"operations", Json::array({
                 operation("value.input", "values", Json::array(),
                           Json::array({declaredOutput("value")})),
                 operation("value.add", "values",
                           Json::array({declaredInput("a"), declaredInput("b")}),
                           Json::array({declaredOutput("value")})),
                 {{"op", "graph.input"}, {"domain", "values"},
                  {"evaluation", "direct"}, {"sink", false},
                  {"ports", "parameter_dependent"}},
                 {{"op", "graph.instance"}, {"domain", "values"},
                  {"evaluation", "direct"}, {"sink", false},
                  {"ports", "parameter_dependent"}}})}},
            {{"namespace", "render"}, {"version", 1}, {"description", ""},
             {"types", Json::array({
                 typeSchema("pelican.render.storage_buffer@1",
                            Json::array({"shader_storage"}))})},
             {"sinks", Json::array()},
             {"operations", Json::array({
                 operation("render.buffer", "rendering",
                           Json::array({declaredInput("value")}),
                           Json::array({declaredOutput(
                               "resource", "pelican.render.storage_buffer@1")}),
                           true)})}}})}};
}

struct Harness {
    GraphCanvasModel model;

    PelicanStudio::InspectorRpcRequest take(std::string_view method) {
        auto requests = model.takeRpcRequests();
        const auto found = std::ranges::find(requests, std::string{method},
                                             &PelicanStudio::InspectorRpcRequest::method);
        REQUIRE(found != requests.end());
        return *found;
    }
    void reply(const PelicanStudio::InspectorRpcRequest &request, const Json &value) {
        model.receiveRpcResult(request.request_id, value.dump());
    }
    // Connecting asks for the document and the catalog on separate slots.
    void open(const Json &document = context(), bool with_catalog = true) {
        model.setConnected(true);
        auto requests = model.takeRpcRequests();
        REQUIRE(requests.size() == 2);
        for (const auto &request : requests) {
            if (request.method == "get_graph_instances")
                model.receiveRpcResult(request.request_id, document.dump());
            else if (request.method == "get_graph_catalog" && with_catalog)
                model.receiveRpcResult(request.request_id, catalog().dump());
        }
    }
    const GraphCanvasNode &node(const std::string &id) {
        static GraphCanvasNode empty;
        nodes_ = model.nodes();
        const auto found = std::ranges::find(nodes_, id, &GraphCanvasNode::id);
        REQUIRE(found != nodes_.end());
        return *found;
    }
    std::vector<GraphCanvasNode> nodes_;
};

const PelicanStudio::GraphCanvasPort &port(const GraphCanvasNode &node,
                                           const std::string &name) {
    const auto found = std::ranges::find(node.inputs, name,
                                         &PelicanStudio::GraphCanvasPort::port);
    REQUIRE(found != node.inputs.end());
    return *found;
}
} // namespace

TEST_CASE("The graph canvas reads root and definition scopes out of one document",
          "[studio][graph-canvas]") {
    Harness h;
    h.open();

    // Only the three reads the canvas can justify ever leave it.
    REQUIRE(h.model.takeRpcRequests().empty());

    auto scopes = h.model.scopes();
    REQUIRE(scopes.size() == 2);
    REQUIRE(scopes[0].root);
    REQUIRE(scopes[0].label == "root");
    REQUIRE(scopes[1].id == "gain_curve");

    REQUIRE(h.model.currentScope().empty());
    REQUIRE(h.model.nodes().size() == 2);
    REQUIRE(h.model.edges().size() == 1);
    const auto root_edge = h.model.edges().front();
    REQUIRE(root_edge.from_node == "clock");
    REQUIRE(root_edge.from_port == "value");
    REQUIRE(root_edge.to_node == "left");
    REQUIRE(root_edge.to_port == "gain");

    // The catalog's declared contract supplies the ports of an ordinary node.
    const auto &clock = h.node("clock");
    REQUIRE(clock.domain == "values");
    REQUIRE(clock.evaluation == "direct");
    REQUIRE_FALSE(clock.sink);
    REQUIRE(clock.outputs.size() == 1);
    REQUIRE(clock.outputs[0].port == "value");
    // The socket's type is the catalog's, read off the declared contract.
    REQUIRE(clock.outputs[0].type == "pelican.value.scalar@1");
    // A node with no authored label is headed by its id, not by an empty band.
    REQUIRE(clock.label.empty());
    REQUIRE(clock.heading() == "clock");

    // graph.instance is parameter_dependent, so its ports come from the node
    // and its definition, and they are not labelled as declared.
    const auto &left = h.node("left");
    REQUIRE(left.instance);
    REQUIRE(left.definition == "gain_curve");
    REQUIRE(left.inputs.size() == 2);
    REQUIRE(left.inputs[0].port == "gain");
    REQUIRE(left.inputs[1].port == "bias");
    REQUIRE_FALSE(left.inputs[0].declared);
    REQUIRE(left.inputs[0].connected);
    REQUIRE(left.inputs[0].source_node == "clock");
    REQUIRE(left.inputs[1].has_parameter);
    REQUIRE(left.inputs[1].parameter == 5);
    REQUIRE(left.outputs.size() == 1);
    REQUIRE(left.outputs[0].port == "value");
    // An instance's ports are the definition's boundary: its inputs are the
    // graph.input scalars, and its output follows the definition's own
    // outputs map down to the node that really produces it.
    REQUIRE(left.inputs[0].type == "pelican.value.scalar@1");
    REQUIRE(left.outputs[0].type == "pelican.value.scalar@1");

    // Descending into the instance shows the definition body.
    REQUIRE(h.model.enterInstance("left"));
    REQUIRE(h.model.currentScope() == "gain_curve");
    REQUIRE(h.model.breadcrumb() == std::vector<std::string>{"", "gain_curve"});
    REQUIRE(h.model.nodes().size() == 4);
    REQUIRE(h.model.edges().size() == 3);

    const auto &sum = h.node("sum");
    REQUIRE(sum.inputs.size() == 2);
    REQUIRE(sum.inputs[0].port == "a");
    REQUIRE(sum.inputs[0].declared);
    REQUIRE(sum.inputs[0].connected);
    REQUIRE(sum.inputs[0].source_node == "gain");
    REQUIRE(sum.label.empty());
    REQUIRE(sum.heading() == "sum");
    REQUIRE(sum.op == "value.add");
    // graph.input is structural: no module registers it, so its scalar output
    // comes from what composition expands it into, not from the catalog.
    const auto &gain_input = h.node("gain");
    REQUIRE(gain_input.outputs.size() == 1);
    REQUIRE(gain_input.outputs[0].port == "value");
    REQUIRE(gain_input.outputs[0].type == "pelican.value.scalar@1");
    const auto &buffer = h.node("buffer");
    REQUIRE(buffer.domain == "rendering");
    // The document's own `label` is what the heading shows; the type name is
    // still the op, so a renamed node never hides what it is.
    REQUIRE(buffer.label == "pulse buffer");
    REQUIRE(buffer.heading() == "pulse buffer");
    REQUIRE(buffer.op == "render.buffer");
    REQUIRE(buffer.sink);
    REQUIRE(port(buffer, "value").declared);
    REQUIRE(buffer.outputs.size() == 1);
    REQUIRE(buffer.outputs[0].port == "resource");
    REQUIRE(buffer.outputs[0].type == "pelican.render.storage_buffer@1");
    // The catalog's type schemas came across with the operations.
    REQUIRE(h.model.typeSchema("pelican.render.storage_buffer@1") != nullptr);
    REQUIRE(h.model.typeSchema("pelican.pose@1") == nullptr);

    REQUIRE(h.model.leaveScope());
    REQUIRE(h.model.currentScope().empty());
    REQUIRE_FALSE(h.model.leaveScope());

    REQUIRE(h.model.notice().kind == InspectorNoticeKind::None);
}

// P-2b acceptance 4. The shipped learning project's own rendering config, read
// the way Studio reads one: a canned `get_graph_instances` reply. The canvas
// has to find the definition scope and the owned instance in it, because that
// is what a reader opens the demo to look at.
TEST_CASE("The graph canvas lists the posegraph_demo scope and its instance",
          "[studio][graph-canvas][posegraph-demo]") {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "projects" /
                      "posegraph_demo" / "passes" / "main.json";
    std::ifstream stream{path};
    REQUIRE(stream.good());
    std::stringstream buffer;
    buffer << stream.rdbuf();
    const auto document = Json::parse(buffer.str());
    auto instances = Json::array();
    for (const auto &node : document.at("nodes"))
        if (node.at("op") == "graph.instance") instances.push_back(node);
    const Json demo_context{{"source_reference", "projects/posegraph_demo/passes/main.json"},
                            {"source_digest", {{"hex", "posegraph-demo"}}},
                            {"instances", instances},
                            {"nodes", document.at("nodes")},
                            {"definitions", document.at("definitions")}};

    Harness h;
    h.open(demo_context);
    auto scopes = h.model.scopes();
    REQUIRE(scopes.size() == 2);
    REQUIRE(scopes[0].root);
    REQUIRE(scopes[1].id == "walk_run");

    // The values side of the root: a clock, the playback that wraps it, the
    // curve, and the instance the curve drives.
    REQUIRE(h.model.nodes().size() == 4);
    REQUIRE(h.model.edges().size() == 3);
    const auto &hero = h.node("hero_walk_run");
    REQUIRE(hero.instance);
    REQUIRE(hero.definition == "walk_run");
    REQUIRE(hero.inputs.size() == 1);
    REQUIRE(hero.inputs[0].port == "weight");
    REQUIRE(hero.inputs[0].connected);
    REQUIRE(hero.inputs[0].source_node == "blend_curve");
    // The parameter under the edge is the value the port returns to when the
    // edge is taken away, which is the exercise the README sets.
    REQUIRE(hero.inputs[0].has_parameter);
    REQUIRE(hero.inputs[0].parameter == 0.0);

    // Descending into the instance shows the pose chain itself.
    REQUIRE(h.model.enterInstance("hero_walk_run"));
    REQUIRE(h.model.currentScope() == "walk_run");
    REQUIRE(h.model.breadcrumb() == std::vector<std::string>{"", "walk_run"});
    REQUIRE(h.model.nodes().size() == 6);
    const auto &mix = h.node("mix");
    REQUIRE(port(mix, "a").source_node == "walk");
    REQUIRE(port(mix, "b").source_node == "run");
    REQUIRE(port(mix, "weight").source_node == "weight");
    const auto &sink = h.node("out");
    REQUIRE(port(sink, "pose").source_node == "mix");
}

TEST_CASE("A document without definitions leaves the root scope standing",
          "[studio][graph-canvas]") {
    Harness h;
    // The control for the scope list: remove `definitions` from the reply.
    auto document = context();
    document.erase("definitions");
    h.open(document);

    REQUIRE(h.model.scopes().size() == 1);
    REQUIRE(h.model.scopes().front().root);
    // The canvas does not fall over: the root nodes and the edge are still there.
    REQUIRE(h.model.nodes().size() == 2);
    REQUIRE(h.model.edges().size() == 1);
    // And an instance whose definition body is unknown cannot be descended into.
    REQUIRE_FALSE(h.model.enterInstance("left"));
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
    REQUIRE(h.model.notice().message.find("no definitions") != std::string::npos);

    // The ports of the instance are still readable, from the node's own keys.
    const auto &left = h.node("left");
    REQUIRE(left.inputs.size() == 2);
    REQUIRE_FALSE(left.inputs[0].declared);
}

TEST_CASE("Without the catalog the canvas draws the document's own keys and says so",
          "[studio][graph-canvas]") {
    Harness h;
    // The control for the declared ports: never answer get_graph_catalog.
    h.open(context(), false);

    REQUIRE_FALSE(h.model.catalogReceived());
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
    REQUIRE(h.model.notice().message.find("catalog not fetched") != std::string::npos);

    // No contract: no domain, no evaluation, and the ports are only what the
    // document itself names.
    REQUIRE(h.model.enterInstance("left"));
    const auto &sum = h.node("sum");
    REQUIRE_FALSE(sum.contract_known);
    REQUIRE(sum.domain.empty());
    REQUIRE(sum.inputs.size() == 2);
    REQUIRE_FALSE(sum.inputs[0].declared);
    REQUIRE_FALSE(sum.inputs[1].declared);
    REQUIRE(sum.outputs.size() == 1);
    REQUIRE(sum.outputs[0].port == "value");
    // Without a contract there is no type to put on the socket, and the canvas
    // says so rather than picking one.
    REQUIRE(sum.outputs[0].type.empty());
    REQUIRE(sum.inputs[0].type.empty());
    // `gain` declares no inputs in the document, so without a contract it
    // draws with none rather than invented ones. Its single output is the one
    // thing a graph.input always has, which is a fact about composition and
    // not about the catalog.
    const auto &gain = h.node("gain");
    REQUIRE(gain.inputs.empty());
    REQUIRE(gain.outputs.size() == 1);
    REQUIRE(gain.outputs[0].port == "value");
    // The edges are the document's, so the shape of the graph is unchanged.
    REQUIRE(h.model.edges().size() == 3);
}

TEST_CASE("Published values land on the instance's input ports",
          "[studio][graph-canvas]") {
    Harness h;
    h.open();

    REQUIRE(h.model.valueInstances() == std::vector<std::string>{"left"});
    h.model.watchInstance("left");
    const auto request = h.take("get_graph_values");
    REQUIRE(request.params == Json{{"instance", "left"}});
    h.reply(request, {{"source_digest", {{"hex", "source-a"}}},
        {"instance", "left"}, {"published", true}, {"generation", 3}, {"frame", 9},
        {"inputs", Json::array({
            {{"port", "gain"}, {"value", 1.25}, {"source", "connected"},
             {"connected", {{"node", "clock"}, {"port", "value"}}}},
            {{"port", "bias"}, {"value", 7.0}, {"source", "override"},
             {"parameter", "left/bias"}}})}});

    REQUIRE(h.model.valuesPublished());
    const auto &left = h.node("left");
    REQUIRE(port(left, "gain").has_value);
    REQUIRE(port(left, "gain").value == 1.25);
    REQUIRE(port(left, "gain").value_source == "connected");
    REQUIRE(port(left, "gain").value_connected_node == "clock");
    REQUIRE(port(left, "bias").value == 7.0);
    REQUIRE(port(left, "bias").value_source == "override");
    // The document's own stored value is still what the port returns to.
    REQUIRE(port(left, "bias").has_parameter);
    REQUIRE(port(left, "bias").parameter == 5);
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::None);

    // A node inside a definition never takes the overlay: get_graph_values
    // answers for an instance, not for the nodes of its body (AG-3).
    REQUIRE(h.model.enterInstance("left"));
    REQUIRE_FALSE(port(h.node("sum"), "a").has_value);
}

TEST_CASE("An unpublished graph and a failed value read both leave the canvas standing",
          "[studio][graph-canvas]") {
    SECTION("published: false") {
        Harness h;
        h.open();
        h.model.watchInstance("left");
        h.reply(h.take("get_graph_values"),
                {{"instance", "left"}, {"published", false}, {"generation", 0},
                 {"frame", 0}, {"inputs", Json::array()}});
        REQUIRE_FALSE(h.model.valuesPublished());
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
        REQUIRE(h.model.notice().message.find("not published") != std::string::npos);
        REQUIRE(h.model.nodes().size() == 2);
        REQUIRE_FALSE(port(h.node("left"), "gain").has_value);
    }
    SECTION("the read fails") {
        Harness h;
        h.open();
        h.model.watchInstance("left");
        const auto request = h.take("get_graph_values");
        h.model.receiveRpcFailure(request.request_id, "engine is gone");
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::Error);
        REQUIRE(h.model.notice().message == "engine is gone");
        // The canvas is the document's, so it survives a missing report.
        REQUIRE(h.model.nodes().size() == 2);
        REQUIRE(h.model.edges().size() == 1);
        REQUIRE(h.model.scopes().size() == 2);
    }
}

TEST_CASE("A refresh keeps the scope trail only while the definition is still there",
          "[studio][graph-canvas]") {
    Harness h;
    h.open();
    REQUIRE(h.model.enterInstance("left"));
    REQUIRE(h.model.currentScope() == "gain_curve");

    // Same document: the trail survives a refresh.
    h.model.refresh();
    h.reply(h.take("get_graph_instances"), context());
    REQUIRE(h.model.currentScope() == "gain_curve");

    // A document that dropped the definition cannot keep showing its body.
    auto without = context();
    without["definitions"] = Json::object();
    h.model.refresh();
    h.reply(h.take("get_graph_instances"), without);
    REQUIRE(h.model.currentScope().empty());
    REQUIRE(h.model.scopes().size() == 1);
    // allNodeKeys is what the scene prunes retained positions against.
    REQUIRE(h.model.allNodeKeys().size() == 2);
}
