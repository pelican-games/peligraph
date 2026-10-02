// SG-1: pelican.canvas in the Graph tab, without Qt -- a canvas.stack's
// sockets, the Layers panel's rows against the document's `layers`, and the
// op lists its gestures send (one edit each).
#include "graphcanvaslayerstack.hpp"
#include "graphcanvasmodel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
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

TEST_CASE("SG-2: + Image is a canvas.image of the picture's pixels at the stack's middle, as a layer "
          "on top, in one list",
          "[studio][canvas][sg2]") {
    const auto nodes = document();
    const auto ops = graphCanvasAddImageLayerOps(nodes, "", "hud", "project://assets/canvas/logo.png",
                                                 {200.0, 100.0}, "logo");
    REQUIRE(ops.size() == 3);
    CHECK(ops[0] == Json{{"op", "add_node"}, {"scope", "root"},
                         {"node", {{"id", "image_1"}, {"op", "canvas.image"},
                                   {"parameters", {{"asset", "project://assets/canvas/logo.png"},
                                                   {"offset", {540.0, 310.0}},
                                                   {"size", {200.0, 100.0}}}},
                                   {"outputs", {"layer"}}}}});
    CHECK(ops[1]["parameters"]["layers"][4] == Json{{"input", "image_1"}, {"name", "logo"}});
    CHECK(ops[2] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "image_1"},
                         {"source", {{"node", "image_1"}, {"port", "layer"}}}});
    // A picture with no pixels, or no stack, sends nothing.
    CHECK(graphCanvasAddImageLayerOps(nodes, "", "hud", "project://assets/canvas/x.png", {0.0, 4.0}, "x").empty());
    CHECK(graphCanvasAddImageLayerOps(nodes, "", "missing", "project://assets/canvas/x.png", {4.0, 4.0}, "x").empty());
    // The dock's stacks: the one each output reads.
    CHECK(graphCanvasOutputStacks(nodes) == std::vector<GraphCanvasOutputStack>{{"screen", "hud"}});
}

TEST_CASE("SG-2: clip, pass through and a mask are members of a layer; the mask is a luminance image "
          "on a port of its own",
          "[studio][canvas][sg2]") {
    auto nodes = document();
    // Nothing below the bottom layer to clip to.
    CHECK_FALSE(graphCanvasClipAllowed(nodes, "hud", 0));
    CHECK(graphCanvasClipAllowed(nodes, "hud", 1));
    const auto clip = graphCanvasSetLayerFlagOps(nodes, "", "hud", 1, "clip", true);
    REQUIRE(clip.size() == 1);
    CHECK(clip[0]["parameters"]["layers"][1] ==
          Json{{"input", "rect_1_stroke"}, {"name", "Card stroke"}, {"clip", true}});
    const auto through = graphCanvasSetLayerFlagOps(nodes, "", "hud", 3, "pass_through", true);
    REQUIRE(through.size() == 1);
    CHECK(through[0]["parameters"]["layers"][3]["pass_through"] == true);
    // Off is the default, left out; normal likewise.
    CHECK(graphCanvasSetLayerFlagOps(nodes, "", "hud", 1, "clip", false).empty());
    CHECK(graphCanvasSetLayerFlagOps(nodes, "", "hud", 1, "blend", "normal").empty());

    const auto mask = graphCanvasAddMaskOps(nodes, "", "hud", 0, "project://assets/canvas/m.png", {64.0, 32.0});
    REQUIRE(mask.size() == 3);
    CHECK(mask[0]["node"] == Json{{"id", "rect_1_mask"}, {"op", "canvas.image"},
                                  {"parameters", {{"asset", "project://assets/canvas/m.png"},
                                                  {"channel", "luminance"},
                                                  {"offset", {608.0, 344.0}},
                                                  {"size", {64.0, 32.0}}}},
                                  {"outputs", {"layer"}}});
    CHECK(mask[1]["parameters"]["layers"][0] ==
          Json{{"input", "rect_1"}, {"name", "Card"}, {"mask", "rect_1_mask"}});
    CHECK(mask[2] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "rect_1_mask"},
                          {"source", {{"node", "rect_1_mask"}, {"port", "layer"}}}});

    // The document after it: the mask is a port after the layers, and the
    // row says so.
    nodes.push_back(node("rect_1_mask", "canvas.image", mask[0]["node"]["parameters"]));
    auto &hud = *std::ranges::find(nodes, std::string{"hud"}, &GraphCanvasNode::id);
    hud.parameters = mask[1]["parameters"];
    hud.parameters["layers"][1]["clip"] = true;
    const auto lines = hud.inputs;
    hud.inputs = graphCanvasStackPorts(hud.parameters);
    for (auto &port : hud.inputs) {
        for (const auto &line : lines)
            if (line.port == port.port) port = line;
        if (port.port == "rect_1_mask") {
            port.connected = true;
            port.source_node = "rect_1_mask";
            port.source_port = "layer";
        }
    }
    CHECK(hud.inputs[hud.inputs.size() - 2].port == "rect_1_mask");
    const auto rows = graphCanvasLayerRows(nodes, "hud");
    CHECK(rows[3].mask == "rect_1_mask");
    CHECK(rows[3].mask_source == "rect_1_mask");
    CHECK(rows[2].clip);
    CHECK(graphCanvasRowFor(nodes, "hud", "rect_1_mask") == std::optional<std::size_t>{0});
    CHECK(graphCanvasAddMaskOps(nodes, "", "hud", 0, "project://assets/canvas/m.png", {4.0, 4.0}).empty());

    const auto removed = graphCanvasRemoveMaskOps(nodes, "", "hud", 0);
    REQUIRE(removed.size() == 3);
    CHECK_FALSE(removed[0]["parameters"]["layers"][0].contains("mask"));
    CHECK(removed[1]["port"] == "rect_1_mask");
    CHECK(removed[1]["source"].is_null());
    CHECK(removed[2] == Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "rect_1_mask"}});
    // Deleting the layer takes its mask with it.
    const auto deleted = graphCanvasDeleteLayerOps(nodes, "", "hud", 0);
    CHECK(std::ranges::any_of(deleted, [](const Json &op) {
        return op["op"] == "remove_node" && op["node"] == "rect_1_mask";
    }));

    // CV-2a's refusals as the dock's sentence.
    CHECK(graphCanvasRefusalSentence("canvas.clip_base_missing@1 {\"layer\":\"Card\",\"node\":\"root\"}")
              .starts_with("Layer \"Card\" is clipped"));
    CHECK(graphCanvasRefusalSentence("graph refused (canvas.op_unsupported@1)").starts_with("The canvas reads"));
    CHECK(graphCanvasRefusalSentence("graph.cycle@1").empty());
}

TEST_CASE("SG-2: a Transform is a canvas.transform between the layer and the stack, gone at the "
          "defaults; an imported PSD is a definition, its instance and a layer, in one list",
          "[studio][canvas][sg2]") {
    auto nodes = document();
    GraphCanvasTransformValues moved;
    moved.translate = {10.0, -4.0};
    const auto added = graphCanvasSetTransformOps(nodes, "", "hud", 2, moved);
    REQUIRE(added.size() == 2);
    CHECK(added[0]["node"] == Json{{"id", "ellipse_1_transform"}, {"op", "canvas.transform"},
                                   {"parameters", {{"translate", {10.0, -4.0}}, {"pivot", {640.0, 360.0}}}},
                                   {"inputs", {{"layer", {{"node", "ellipse_1_fill"}, {"port", "layer"}}}}},
                                   {"outputs", {"layer"}}});
    CHECK(added[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "ellipse_1"},
                           {"source", {{"node", "ellipse_1_transform"}, {"port", "layer"}}}});
    CHECK(graphCanvasSetTransformOps(nodes, "", "hud", 2, GraphCanvasTransformValues{}).empty());

    // With it in place the row still draws the fill, and the defaults take
    // it out again.
    auto transform = node("ellipse_1_transform", "canvas.transform", added[0]["node"]["parameters"]);
    connect(transform, "layer", "ellipse_1_fill", "layer");
    nodes.push_back(transform);
    for (auto &port : std::ranges::find(nodes, std::string{"hud"}, &GraphCanvasNode::id)->inputs)
        if (port.port == "ellipse_1") port.source_node = "ellipse_1_transform";
    const auto rows = graphCanvasLayerRows(nodes, "hud");
    CHECK(rows[1].source == "ellipse_1_fill");
    CHECK(rows[1].transform == "ellipse_1_transform");
    CHECK(rows[1].kind == "ellipse");
    CHECK(graphCanvasLayerTransform(nodes, rows[1]) == moved);
    const auto back = graphCanvasSetTransformOps(nodes, "", "hud", 2, GraphCanvasTransformValues{});
    REQUIRE(back.size() == 2);
    CHECK(back[0]["source"] == Json{{"node", "ellipse_1_fill"}, {"port", "layer"}});
    CHECK(back[1] == Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "ellipse_1_transform"}});

    // A conversion made with another prefix, or none, reads from the
    // directory the copy takes.
    const Json definition{
        {"nodes",
         {{{"id", "a"}, {"op", "canvas.image"},
           {"parameters", {{"asset", "project://imports/other/layers/0000_A.png"}, {"size", {4, 4}}}}},
          {{"id", "b"}, {"op", "canvas.image"},
           {"parameters", {{"asset", "masks/0000_A.png"}, {"channel", "luminance"}, {"size", {4, 4}}}}},
          {{"id", "root"}, {"op", "canvas.stack"},
           {"parameters", {{"size", {4, 4}}, {"layers", {{{"input", "a"}, {"mask", "b"}}}}}},
           {"inputs", {{"a", {{"node", "a"}, {"port", "layer"}}}, {"b", {{"node", "b"}, {"port", "layer"}}}}}}}},
        {"outputs", {{"layer", {{"node", "root"}, {"port", "layer"}}}}}};
    const auto rebased = graphCanvasRebaseImportAssets(definition, "psd_card");
    CHECK(rebased["nodes"][0]["parameters"]["asset"] == "project://imports/psd_card/layers/0000_A.png");
    CHECK(rebased["nodes"][1]["parameters"]["asset"] == "project://imports/psd_card/masks/0000_A.png");
    CHECK(graphCanvasDefinitionStack(rebased) == "root");

    const auto imported = graphCanvasImportDefinitionOps(nodes, "", "hud", "psd_card", rebased, "card");
    REQUIRE(imported.size() == 4);
    CHECK(imported[0] == Json{{"op", "add_definition"}, {"name", "psd_card"}, {"definition", rebased}});
    CHECK(imported[1]["node"] == Json{{"id", "psd_card"}, {"op", "graph.instance"}, {"definition", "psd_card"},
                                      {"parameters", Json::object()}});
    CHECK(imported[2]["parameters"]["layers"][4] == Json{{"input", "psd_card"}, {"name", "PSD: card"}});
    CHECK(imported[3]["source"] == Json{{"node", "psd_card"}, {"port", "layer"}});
}
