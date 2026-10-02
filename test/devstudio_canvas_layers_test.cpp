// SG-1: pelican.canvas in the Graph tab, without Qt -- a canvas.stack's
// sockets, the Layers panel's rows against the document's `layers`, and the
// op lists its gestures send (one edit each).
#include "graphcanvaslayerstack.hpp"
#include "graphcanvasmodel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace PelicanStudio;

GraphCanvasNode node(std::string id, std::string op, Json parameters = Json::object()) {
    GraphCanvasNode made;
    made.id = std::move(id);
    made.op = std::move(op);
    made.parameters = std::move(parameters);
    return made;
}

void connect(GraphCanvasNode &to, std::string port, std::string source, std::string source_port,
             std::string type = {}) {
    GraphCanvasPort made;
    made.port = std::move(port);
    made.type = std::move(type);
    made.connected = true;
    made.source_node = std::move(source);
    made.source_port = std::move(source_port);
    to.inputs.push_back(std::move(made));
}

// A stack the way the canvas reads it: its ports from its `layers`, the
// connected ones with their lines.
GraphCanvasNode stack(std::string id, Json parameters, const std::vector<std::pair<std::string, std::string>> &lines) {
    auto made = node(std::move(id), "canvas.stack", parameters);
    made.inputs = graphCanvasStackPorts(parameters);
    for (const auto &[port, source] : lines)
        for (auto &input : made.inputs)
            if (input.port == port) {
                input.connected = true;
                input.source_node = source;
                input.source_port = source.ends_with("value") ? "value" : "layer";
            }
    return made;
}

// rect_1 filled and stroked, ellipse_1 filled, a group, in `hud`.
std::vector<GraphCanvasNode> document() {
    std::vector<GraphCanvasNode> nodes;
    nodes.push_back(node("rect_1", "shape.rect", Json{{"size", {320, 180}}, {"center", {640, 360}}}));
    nodes.push_back(node("rect_1_fill", "shape.fill", Json{{"color", {0.2, 0.45, 0.9, 1}}}));
    connect(nodes.back(), "geometry", "rect_1", "geometry");
    nodes.push_back(node("rect_1_stroke", "shape.stroke", Json{{"color", {1, 1, 1, 1}}, {"width", 4}}));
    connect(nodes.back(), "geometry", "rect_1", "geometry");
    nodes.push_back(node("ellipse_1", "shape.ellipse", Json{{"size", {320, 180}}, {"center", {640, 360}}}));
    nodes.push_back(node("ellipse_1_fill", "shape.fill", Json{{"color", {0.95, 0.55, 0.15, 1}}}));
    connect(nodes.back(), "geometry", "ellipse_1", "geometry");
    nodes.push_back(node("group_1", "canvas.stack", Json{{"size", {1280, 720}}, {"layers", Json::array()}}));
    nodes.back().inputs = graphCanvasStackPorts(nodes.back().parameters);
    nodes.push_back(node("pulse_value", "value.constant", Json{{"value", 0.5}}));
    nodes.push_back(stack("hud",
                          Json{{"size", {1280, 720}},
                               {"layers", {{{"input", "rect_1"}, {"name", "Card"}},
                                           {{"input", "rect_1_stroke"}, {"name", "Card stroke"}},
                                           {{"input", "ellipse_1"}, {"visible", false}, {"opacity", 0.5}},
                                           {{"input", "group_1"}, {"name", "Group 1"}}}}},
                          {{"rect_1", "rect_1_fill"},
                           {"rect_1_stroke", "rect_1_stroke"},
                           {"ellipse_1", "ellipse_1_fill"},
                           {"ellipse_1.opacity", "pulse_value"},
                           {"group_1", "group_1"}}));
    auto out = node("screen", "canvas.output");
    connect(out, "layer", "hud", "layer", std::string{graphCanvasLayerType});
    nodes.push_back(std::move(out));
    return nodes;
}

} // namespace

TEST_CASE("SG-1: canvas.*, shape.* and filter.* are the canvas domain, and the palette offers "
          "canvas.output as the Canvas",
          "[studio][canvas][sg1]") {
    CHECK(GraphCanvasModel::opDomain("canvas.stack") == "canvas");
    CHECK(GraphCanvasModel::opDomain("shape.rect") == "canvas");
    CHECK(GraphCanvasModel::opDomain("filter.blur") == "canvas");
    CHECK(GraphCanvasModel::opDomain("render.raster") == "render");
    const auto &domains = GraphCanvasModel::domainNames();
    CHECK(std::ranges::find(domains, std::string{"canvas"}) != domains.end());
    CHECK(graphCanvasPaletteLabel("canvas.output").value_or("").starts_with("Canvas"));
    CHECK_FALSE(graphCanvasPaletteLabel("canvas.stack"));
}

TEST_CASE("SG-1: a stack's sockets follow its layers -- a layer and its opacity each, then "
          "+ layer",
          "[studio][canvas][sg1]") {
    const auto ports = graphCanvasStackPorts(
        Json{{"size", {64, 64}}, {"layers", {{{"input", "a"}}, {{"input", "b"}, {"name", "B"}}}}});
    std::vector<std::string> names;
    for (const auto &port : ports) names.push_back(port.port);
    CHECK(names == std::vector<std::string>{"a", "a.opacity", "b", "b.opacity", "+ layer"});
    CHECK(ports[0].type == std::string{graphCanvasLayerType});
    CHECK(ports[1].type == "pelican.value.scalar@1");
    CHECK(ports[4].type == std::string{graphCanvasLayerType});
    CHECK_FALSE(ports[4].connected);
}

TEST_CASE("SG-1: the Layers rows are the document's layers top first", "[studio][canvas][sg1]") {
    const auto nodes = document();
    const auto rows = graphCanvasLayerRows(nodes, "hud");
    REQUIRE(rows.size() == 4);
    // Top first: the last element of `layers` is the first row.
    CHECK(rows[0].index == 3);
    CHECK(rows[0].name == "Group 1");
    CHECK(rows[0].kind == "group");
    CHECK(rows[1].index == 2);
    CHECK(rows[1].name == "ellipse_1");
    CHECK_FALSE(rows[1].visible);
    CHECK(rows[1].opacity == 0.5);
    CHECK(rows[1].opacity_driven);
    CHECK(rows[1].kind == "ellipse");
    CHECK(rows[1].paint == "fill");
    CHECK(rows[1].shape == "ellipse_1");
    CHECK(rows[2].paint == "stroke");
    CHECK(rows[2].source == "rect_1_stroke");
    CHECK(graphCanvasLayerKindText(rows[2]) == "Rect stroke");
    CHECK(rows[3].index == 0);
    CHECK(rows[3].name == "Card");
    CHECK(rows[3].source == "rect_1_fill");
    CHECK(rows[3].visible);
    CHECK(rows[3].opacity == 1.0);

    // The stack the panel shows for a pick, and the row a pick stands for.
    CHECK(graphCanvasStackFor(nodes, "screen") == "hud");
    CHECK(graphCanvasStackFor(nodes, "hud") == "hud");
    CHECK(graphCanvasStackFor(nodes, "ellipse_1_fill") == "hud");
    CHECK(graphCanvasStackFor(nodes, "rect_1") == "hud");
    CHECK(graphCanvasStackFor(nodes, "pulse_value").empty());
    CHECK(graphCanvasRowFor(nodes, "hud", "rect_1_stroke") == std::optional<std::size_t>{1});
    // A shape two rows paint stands for the upper one.
    CHECK(graphCanvasRowFor(nodes, "hud", "rect_1") == std::optional<std::size_t>{1});
    CHECK(graphCanvasRowFor(nodes, "hud", "rect_1_fill") == std::optional<std::size_t>{0});
}

TEST_CASE("SG-1: + Rect is the shape, its fill and a layer on top, in one list",
          "[studio][canvas][sg1]") {
    const auto nodes = document();
    const auto ops = graphCanvasAddShapeLayerOps(nodes, "", "hud", "rect");
    // rect_1 is taken: rect_2, a quarter of the stack at its middle.
    const Json expected = Json::array({
        {{"op", "add_node"}, {"scope", "root"},
         {"node", {{"id", "rect_2"}, {"op", "shape.rect"},
                   {"parameters", {{"size", {320.0, 180.0}}, {"center", {640.0, 360.0}},
                                   {"corner_radius", 0.0}, {"rotation", 0.0}}},
                   {"outputs", {"geometry"}}}}},
        {{"op", "add_node"}, {"scope", "root"},
         {"node", {{"id", "rect_2_fill"}, {"op", "shape.fill"},
                   {"parameters", {{"color", {0.2, 0.45, 0.9, 1.0}}}},
                   {"inputs", {{"geometry", {{"node", "rect_2"}, {"port", "geometry"}}}}},
                   {"outputs", {"layer"}}}}},
        {{"op", "set_node_parameters"}, {"scope", "root"}, {"node", "hud"},
         {"parameters", {{"size", {1280, 720}},
                         {"layers", {{{"input", "rect_1"}, {"name", "Card"}},
                                     {{"input", "rect_1_stroke"}, {"name", "Card stroke"}},
                                     {{"input", "ellipse_1"}, {"visible", false}, {"opacity", 0.5}},
                                     {{"input", "group_1"}, {"name", "Group 1"}},
                                     {{"input", "rect_2"}, {"name", "Rect 2"}}}}}}},
        {{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "rect_2"},
         {"source", {{"node", "rect_2_fill"}, {"port", "layer"}}}},
    });
    CHECK(ops == expected);
    // A stack that is not there sends nothing.
    CHECK(graphCanvasAddShapeLayerOps(nodes, "", "missing", "rect").empty());
}

TEST_CASE("SG-1: the Canvas, a move, the eye, a dropped line and Delete are one list each",
          "[studio][canvas][sg1]") {
    const auto nodes = document();

    const auto canvas = graphCanvasCompositionOps({}, "");
    REQUIRE(canvas.size() == 2);
    CHECK(canvas[0]["node"]["id"] == "canvas_stack");
    CHECK(canvas[0]["node"]["parameters"] == Json{{"size", {1280.0, 720.0}}, {"layers", Json::array()}});
    CHECK(canvas[1]["node"]["id"] == "canvas");
    CHECK(canvas[1]["node"]["inputs"]["layer"] == Json{{"node", "canvas_stack"}, {"port", "layer"}});

    // Down on the bottom layer is nothing; Up swaps it with the one above.
    CHECK(graphCanvasMoveLayerOps(nodes, "", "hud", 0, -1).empty());
    const auto up = graphCanvasMoveLayerOps(nodes, "", "hud", 0, 1);
    REQUIRE(up.size() == 1);
    CHECK(up[0]["parameters"]["layers"][0]["input"] == "rect_1_stroke");
    CHECK(up[0]["parameters"]["layers"][1]["input"] == "rect_1");

    // The eye: hidden is `visible: false`, shown is the default (left out).
    const auto shown = graphCanvasSetLayerOps(nodes, "", "hud", 2, "visible", true);
    REQUIRE(shown.size() == 1);
    CHECK_FALSE(shown[0]["parameters"]["layers"][2].contains("visible"));
    CHECK(graphCanvasSetLayerOps(nodes, "", "hud", 0, "visible", true).empty());

    // A line onto + layer: a layer on top reading it.
    const auto dropped = graphCanvasConnectLayerOps(nodes, "", "hud", "ellipse_1_fill", "layer");
    REQUIRE(dropped.size() == 2);
    CHECK(dropped[0]["parameters"]["layers"][4] == Json{{"input", "ellipse_1_fill"}, {"name", "ellipse_1_fill"}});
    CHECK(dropped[1]["port"] == "ellipse_1_fill");

    // Delete the ellipse: its element, its two lines, and the fill and the
    // shape nothing else reads -- readers before what they read.
    const auto removed = graphCanvasDeleteLayerOps(nodes, "", "hud", 2);
    REQUIRE(removed.size() == 5);
    CHECK(removed[0]["parameters"]["layers"].size() == 3);
    CHECK(removed[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "ellipse_1"},
                             {"source", nullptr}});
    CHECK(removed[2]["port"] == "ellipse_1.opacity");
    CHECK(removed[3] == Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "ellipse_1_fill"}});
    CHECK(removed[4] == Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "ellipse_1"}});
    // The card's fill goes; its shape stays, the stroke reads it too.
    const auto card = graphCanvasDeleteLayerOps(nodes, "", "hud", 0);
    REQUIRE(card.size() == 3);
    CHECK(card[2]["node"] == "rect_1_fill");

    // Taking a layer's line away takes the layer.
    const auto cut = graphCanvasDisconnectLayerOps(nodes, "", "hud", "group_1");
    REQUIRE(cut.size() == 2);
    CHECK(cut[0]["parameters"]["layers"].size() == 3);
    CHECK(graphCanvasDisconnectLayerOps(nodes, "", "hud", "ellipse_1.opacity").empty());

    // + Stroke: just above the row, reading its shape.
    const auto stroke = graphCanvasAddStrokeLayerOps(nodes, "", "hud", 2);
    REQUIRE(stroke.size() == 3);
    CHECK(stroke[0]["node"]["id"] == "ellipse_1_stroke");
    CHECK(stroke[1]["parameters"]["layers"][3]["input"] == "ellipse_1_stroke");
}
