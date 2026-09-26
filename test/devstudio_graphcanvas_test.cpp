#include "graphcanvasmodel.hpp"
#include "graphcomposition.hpp"

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
    // What get_graph_catalog answers on open().
    Json catalog_reply = catalog();

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
                model.receiveRpcResult(request.request_id, catalog_reply.dump());
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

// --------------------------------------------------------------------------
// AG-3b: the node values overlay, in both scopes.

namespace {
// What AG-3a answers. `instance` is echoed back: the empty string is the root
// scope's question, and `node` is the id inside that scope while `expanded` is
// the whole path.
Json nodeValues(const std::string &instance, const Json &nodes,
                bool published = true, double frame = 12) {
    return {{"instance", instance}, {"generation", 4}, {"frame", frame},
            {"published", published}, {"source", "rendering"},
            {"nodes", nodes}};
}

Json reportedNode(const std::string &node, const std::string &expanded,
                  const Json &outputs) {
    return {{"node", node}, {"expanded", expanded}, {"outputs", outputs}};
}

// Answer whatever observation pair is in the air, so the next thing the test
// asks for is asked rather than skipped: a question already outstanding is not
// asked twice.
void answerObservation(Harness &h,
                       const std::vector<PelicanStudio::InspectorRpcRequest> &requests) {
    for (const auto &request : requests) {
        if (request.method == "get_status")
            h.reply(request, {{"time", 0.0}, {"frame", 0},
                              {"animation", {{"available", false}}}});
        else if (request.method == "get_graph_node_values")
            h.reply(request, {{"instance", request.params.value("instance", std::string{})},
                              {"generation", 0}, {"frame", 0},
                              {"published", false}, {"source", "rendering"},
                              {"nodes", Json::array()}});
    }
}

void settleObservation(Harness &h) {
    answerObservation(h, h.model.takeRpcRequests());
}

const PelicanStudio::GraphCanvasOutput &output(const GraphCanvasNode &node,
                                               const std::string &name) {
    const auto found = std::ranges::find(
        node.outputs, name, &PelicanStudio::GraphCanvasOutput::port);
    REQUIRE(found != node.outputs.end());
    return *found;
}
} // namespace

TEST_CASE("The node values overlay reaches a definition body and the root scope",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    // Naming the instance is what turns the overlay on. Nothing is read for a
    // canvas nobody asked to observe.
    REQUIRE(h.model.takeRpcRequests().empty());
    h.model.watchInstance("left");

    SECTION("a definition body, read through the instance that owns it") {
        // The root scope's question went out on the selection. Descending asks
        // a different one, and only the newest answer of a kind is kept.
        (void)h.model.takeRpcRequests();
        REQUIRE(h.model.selectScope("gain_curve"));
        const auto request = h.take("get_graph_node_values");
        // The scope decides the question: a definition body is read through
        // the instance, the root document through no instance at all.
        REQUIRE(request.params == Json{{"instance", "left"}});
        h.reply(request,
                nodeValues("left", Json::array({
                    reportedNode("gain", "left/gain", {{"value", 2.5}}),
                    reportedNode("bias", "left/bias", {{"value", 5.0}}),
                    reportedNode("sum", "left/sum", {{"value", 7.5}}),
                    // A rendering-domain node has no value of its own. It is
                    // listed with no outputs, which is not the same as absent.
                    reportedNode("buffer", "left/buffer", Json::object())})));

        REQUIRE(h.model.nodeValuesPublished());
        REQUIRE(output(h.node("sum"), "value").has_value);
        REQUIRE(output(h.node("sum"), "value").value == 7.5);
        REQUIRE(output(h.node("gain"), "value").value == 2.5);
        // outputs: {} is a node with no band, not a node with a zero.
        REQUIRE_FALSE(output(h.node("buffer"), "resource").has_value);
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::None);
    }

    SECTION("the root scope, read with no instance at all") {
        const auto request = h.take("get_graph_node_values");
        REQUIRE(request.params == Json::object());
        h.reply(request,
                nodeValues("", Json::array({
                    // A root node answers under its own id: node == expanded.
                    reportedNode("clock", "clock", {{"value", 0.5}}),
                    // The instance body is in the same reply, under the path
                    // that reaches it. No canvas node is called that, so
                    // nothing of the body leaks onto the root drawing.
                    reportedNode("left/sum", "left/sum", {{"value", 7.5}})})));

        REQUIRE(output(h.node("clock"), "value").has_value);
        REQUIRE(output(h.node("clock"), "value").value == 0.5);
        // `left` is a graph.instance: the expansion has no node by that name,
        // so its declared outputs carry no band.
        REQUIRE_FALSE(output(h.node("left"), "value").has_value);
    }

    SECTION("published: false says so and draws no band") {
        (void)h.model.takeRpcRequests();
        REQUIRE(h.model.selectScope("gain_curve"));
        h.reply(h.take("get_graph_node_values"),
                nodeValues("left", Json::array(), false));
        REQUIRE_FALSE(h.model.nodeValuesPublished());
        REQUIRE(h.model.notice().kind == InspectorNoticeKind::Information);
        REQUIRE(h.model.notice().message.find("not published") != std::string::npos);
        REQUIRE_FALSE(output(h.node("sum"), "value").has_value);
    }

    SECTION("an answer about another scope is not drawn against these nodes") {
        (void)h.model.takeRpcRequests();
        REQUIRE(h.model.selectScope("gain_curve"));
        // The reply names the question it answered, and it is the root's.
        h.reply(h.take("get_graph_node_values"),
                nodeValues("", Json::array({
                    reportedNode("sum", "sum", {{"value", 99.0}})})));
        // The reply answers the root's question; the canvas is showing a
        // definition body. A number drawn against the wrong node list is worse
        // than no number.
        REQUIRE_FALSE(output(h.node("sum"), "value").has_value);
    }
}

TEST_CASE("Live reads on a clock and off it the overlay follows the document",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    settleObservation(h);

    // Off: polling asks for nothing. The catalog is already in hand and no
    // edit is outstanding, so the slot stays empty.
    h.model.poll();
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());

    // Every tick of the fake clock is one pair of reads, and the pair has to
    // be answered before the next one goes out: a question already in the air
    // is not asked again, so a slow engine cannot be made to queue reads.
    const auto takePair = [&] {
        auto requests = h.model.takeRpcRequests();
        REQUIRE(requests.size() == 2);
        REQUIRE(requests[0].method == "get_status");
        REQUIRE(requests[1].method == "get_graph_node_values");
        answerObservation(h, requests);
    };
    h.model.setLive(true);
    takePair();
    for (int tick = 0; tick < 3; ++tick) {
        h.model.poll();
        takePair();
    }

    // A tick while the previous pair is still unanswered asks nothing.
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().size() == 2);
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());
    settleObservation(h);

    h.model.setLive(false);
    (void)h.model.takeRpcRequests();
    h.model.poll();
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());
}

TEST_CASE("The Timeline buffers what Live watched go by",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    REQUIRE(h.model.selectScope("gain_curve"));
    h.model.setTimelineNode("sum");
    settleObservation(h);
    h.model.setLive(true);

    const auto tick = [&](double time, double value) {
        for (const auto &request : h.model.takeRpcRequests()) {
            if (request.method == "get_status")
                h.reply(request, {{"time", time}, {"frame", 3},
                                  {"animation", {{"available", false}}}});
            else if (request.method == "get_graph_node_values")
                h.reply(request, nodeValues("left", Json::array({
                    reportedNode("sum", "left/sum", {{"value", value}})})));
        }
        h.model.poll();
    };
    tick(0.25, 7.5);
    tick(0.50, 8.0);
    tick(0.75, 8.5);

    REQUIRE(h.model.timelinePorts() == std::vector<std::string>{"value"});
    const auto &samples = h.model.timelineSamples();
    REQUIRE(samples.size() == 3);
    REQUIRE(samples[0].time == 0.25);
    REQUIRE(samples[0].values == std::vector<double>{7.5});
    REQUIRE(samples[2].time == 0.75);
    REQUIRE(samples[2].values == std::vector<double>{8.5});
    REQUIRE_FALSE(samples[2].has_phase);

    // A different node is a different strip: the lines would otherwise change
    // what they meant halfway along.
    h.model.setTimelineNode("gain");
    REQUIRE(h.model.timelineSamples().empty());
    REQUIRE(h.model.timelinePorts().empty());
}

TEST_CASE("A scrub is set_time and one drawn frame, in that order",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    settleObservation(h);

    REQUIRE(h.model.scrub(1.5));
    // A second scrub while the first is in the air would race it.
    REQUIRE_FALSE(h.model.scrub(2.0));
    auto requests = h.model.takeRpcRequests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].method == "set_time");
    REQUIRE(requests[0].params == Json{{"t", 1.5}});
    REQUIRE(requests[1].method == "render_frame");
    REQUIRE(requests[1].params == Json::object());
    // The scrub does not take the edit slot: a drag on the Timeline must not
    // make the canvas look busy to an edit trying to land.
    REQUIRE(h.model.canEdit());

    // The frame that was asked for has been drawn; the values are read back.
    h.model.receiveRpcResult(requests[1].request_id,
                             Json{{"frame", 90}, {"time", 1.5}}.dump());
    REQUIRE_FALSE(h.model.scrubbing());
    auto after = h.model.takeRpcRequests();
    REQUIRE(after.size() == 2);
    REQUIRE(after[0].method == "get_status");
    REQUIRE(after[1].method == "get_graph_node_values");
}

TEST_CASE("The animation band is drawn only for the watched instance owner",
          "[studio][graph-canvas][ag3b]") {
    Harness h;
    h.open();
    h.model.watchInstance("left");
    // The fixture's `left` is owned by main/41.
    const Json sinks = Json::array({
        Json{{"owner", {{"scene", "main"}, {"object", 41}}},
             {"node", "left/pose"}, {"source", "pose_graph_clip"},
             {"cursors", Json::array({Json{{"clip", "Turn"},
                                           {"time_seconds", 0.5},
                                           {"normalized_phase", 0.25}}})},
             {"transition", {{"active", true}, {"target", "walk"},
                             {"progress", 0.4}}}},
        Json{{"owner", {{"scene", "main"}, {"object", 99}}},
             {"node", "other/pose"}, {"source", "component_clip"},
             {"cursors", Json::array()}}});
    for (const auto &request : h.model.takeRpcRequests())
        if (request.method == "get_status")
            h.reply(request, {{"time", 0.5}, {"frame", 30},
                              {"animation", {{"available", true},
                                             {"sinks", sinks}}}});

    REQUIRE(h.model.animationAvailable());
    const auto owned = h.model.animationSinks();
    REQUIRE(owned.size() == 1);
    REQUIRE(owned[0].node == "left/pose");
    REQUIRE(owned[0].source == "pose_graph_clip");
    REQUIRE(owned[0].cursors.size() == 1);
    REQUIRE(owned[0].cursors[0].clip == "Turn");
    REQUIRE(owned[0].cursors[0].normalized_phase == 0.25);
    REQUIRE(owned[0].transition_active);
    REQUIRE(owned[0].transition_progress == 0.4);

    // Watching an instance the status has no sink for draws no band at all --
    // which is not the same as "the engine reported none".
    h.model.watchInstance("");
    REQUIRE(h.model.animationSinks().empty());
}

// Q-2. The canvas's 250 ms clock keeps running -- it is also what flushes the
// model's request queue after a scope change -- so what matters is that a tick
// asks the engine for something only when one of the three things that read on
// a clock is outstanding: the catalog, Live, or an edit ticket. That is this
// predicate, and it is the reason the canvas costs zero RPC per second while
// idle.
TEST_CASE("The graph canvas polls only for the catalog, Live, or an edit ticket",
          "[studio][graph-canvas][poll][idle][negative-contrast][q2]") {
    Harness h;
    // Disconnected: there is no one to ask.
    REQUIRE_FALSE(h.model.pollWanted());

    // Connecting asks for the document and the catalog itself, on their own
    // slots -- the clock is not what fetches them.
    h.model.setConnected(true);
    auto requests = h.model.takeRpcRequests();
    REQUIRE(requests.size() == 2);
    std::uint64_t catalog_request = 0;
    for (const auto &request : requests) {
        if (request.method == "get_graph_instances")
            h.model.receiveRpcResult(request.request_id, context().dump());
        else if (request.method == "get_graph_catalog")
            catalog_request = request.request_id;
    }
    REQUIRE(catalog_request != 0);

    // A catalog that did not arrive is the one thing a clock has to retry.
    h.model.receiveRpcFailure(catalog_request, "the engine dropped it");
    REQUIRE(h.model.pollWanted());
    h.model.poll();
    h.model.receiveRpcResult(h.take("get_graph_catalog").request_id,
                             catalog().dump());

    // Catalog in hand, Live off, nothing edited: nothing left to ask, and the
    // poll issues nothing even when it is called.
    REQUIRE_FALSE(h.model.pollWanted());
    h.model.poll();
    REQUIRE(h.model.takeRpcRequests().empty());

    // Live reads on a clock by definition.
    h.model.setLive(true);
    REQUIRE(h.model.pollWanted());
    h.model.setLive(false);
    REQUIRE_FALSE(h.model.pollWanted());
}

namespace {
// Run emitted edits through the production document editor. This exercises
// Undo's data restoration as well as the asynchronous ticket/CAS boundary.
struct GraphHistoryHarness {
    Harness client;
    Json document = fixtureDocument();
    int generation = 0;

    std::string digest() const { return "revision-" + std::to_string(generation); }
    Json snapshot() const {
        return {{"source_reference", "fixture"}, {"root_schema", "pelican.graph"},
                {"source_digest", {{"hex", digest()}}},
                {"nodes", document.at("nodes")},
                {"definitions", document.at("definitions")},
                {"layout", document.value("layout", Json::object())}};
    }
    void open() { client.open(snapshot()); }
    void refresh() {
        client.reply(client.take("get_graph_instances"), snapshot());
    }
    // LP-4c: the op list the last commit() sent.
    Json last_ops;
    void commit() {
        const auto submit = client.take("edit_graph_document");
        REQUIRE(submit.params.at("base_source_digest") == digest());
        last_ops = submit.params.at("ops");
        auto edited = Pelican::editGraphDocument(document, submit.params.at("ops"));
        client.reply(submit, {{"status", "accepted"}, {"ticket", "edit"}});
        REQUIRE_FALSE(client.model.canUndo());
        const auto poll = client.take("get_edit_result");
        document = std::move(edited);
        ++generation;
        client.reply(poll, {{"status", "committed"}, {"source_digest", {{"hex", digest()}}}});
        refresh();
    }
};
}

// Q-13. A placement is a commit like any other, and so is every other panel's
// edit: the digest a debounced set_layout carries can be one revision behind.
// The placement is the canvas's own, so it goes once more against the next
// read; a gesture built from the old nodes does not.
TEST_CASE("Q-13 a placement refused for a moved document is written once more "
          "against the revision read next",
          "[studio][graph-canvas][q13]") {
    GraphHistoryHarness h;
    h.open();
    const Json moved{{"status", "rejected"},
                     {"committed", false},
                     {"error",
                      {{"code", "external_modification"},
                       {"message", "render config base digest no longer "
                                   "matches the engine-owned document"}}}};
    const auto edits = [&h] {
        auto requests = h.client.model.takeRpcRequests();
        return std::ranges::count(requests, std::string{"edit_graph_document"},
                                  &PelicanStudio::InspectorRpcRequest::method);
    };

    SECTION("a placement") {
        const Json placement{{"nodes", {{"left", {{"x", 70}, {"y", 80}}}}}};
        REQUIRE(h.client.model.setLayout("", placement));
        const auto first = h.client.take("edit_graph_document");
        CHECK(first.params.at("base_source_digest") == h.digest());
        // Another panel's commit landed first.
        ++h.generation;
        h.client.reply(first, moved);
        h.refresh();
        const auto again = h.client.take("edit_graph_document");
        CHECK(again.params.at("base_source_digest") == h.digest());
        CHECK(again.params.at("ops") == first.params.at("ops"));
        // Refused a second time: once is all it goes again.
        ++h.generation;
        h.client.reply(again, moved);
        h.refresh();
        CHECK(edits() == 0);
    }

    SECTION("a node edit is dropped, as it always was") {
        REQUIRE(h.client.model.setNodeLabel("clock", "Scene clock"));
        const auto first = h.client.take("edit_graph_document");
        ++h.generation;
        h.client.reply(first, moved);
        h.refresh();
        CHECK(edits() == 0);
    }
}

TEST_CASE("Graph Undo restores deleted nodes, order and placement together",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.document["layout"] = {{"nodes", {
        {"clock", {{"x", 11}, {"y", 12}}},
        {"left", {{"x", 40}, {"y", 50}}}}}};
    const auto before = h.document;
    h.open();
    REQUIRE(h.client.model.removeNode("clock"));
    h.commit();
    REQUIRE_FALSE(h.document.at("layout").at("nodes").contains("clock"));
    const auto removed = h.document;
    REQUIRE(h.client.model.canUndo());
    REQUIRE(h.client.model.undo());
    h.commit();
    REQUIRE(h.document == before);
    REQUIRE_FALSE(h.client.model.canUndo());
    REQUIRE(h.client.model.canRedo());
    REQUIRE(h.client.model.redo());
    h.commit();
    REQUIRE(h.document == removed);
}

TEST_CASE("Graph Undo preserves complete nodes and groups seeded additions",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.open();
    const auto before = h.document;
    SECTION("parameter and label edits restore absent fields") {
        REQUIRE(h.client.model.setNodeLabel("clock", "Scene clock"));
        h.commit();
        REQUIRE(h.client.model.setNodeParameter("clock", "name", "other_time"));
        h.commit();
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document.at("nodes")[0].at("label") == "Scene clock");
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document == before);
    }
    SECTION("one add gesture includes its seed constants") {
        REQUIRE(h.client.model.addNode("value.add"));
        h.commit();
        REQUIRE(h.document.at("nodes").size() == before.at("nodes").size() + 3);
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document == before);
        REQUIRE(h.client.model.redo());
        h.commit();
        REQUIRE(h.document.at("nodes").size() == before.at("nodes").size() + 3);
    }
    SECTION("definition edits stay in their own scope") {
        REQUIRE(h.client.model.selectScope("gain_curve"));
        REQUIRE(h.client.model.setNodeLabel("sum", "Sum"));
        h.commit();
        REQUIRE(h.client.model.selectScope(""));
        REQUIRE(h.client.model.undo());
        h.commit();
        REQUIRE(h.document == before);
    }
}

TEST_CASE("Graph Undo restores disconnected edges and their reroutes",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.document["nodes"][1]["inputs"]["gain"] = {{"node", "clock"}, {"port", "value"}};
    h.document["layout"] = {{"reroutes", Json::array({
        {{"edge", {{"node", "left"}, {"port", "gain"}}},
         {"points", Json::array({Json::array({10, 20})})}}})}};
    const auto before = h.document;
    h.open();
    REQUIRE(h.client.model.disconnectInput("left", "gain"));
    h.commit();
    REQUIRE(h.document.at("layout").at("reroutes").empty());
    REQUIRE(h.client.model.undo());
    h.commit();
    REQUIRE(h.document == before);
}

TEST_CASE("Graph history never replays across another writer or a reconnect",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.open();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock"));
    h.commit();
    REQUIRE(h.client.model.canUndo());
    SECTION("external change invalidates history") {
        ++h.generation;
        h.document["nodes"][0]["label"] = "Another writer";
        h.client.model.refresh();
        h.refresh();
    }
    SECTION("reconnect starts a new history") {
        h.client.model.setConnected(false);
        h.open();
    }
    REQUIRE_FALSE(h.client.model.canUndo());
    REQUIRE_FALSE(h.client.model.canRedo());
    REQUIRE_FALSE(h.client.model.undo());
    REQUIRE(h.client.model.takeRpcRequests().empty());
}

TEST_CASE("Q-10 a sound refused while a graph edit is prepared reads as one sentence",
          "[studio][graph-canvas][q10]") {
    GraphHistoryHarness h;
    h.open();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock"));
    const auto submitted = h.client.take("edit_graph_document");
    h.client.reply(submitted, {{"status", "accepted"}, {"ticket", "edit"}});
    const auto poll = h.client.take("get_edit_result");
    // What the render pipeline answers when Audio::prepareSound refuses the
    // asset: its own code, and AU-2's code at the head of the message.
    h.client.reply(poll, {{"status", "rejected"}, {"committed", false},
                          {"error", {{"code", "render_pipeline_preflight_failed"},
                                     {"message", R"(audio.format_unsupported@1 {"asset":"project://a.ogg",)"
                                                 R"("extension":".ogg","signature":"wav"})"},
                                     {"ticket", "edit"}}}});
    const auto notice = h.client.model.notice();
    CHECK(notice.kind == PelicanStudio::InspectorNoticeKind::Error);
    CHECK(notice.message ==
          "Sound extension and contents disagree (WAV or Ogg Vorbis) (audio.format_unsupported@1)");

    // The control: a refusal Studio has no sentence for keeps its own words.
    h.refresh();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock again"));
    h.client.reply(h.client.take("edit_graph_document"),
                   {{"error", {{"code", "invalid_graph_document"}, {"message", "Rejected edit"}}}});
    CHECK(h.client.model.notice().message == "Rejected edit (invalid_graph_document)");
}

TEST_CASE("Rejected graph edits retain history but successful branches discard Redo",
          "[studio][graph-canvas][graph-history]") {
    GraphHistoryHarness h;
    h.open();
    REQUIRE(h.client.model.setNodeLabel("clock", "Clock"));
    h.commit();
    REQUIRE(h.client.model.undo());
    h.commit();
    REQUIRE(h.client.model.canRedo());
    REQUIRE(h.client.model.setNodeLabel("clock", "Rejected"));
    const auto submitted = h.client.take("edit_graph_document");
    h.client.reply(submitted, {{"error", {{"message", "Rejected edit"}}}});
    h.refresh();
    REQUIRE(h.client.model.canRedo());
    REQUIRE(h.client.model.setNodeLabel("clock", "New branch"));
    h.commit();
    REQUIRE_FALSE(h.client.model.canRedo());
    REQUIRE(h.client.model.canUndo());
}

// --- LP-4c: graph.loop on the Graph tab.

namespace {

Json lp4cColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

// A loop body: a boundary port `target`, and a passthrough drawing into an
// image of its own, which is the body's one output.
Json lp4cBody(const std::string &pass) {
    const auto type = lp4cColorType();
    return Json{{"nodes", Json::array({
        {{"id", "target"}, {"op", "graph.input"}},
        {{"id", "image"}, {"op", "render.image"},
         {"parameters", {{"type", type}, {"format", "R16G16B16A16_SFLOAT"}, {"extent_scale", 1.0},
                         {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}},
         {"outputs", {"resource"}}},
        {{"id", pass}, {"op", "render.fullscreen"},
         {"parameters", {{"pass_set", "main_render"},
                         {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}},
                         {"ports", {{"color", {{"kind", "image"}, {"type", type}, {"access", "write"},
                                               {"attachment", "color"}, {"location", 0}}}}}}},
         {"inputs", {{"color", {{"node", "image"}, {"port", "resource"}}}}},
         {"outputs", {"color"}}}})},
        {"outputs", {{"image", {{"node", pass}, {"port", "color"}}}}}};
}

// Element 0 runs `first`, every other element `paint`; `spare` is a body
// nothing runs, with another boundary.
Json lp4cDocument() {
    return Json{{"schema", "pelican.graph"}, {"version", 1},
        {"definitions", {{"first", lp4cBody("first_fill")}, {"paint", lp4cBody("fill")},
                         {"spare", {{"nodes", Json::array({{{"id", "gain"}, {"op", "graph.input"},
                                                            {"default", 1.0}}})},
                                    {"outputs", Json::object()}}}}},
        {"nodes", Json::array({
            {{"id", "lit"}, {"op", "render.target_ref"},
             {"parameters", {{"name", "lit_color"}, {"type", lp4cColorType()}}}, {"outputs", {"resource"}}},
            {{"id", "steps"}, {"op", "graph.loop"},
             {"parameters", {{"over", {{"range", {{"count", 3}}}}},
                             {"branches", Json::array({{{"when", {{"element", 0}}}, {"body", "first"}},
                                                       {{"body", "paint"}}})}}},
             {"inputs", {{"target", {{"node", "lit"}, {"port", "resource"}}}}},
             {"outputs", {"image", "image[]"}}}})},
        {"outputs", Json::object()}};
}

// The catalog with the render operations, which declare no ports of their
// own (parameter_dependent), as the engine publishes them.
Json lp4cCatalog() {
    auto reply = catalog();
    auto &render = reply["modules"][1]["operations"];
    for (const char *op : {"render.image", "render.target_ref", "render.fullscreen"})
        render.push_back({{"op", op}, {"domain", "rendering"}, {"evaluation", "direct"},
                          {"sink", false}, {"ports", "parameter_dependent"}});
    return reply;
}

// get_frame_plan's answer for the loop: element 0 on branch 0 and the others
// on branch 1.
Json lp4cFramePlan(std::int64_t count = 3) {
    auto elements = Json::array();
    for (std::int64_t element = 0; element < count; ++element)
        elements.push_back({{"element", element}, {"index", element}, {"branch", element == 0 ? 0 : 1}});
    return {{"schema", "pelican.frame_plan"}, {"version", 1},
            {"loops", Json::array({{{"node", "steps"}, {"tier", "expand"}, {"count", count},
                                    {"elements", elements},
                                    {"unmatched_branches", Json::array()}}})}};
}

struct LoopHarness : GraphHistoryHarness {
    LoopHarness() {
        document = lp4cDocument();
        client.catalog_reply = lp4cCatalog();
    }
    // The loop report the model asks for once the document holds a loop.
    void report(const Json &plan = lp4cFramePlan()) {
        client.reply(client.take("get_frame_plan"), plan);
    }
    // One gesture's op list, refused, so the next gesture starts clean.
    Json refuse(const Json &error = Json{{"code", "invalid_graph_document"},
                                         {"message", "refused in the test"}}) {
        const auto submit = client.take("edit_graph_document");
        const auto ops = submit.params.at("ops");
        client.reply(submit, {{"status", "rejected"}, {"committed", false}, {"error", error}});
        refresh();
        return ops;
    }
    Json parameters(const Json &ops) {
        REQUIRE(ops.size() == 1);
        REQUIRE(ops[0].at("op") == "set_node_parameters");
        CHECK(ops[0].at("scope") == "root");
        CHECK(ops[0].at("node") == "steps");
        return ops[0].at("parameters");
    }
};

} // namespace

TEST_CASE("LP-4c a loop's ports are its body's boundary and its parameters are not ports",
          "[studio][graph-canvas][lp4c]") {
    REQUIRE_NOTHROW(Pelican::editGraphDocument(lp4cDocument(), Json::array()));
    LoopHarness h;
    h.open();
    const auto loop = h.client.node("steps");
    CHECK(loop.loop);
    CHECK_FALSE(loop.instance);
    // The input ports are the first body's graph.input nodes, and nothing else:
    // over and branches are the Loop fields.
    REQUIRE(loop.inputs.size() == 1);
    CHECK(loop.inputs[0].port == "target");
    CHECK(loop.inputs[0].connected);
    CHECK(loop.inputs[0].source_node == "lit");
    CHECK_FALSE(loop.inputs[0].has_parameter);
    CHECK(std::ranges::none_of(loop.inputs, [](const auto &port) {
        return port.port == "over" || port.port == "branches";
    }));
    // The outputs are the p and p[] it declares; p[] is a bundle.
    REQUIRE(loop.outputs.size() == 2);
    CHECK_FALSE(output(loop, "image").bundle);
    CHECK(output(loop, "image[]").bundle);
    // A descent opens the first body.
    CHECK(loop.definition == "first");
    CHECK(h.client.model.canEnterInstance("steps"));
    // Before the report the heading says loop; after it, the tier and count.
    CHECK(loop.badge == "↻ loop");
    h.report();
    CHECK(h.client.node("steps").badge == "↻ expand ×3");

    const auto read = h.client.model.loop("steps");
    REQUIRE(read);
    CHECK(read->count == 3);
    CHECK_FALSE(read->count_expression);
    CHECK(read->body == "first");
    CHECK(read->body_inputs == std::vector<std::string>{"target"});
    CHECK(read->body_outputs == std::vector<std::string>{"image"});
    REQUIRE(read->branches.size() == 2);
    CHECK(read->branches[0].when == "element");
    CHECK(read->branches[0].values == std::vector<std::int64_t>{0});
    CHECK(read->branches[0].matched == std::vector<std::int64_t>{0});
    CHECK(read->branches[1].when == "else");
    CHECK(read->branches[1].matched == std::vector<std::int64_t>{1, 2});
    CHECK(read->reported_count == 3);
    // Only the boundary-sharing definitions are a branch's choices.
    CHECK(h.client.model.loopBodyChoices("steps") == std::vector<std::string>{"first", "paint"});
}

TEST_CASE("LP-4c Add loop sends the new body and the loop in one list",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    CHECK(h.client.model.suggestDefinitionName() == "loop_body");
    SECTION("over a new, empty definition") {
        REQUIRE(h.client.model.addLoop({}, "loop_body"));
        h.commit();
        CHECK(h.last_ops == Json::array({
            {{"op", "add_definition"}, {"name", "loop_body"}},
            {{"op", "add_node"}, {"scope", "root"},
             {"node", {{"id", "loop"}, {"op", "graph.loop"},
                       {"parameters", {{"over", {{"range", {{"count", 1}}}}},
                                       {"branches", Json::array({{{"body", "loop_body"}}})}}},
                       {"outputs", Json::array()}}}}}));
        CHECK(h.document.at("definitions").at("loop_body") ==
              Json{{"nodes", Json::array()}, {"outputs", Json::object()}});
        CHECK(h.client.model.suggestDefinitionName() == "loop_body_2");
    }
    SECTION("over an existing body, declaring its outputs as p") {
        REQUIRE(h.client.model.addLoop("paint"));
        // Its boundary port has no loop input yet, which the engine refuses:
        // the list is what is looked at here.
        const auto ops = h.refuse();
        REQUIRE(ops.size() == 1);
        CHECK(ops[0].at("op") == "add_node");
        CHECK(ops[0].at("node").at("parameters").at("branches") ==
              Json::array({{{"body", "paint"}}}));
        CHECK(ops[0].at("node").at("outputs") == Json::array({"image"}));
    }
    SECTION("the first pass put in the new body gives the loop a place, in the same list") {
        REQUIRE(h.client.model.addLoop({}, "loop_body"));
        h.commit();
        REQUIRE(h.client.model.enterInstance("loop"));
        REQUIRE(h.client.model.currentScope() == "loop_body");
        // A fullscreen pass arrives whole: an image of its own, the pass
        // drawing the engine's passthrough into it.
        REQUIRE(h.client.model.addNode("render.fullscreen"));
        h.commit();
        REQUIRE(h.last_ops.size() == 3);
        CHECK(h.last_ops[0].at("node").at("op") == "render.image");
        CHECK(h.last_ops[1].at("node").at("op") == "render.fullscreen");
        CHECK(h.last_ops[1].at("node").at("inputs").at("color").at("node") ==
              h.last_ops[0].at("node").at("id"));
        CHECK(h.last_ops[2].at("op") == "set_node_parameters");
        CHECK(h.last_ops[2].at("node") == "loop");
        CHECK(h.last_ops[2].at("parameters").at("canonical_anchor") == "post_main");
        CHECK(h.document.at("definitions").at("loop_body").at("nodes").size() == 2);
        // A second pass leaves the place the loop has.
        REQUIRE(h.client.model.addNode("render.fullscreen"));
        h.commit();
        CHECK(h.last_ops.size() == 2);
        // The Place field moves it; an empty choice takes it away.
        REQUIRE(h.client.model.selectScope(""));
        const auto read = h.client.model.loop("loop");
        REQUIRE(read);
        CHECK(read->body_draws);
        CHECK(read->anchor == "post_main");
        REQUIRE(h.client.model.setLoopAnchor("loop", "post_ldr"));
        h.commit();
        CHECK(h.document.at("nodes").back().at("parameters").at("canonical_anchor") == "post_ldr");
        CHECK_FALSE(h.client.model.setLoopAnchor("loop", "nowhere"));
    }
    // A loop stands in the root only; the palette says so.
    const auto root_palette = h.client.model.paletteOperations("graph.loop");
    CHECK(std::ranges::any_of(root_palette, [](const auto &entry) { return entry.op == "graph.loop"; }));
    REQUIRE(h.client.model.selectScope("paint"));
    CHECK(h.client.model.addLoopOps("paint").empty());
    CHECK(h.client.model.paletteOperations("graph.loop").empty());
}

TEST_CASE("LP-4c the Loop fields each send the loop's parameters with one member changed",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    const auto base = h.document.at("nodes").at(1).at("parameters");
    const auto with = [&](const char *key, const Json &value) {
        auto expected = base;
        expected[key] = value;
        return expected;
    };
    REQUIRE(h.client.model.setLoopCount("steps", 5));
    CHECK(h.parameters(h.refuse()) == with("over", {{"range", {{"count", 5}}}}));
    REQUIRE(h.client.model.setLoopCountExpression("steps", "2 + 1"));
    CHECK(h.parameters(h.refuse()) == with("over", {{"range", {{"count", {{"$expr", "2 + 1"}}}}}}));
    REQUIRE(h.client.model.setLoopDescending("steps", true));
    CHECK(h.parameters(h.refuse()) == with("order", {{"by", "element"}, {"direction", "descending"}}));
    REQUIRE(h.client.model.setLoopLimit("steps", 2));
    CHECK(h.parameters(h.refuse()) == with("limit", 2));
    REQUIRE(h.client.model.setLoopTier("steps", "draw_loop"));
    CHECK(h.parameters(h.refuse()) == with("tier", "draw_loop"));
    REQUIRE(h.client.model.setLoopWhere("steps", "element_not_in", {1, 4}));
    CHECK(h.parameters(h.refuse()) == with("where", {{"element_not_in", {1, 4}}}));
    REQUIRE(h.client.model.setLoopWhere("steps", "element_in", {2}));
    CHECK(h.parameters(h.refuse()) == with("where", {{"element_in", {2}}}));
    REQUIRE(h.client.model.setLoopCarry("steps", {{"image", "target"}}));
    CHECK(h.parameters(h.refuse()) ==
          with("carry", Json::array({{{"from", "image"}, {"to", "target"}}})));
    // Setting what is already there sends nothing.
    CHECK_FALSE(h.client.model.setLoopDescending("steps", false));
    CHECK_FALSE(h.client.model.setLoopLimit("steps", std::nullopt));
    CHECK_FALSE(h.client.model.setLoopTier("steps", ""));
    // The outputs are the node's own member: one replace_node.
    REQUIRE(h.client.model.setLoopOutputs("steps", {"image[]"}));
    const auto replaced = h.refuse();
    REQUIRE(replaced.size() == 1);
    CHECK(replaced[0].at("op") == "replace_node");
    auto expected = h.document.at("nodes").at(1);
    expected["outputs"] = Json::array({"image[]"});
    CHECK(replaced[0].at("node") == expected);
    // And the ones the engine takes commit.
    REQUIRE(h.client.model.setLoopCount("steps", 2));
    h.commit();
    REQUIRE(h.client.model.setLoopDescending("steps", true));
    h.commit();
    REQUIRE(h.client.model.setLoopLimit("steps", 1));
    h.commit();
    REQUIRE(h.client.model.setLoopWhere("steps", "element_not_in", {1}));
    h.commit();
    const auto read = h.client.model.loop("steps");
    REQUIRE(read);
    CHECK(read->count == 2);
    CHECK(read->descending);
    CHECK(read->limit == std::optional<std::int64_t>{1});
    CHECK(read->where == "element_not_in");
    CHECK(read->where_values == std::vector<std::int64_t>{1});
}

TEST_CASE("LP-4c the branch table adds, removes, orders and conditions branches",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    const auto branches = [&] {
        return h.document.at("nodes").at(1).at("parameters").at("branches");
    };
    // Added in front of the else, running the else's body.
    REQUIRE(h.client.model.addLoopBranch("steps"));
    h.commit();
    CHECK(branches() == Json::array({{{"when", {{"element", 0}}}, {"body", "first"}},
                                     {{"when", {{"element", 0}}}, {"body", "paint"}},
                                     {{"body", "paint"}}}));
    REQUIRE(h.client.model.setLoopBranchWhen("steps", 1, "element_in", {1, 2}));
    h.commit();
    CHECK(branches()[1].at("when") == Json{{"element_in", {1, 2}}});
    REQUIRE(h.client.model.setLoopBranchWhen("steps", 1, "element_not_in", {0}));
    h.commit();
    CHECK(branches()[1].at("when") == Json{{"element_not_in", {0}}});
    REQUIRE(h.client.model.setLoopBranchWhen("steps", 1, "element", {2}));
    h.commit();
    CHECK(branches()[1].at("when") == Json{{"element", 2}});
    REQUIRE(h.client.model.moveLoopBranch("steps", 1, -1));
    h.commit();
    CHECK(branches()[0].at("when") == Json{{"element", 2}});
    CHECK(branches()[1].at("body") == "first");
    // (nothing) is the null body.
    REQUIRE(h.client.model.setLoopBranchBody("steps", 0, std::nullopt));
    h.commit();
    CHECK(branches()[0].at("body").is_null());
    REQUIRE(h.client.model.setLoopBranchBody("steps", 0, std::string{"paint"}));
    h.commit();
    CHECK(branches()[0].at("body") == "paint");
    REQUIRE(h.client.model.removeLoopBranch("steps", 0));
    h.commit();
    CHECK(branches().size() == 2);
    // The else is the last row: never removed, moved or conditioned.
    CHECK_FALSE(h.client.model.removeLoopBranch("steps", 1));
    CHECK_FALSE(h.client.model.moveLoopBranch("steps", 0, 1));
    CHECK_FALSE(h.client.model.setLoopBranchWhen("steps", 1, "element", {3}));
    CHECK(std::ranges::none_of(h.client.model.takeRpcRequests(), [](const auto &request) {
        return request.method == "edit_graph_document";
    }));

    // New definition...: the loop's boundary in a definition of its own, and
    // the branch running it, in one list. The body's output is made by a
    // pass, so the body is copied whole.
    REQUIRE(h.client.model.newLoopBranchDefinition("steps", 0, "first_copy"));
    h.commit();
    REQUIRE(h.last_ops.size() == 2);
    CHECK(h.last_ops[0] == Json{{"op", "add_definition"}, {"name", "first_copy"},
                                {"definition", lp4cBody("first_fill")}});
    CHECK(h.last_ops[1].at("op") == "set_node_parameters");
    CHECK(branches()[0].at("body") == "first_copy");
    // A body whose outputs come off its boundary is copied empty: only its
    // graph.input nodes.
    auto passthrough = lp4cDocument();
    passthrough["definitions"]["first"] = Json{
        {"nodes", Json::array({{{"id", "target"}, {"op", "graph.input"}}})},
        {"outputs", {{"image", {{"node", "target"}, {"port", "value"}}}}}};
    passthrough["definitions"]["paint"] = passthrough["definitions"]["first"];
    LoopHarness empty;
    empty.document = passthrough;
    empty.open();
    REQUIRE(empty.client.model.newLoopBranchDefinition("steps", 1, "blank"));
    const auto ops = empty.refuse();
    CHECK(ops[0].at("definition") == passthrough["definitions"]["first"]);
}

TEST_CASE("LP-4c Per iteration reads one element of another loop's p[]",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    auto reader = h.document.at("nodes").at(1);
    reader["id"] = "reader";
    reader["outputs"] = Json::array();
    h.document["nodes"].push_back(reader);
    h.open();
    const auto sources = h.client.model.perIterationSources("reader");
    CHECK(sources == std::vector<std::pair<std::string, std::string>>{{"steps", "image[]"}});
    // A loop does not read its own p[].
    CHECK(h.client.model.perIterationSources("steps").empty());
    REQUIRE(h.client.model.connectPerIteration("reader", "target", "steps", "image[]"));
    CHECK(h.refuse() == Json::array({{{"op", "set_input"}, {"scope", "root"}, {"node", "reader"},
                                      {"port", "target"},
                                      {"source", {{"node", "steps"}, {"port", "image[]"},
                                                  {"element", {{"$expr", "element"}}}}}}}));
    REQUIRE(h.client.model.connectPerIteration("reader", "target", "steps", "image[]",
                                               "element - 1"));
    CHECK(h.refuse()[0].at("source").at("element") == Json{{"$expr", "element - 1"}});
    CHECK_FALSE(h.client.model.connectPerIteration("reader", "target", "lit", "resource"));
}

TEST_CASE("LP-4c Undo inverts each op against what the ops before it left",
          "[studio][graph-canvas][lp4c][graph-history]") {
    LoopHarness h;
    h.document["nodes"].push_back(
        {{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "scene_time"}}},
         {"outputs", {"value"}}});
    h.open();
    const auto before = h.document;
    const auto roundTrip = [&](const Json &after) {
        REQUIRE(h.client.model.canUndo());
        REQUIRE(h.client.model.undo());
        h.commit();
        CHECK(h.document == before);
        CHECK(h.document.dump(2) == before.dump(2));
        REQUIRE(h.client.model.redo());
        h.commit();
        CHECK(h.document == after);
        CHECK(h.document.dump(2) == after.dump(2));
    };
    SECTION("add_definition, then a loop over it") {
        REQUIRE(h.client.model.addLoop({}, "loop_body"));
        h.commit();
        roundTrip(h.document);
    }
    SECTION("add_node, then set_input naming the node just added") {
        REQUIRE(h.client.model.submitEdit(Json::array({
            {{"op", "add_node"}, {"scope", "root"},
             {"node", {{"id", "sum"}, {"op", "value.add"},
                       {"inputs", {{"b", {{"node", "clock"}, {"port", "value"}}}}},
                       {"outputs", {"value"}}}}},
            {{"op", "set_input"}, {"scope", "root"}, {"node", "sum"}, {"port", "a"},
             {"source", {{"node", "clock"}, {"port", "value"}}}}})));
        h.commit();
        CHECK(h.document.at("nodes").back().at("inputs").at("a").at("node") == "clock");
        roundTrip(h.document);
    }
    SECTION("rename_definition, with the loop's branch following it") {
        REQUIRE(h.client.model.renameDefinition("paint", "brush"));
        h.commit();
        CHECK(h.document.at("nodes").at(1).at("parameters").at("branches").at(1).at("body") == "brush");
        roundTrip(h.document);
    }
    SECTION("remove_definition comes back with its body") {
        REQUIRE(h.client.model.removeDefinition("spare"));
        h.commit();
        CHECK_FALSE(h.document.at("definitions").contains("spare"));
        CHECK(h.client.model.undo());
        h.commit();
        CHECK(h.document == before);
        CHECK(h.document.dump(2) == before.dump(2));
    }
}

TEST_CASE("LP-4c loop and definition refusals read as sentences and mark where they point",
          "[studio][graph-canvas][lp4c]") {
    using PelicanStudio::graphLoopRefusalDescription;
    // The engine lays the fields beside the code.
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_branch_else_missing@1"}, {"node", "steps"}}) ==
          "Loop steps has no else: its last branch must have no condition "
          "(graph.loop_branch_else_missing@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_branch_interface@1"}, {"node", "steps"},
                                       {"branch", 1}, {"definition", "spare"}, {"port", "gain"}}) ==
          "Branch 1 of loop steps runs spare, whose inputs and outputs differ from the loop's at gain "
          "(graph.loop_branch_interface@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_expression_invalid@1"}, {"node", "steps"},
                                       {"pointer", "/nodes/1/parameters/over/range/count"},
                                       {"reason", "syntax"}}) ==
          "An expression of loop steps is not valid: syntax at /nodes/1/parameters/over/range/count "
          "(graph.loop_expression_invalid@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_element_unknown@1"}, {"node", "reader"},
                                       {"port", "target"}, {"element", 7}}) ==
          "Input target of reader reads element 7, which the loop it reads does not have "
          "(graph.loop_element_unknown@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.loop_expansion_limit@1"}, {"node", "steps"},
                                       {"count", 100}, {"limit", 64}}) ==
          "Loop steps would make 100 copies; the limit is 64 (graph.loop_expansion_limit@1).");
    CHECK(graphLoopRefusalDescription({{"code", "graph.definition_referenced@1"}, {"definition", "paint"},
                                       {"referenced_by", {"steps", "other"}}}) ==
          "Definition paint is still used by steps, other (graph.definition_referenced@1).");
    // Quoted in a message, as a GraphRegistryError spells itself.
    CHECK(graphLoopRefusalDescription(
              {{"code", "render_pipeline_preflight_failed"},
               {"message", "graph.loop_expansion_limit@1 {\"count\":70,\"limit\":64,\"node\":\"steps\"}"}}) ==
          "Loop steps would make 70 copies; the limit is 64 (graph.loop_expansion_limit@1).");
    CHECK(graphLoopRefusalDescription({{"code", "invalid_graph_document"}, {"message", "x"}}).empty());

    LoopHarness h;
    h.open();
    SECTION("a loop refused by name is marked") {
        REQUIRE(h.client.model.setLoopCount("steps", 100));
        h.refuse({{"code", "graph.loop_expansion_limit@1"}, {"message", "limit"},
                  {"node", "steps"}, {"count", 100}, {"limit", 64}});
        CHECK(h.client.model.notice().message ==
              "Loop steps would make 100 copies; the limit is 64 (graph.loop_expansion_limit@1).");
        CHECK(h.client.node("steps").refused);
        CHECK_FALSE(h.client.node("lit").refused);
        // An edit that commits takes the mark away.
        REQUIRE(h.client.model.setLoopCount("steps", 2));
        h.commit();
        CHECK_FALSE(h.client.node("steps").refused);
    }
    SECTION("an element's copy marks the loop and the body node of that element's branch") {
        h.report();
        REQUIRE(h.client.model.setLoopCount("steps", 4));
        h.refuse({{"code", "graph.loop_expression_invalid@1"}, {"message", "refused"},
                  {"node", "steps/2/fill"}, {"pointer", "/definitions/paint/nodes/2"},
                  {"reason", "not_static"},
                  {"loop_origin", {{"loop", "steps"}, {"element", 2}, {"node", "fill"}}}});
        CHECK(h.client.model.refusalMarks().size() == 2);
        CHECK(h.client.node("steps").refused);
        REQUIRE(h.client.model.selectScope("paint"));
        CHECK(h.client.node("fill").refused);
        REQUIRE(h.client.model.selectScope("first"));
        CHECK_FALSE(h.client.node("first_fill").refused);
    }
    SECTION("a graph variant that refused it is named first") {
        REQUIRE(h.client.model.setLoopCount("steps", 100));
        h.refuse({{"code", "graph.loop_expansion_limit@1"}, {"variant", "preview"},
                  {"node", "steps"}, {"count", 100}, {"limit", 64}});
        CHECK(h.client.model.notice().message ==
              "The preview graph refused it: Loop steps would make 100 copies; the limit is 64 "
              "(graph.loop_expansion_limit@1).");
    }
}

TEST_CASE("LP-4c a descent into a loop opens its chosen branch, and a copy's origin opens its body",
          "[studio][graph-canvas][lp4c]") {
    LoopHarness h;
    h.open();
    REQUIRE(h.client.model.enterInstance("steps"));
    CHECK(h.client.model.currentScope() == "first");
    CHECK(h.client.model.enteredLoop() == "steps");
    CHECK(h.client.model.definitionUseCount("first") == 1);
    // The branch combo swaps the body in place.
    REQUIRE(h.client.model.selectLoopBranch(1));
    CHECK(h.client.model.currentScope() == "paint");
    CHECK(h.client.model.breadcrumb() == std::vector<std::string>{"", "paint"});
    REQUIRE(h.client.model.leaveScope());
    CHECK(h.client.model.enteredLoop().empty());
    // The branch chosen is the one opened next time.
    REQUIRE(h.client.model.enterInstance("steps"));
    CHECK(h.client.model.currentScope() == "paint");
    REQUIRE(h.client.model.leaveScope());
    // Decision 10: element 0 ran `first`, element 2 `paint` (the report).
    h.report();
    REQUIRE(h.client.model.enterLoopOrigin("steps", 2, "fill"));
    CHECK(h.client.model.currentScope() == "paint");
    REQUIRE(h.client.model.enterLoopOrigin("steps", 0, "first_fill"));
    CHECK(h.client.model.currentScope() == "first");
    CHECK_FALSE(h.client.model.enterLoopOrigin("steps", 0, "fill"));
    // A definition two loops run is shared.
    auto shared = lp4cDocument();
    auto second = shared.at("nodes").at(1);
    second["id"] = "again";
    shared["nodes"].push_back(second);
    LoopHarness twice;
    twice.document = shared;
    twice.open();
    CHECK(twice.client.model.definitionUseCount("paint") == 2);
}
