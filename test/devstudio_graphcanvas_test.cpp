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
    void commit() {
        const auto submit = client.take("edit_graph_document");
        REQUIRE(submit.params.at("base_source_digest") == digest());
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
