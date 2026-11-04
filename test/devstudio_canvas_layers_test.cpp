// SG-1: pelican.canvas in the Graph tab, without Qt -- a canvas.stack's
// sockets, the Layers panel's rows against the document's `layers`, and the
// op lists its gestures send (one edit each).
#include "graphcanvaslayertool.hpp"
#include "graphcanvaslayerstack.hpp"
#include "graphcanvasmodel.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
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

namespace {

// `nodes` after `ops`, as the canvas reads them: enough of an edit for the
// next gesture to be built on it (a stack's ports follow its layers).
void applyOps(std::vector<GraphCanvasNode> &nodes, const Json &ops) {
    const auto wire = [](GraphCanvasNode &to, const std::string &port, const Json &source) {
        auto found = std::ranges::find(to.inputs, port, &GraphCanvasPort::port);
        if (found == to.inputs.end()) {
            GraphCanvasPort made;
            made.port = port;
            to.inputs.push_back(made);
            found = std::prev(to.inputs.end());
        }
        found->connected = source.is_object();
        found->source_node = source.is_object() ? source.value("node", std::string{}) : std::string{};
        found->source_port = source.is_object() ? source.value("port", std::string{}) : std::string{};
    };
    const auto restack = [](GraphCanvasNode &made) {
        if (made.op != "canvas.stack") return;
        auto ports = graphCanvasStackPorts(made.parameters);
        for (auto &port : ports)
            if (const auto old = std::ranges::find(made.inputs, port.port, &GraphCanvasPort::port);
                old != made.inputs.end() && old->connected)
                port = *old;
        made.inputs = std::move(ports);
    };
    for (const auto &op : ops) {
        const auto kind = op.value("op", std::string{});
        if (kind == "add_node") {
            const auto &json = op.at("node");
            auto made = node(json.value("id", std::string{}), json.value("op", std::string{}),
                             json.value("parameters", Json::object()));
            restack(made);
            const auto inputs = json.value("inputs", Json::object());
            for (const auto &[port, source] : inputs.items()) wire(made, port, source);
            nodes.push_back(std::move(made));
        } else if (kind == "set_node_parameters") {
            for (auto &each : nodes)
                if (each.id == op.value("node", std::string{})) {
                    each.parameters = op.at("parameters");
                    restack(each);
                }
        } else if (kind == "set_input") {
            for (auto &each : nodes)
                if (each.id == op.value("node", std::string{})) wire(each, op.value("port", std::string{}), op.at("source"));
        } else if (kind == "remove_node") {
            std::erase_if(nodes, [&](const GraphCanvasNode &each) { return each.id == op.value("node", std::string{}); });
        }
    }
}

} // namespace

TEST_CASE("SG-3: the blend combo is the engine's 26 blends in Photoshop's groups; a blend is written, "
          "normal is left out",
          "[studio][canvas][sg3]") {
    const auto &choices = graphCanvasBlendChoices();
    REQUIRE(choices.size() == 26);
    // Every engine name once, none the table had to guess.
    std::vector<std::string> names;
    for (const auto &choice : choices) {
        names.push_back(choice.name);
        CHECK(choice.group < 6);
    }
    auto sorted = names;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
    CHECK(std::ranges::is_sorted(choices, std::less<>{}, &GraphCanvasBlendChoice::group));
    CHECK(choices.front() == GraphCanvasBlendChoice{"normal", "Normal", 0});
    // The darken group in the engine's order: multiply before darken.
    CHECK(choices[1] == GraphCanvasBlendChoice{"multiply", "Multiply", 1});
    CHECK(choices[2].name == "darken");
    CHECK(choices.back() == GraphCanvasBlendChoice{"luminosity", "Luminosity", 5});

    auto nodes = document();
    const auto multiply = graphCanvasSetLayerFlagOps(nodes, "", "hud", 0, "blend", "multiply");
    REQUIRE(multiply.size() == 1);
    CHECK(multiply[0]["parameters"]["layers"][0] == Json{{"input", "rect_1"}, {"name", "Card"}, {"blend", "multiply"}});
    applyOps(nodes, multiply);
    CHECK(graphCanvasLayerRows(nodes, "hud")[3].blend == "multiply");
    const auto normal = graphCanvasSetLayerFlagOps(nodes, "", "hud", 0, "blend", "normal");
    REQUIRE(normal.size() == 1);
    CHECK(normal[0]["parameters"]["layers"][0] == Json{{"input", "rect_1"}, {"name", "Card"}});

    // The refusals SG-3 adds, as one sentence.
    CHECK(graphCanvasRefusalSentence(R"(canvas.place_backdrop_unsupported@1 {"layer":"card","node":"board"})")
              .starts_with("Layer \"card\" is drawn straight into the 3D scene"));
    for (const auto *code : {"canvas.method_unavailable@1", "canvas.range_unbounded@1", "graph.image_source_conflict@1",
                             "canvas.filter_parameter_unsupported@1"})
        CHECK_FALSE(graphCanvasRefusalSentence(code).empty());
}

TEST_CASE("SG-3: + Text is the text, its white fill and a layer on top, in one list; its fields leave the "
          "engine's defaults out",
          "[studio][canvas][sg3]") {
    auto nodes = document();
    const auto ops = graphCanvasAddTextLayerOps(nodes, "", "hud", "Hi", "project://assets/fonts/arial.ttf", 48.0);
    const Json expected = Json::array({
        {{"op", "add_node"}, {"scope", "root"},
         {"node", {{"id", "text_1"}, {"op", "shape.text"},
                   {"parameters", {{"font", "project://assets/fonts/arial.ttf"}, {"text", "Hi"}, {"size", 48.0},
                                   {"origin", {640.0, 360.0}}, {"align", "center"}}},
                   {"outputs", {"geometry"}}}}},
        {{"op", "add_node"}, {"scope", "root"},
         {"node", {{"id", "text_1_fill"}, {"op", "shape.fill"},
                   {"parameters", {{"color", {1.0, 1.0, 1.0, 1.0}}}},
                   {"inputs", {{"geometry", {{"node", "text_1"}, {"port", "geometry"}}}}},
                   {"outputs", {"layer"}}}}},
    });
    REQUIRE(ops.size() == 4);
    CHECK(ops[0] == expected[0]);
    CHECK(ops[1] == expected[1]);
    CHECK(ops[2]["parameters"]["layers"][4] == Json{{"input", "text_1"}, {"name", "Hi"}});
    CHECK(ops[3] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "text_1"},
                         {"source", {{"node", "text_1_fill"}, {"port", "layer"}}}});
    // No font, no size: nothing.
    CHECK(graphCanvasAddTextLayerOps(nodes, "", "hud", "Hi", "", 48.0).empty());
    CHECK(graphCanvasAddTextLayerOps(nodes, "", "hud", "Hi", "project://f.ttf", 0.0).empty());

    applyOps(nodes, ops);
    const auto rows = graphCanvasLayerRows(nodes, "hud");
    REQUIRE(rows[0].input == "text_1");
    CHECK(rows[0].kind == "text");
    CHECK(rows[0].paint == "fill");
    CHECK(rows[0].shape == "text_1");
    CHECK(graphCanvasLayerKindText(rows[0]) == "Text");
    CHECK(graphCanvasStackFor(nodes, "text_1") == "hud");
    CHECK_FALSE(graphCanvasStrokeRowOf(nodes, "hud", 4));

    // Align left and a line height of 1.2 are the engine's: left out.
    const auto left = graphCanvasSetTextParameterOps(nodes, "", "text_1", "align", "left");
    REQUIRE(left.size() == 1);
    CHECK_FALSE(left[0]["parameters"].contains("align"));
    CHECK(graphCanvasSetTextParameterOps(nodes, "", "text_1", "line_height", 1.2).empty());
    CHECK(graphCanvasSetTextParameterOps(nodes, "", "text_1", "box_width", 300.0)[0]["parameters"]["box_width"] == 300.0);
    CHECK(graphCanvasSetTextParameterOps(nodes, "", "text_1", "align", "middle").empty());
    CHECK(graphCanvasSetTextParameterOps(nodes, "", "rect_1", "text", "x").empty());

    // The Stroke box: + Stroke of the text, found again as its stroke row.
    applyOps(nodes, graphCanvasAddStrokeLayerOps(nodes, "", "hud", 4));
    CHECK(graphCanvasStrokeRowOf(nodes, "hud", 4) == std::optional<std::size_t>{5});
}

TEST_CASE("SG-3: + Adjustment is a backdrop under a filter as a layer above the picked one; + Filter goes "
          "between a layer and the stack, over its transform and its filters, and comes off again",
          "[studio][canvas][sg3]") {
    auto nodes = document();
    // CV-3a: + Filter offers the colour filters and then Gaussian Blur, Drop
    // Shadow and Outer Glow; + Adjustment the colour filters alone.
    const auto &filters = graphCanvasFilterChoices();
    REQUIRE(filters.size() == 9);
    CHECK(filters.front() == GraphCanvasFilterChoice{"filter.invert", "Invert"});
    CHECK(graphCanvasAdjustmentChoices().size() == 6);
    CHECK(graphCanvasFilterDefaults("filter.radial_blur").is_null());

    // Above the card (index 0): at index 1.
    const auto adjustment = graphCanvasAddAdjustmentLayerOps(nodes, "", "hud", 0, "filter.invert");
    REQUIRE(adjustment.size() == 4);
    CHECK(adjustment[0]["node"] == Json{{"id", "invert_1_backdrop"}, {"op", "canvas.backdrop"},
                                        {"parameters", Json::object()}, {"outputs", {"layer"}}});
    CHECK(adjustment[1]["node"] == Json{{"id", "invert_1"}, {"op", "filter.invert"}, {"parameters", Json::object()},
                                        {"inputs", {{"layer", {{"node", "invert_1_backdrop"}, {"port", "layer"}}}}},
                                        {"outputs", {"layer"}}});
    CHECK(adjustment[2]["parameters"]["layers"][1] == Json{{"input", "invert_1"}, {"name", "Invert 1"}});
    CHECK(adjustment[3]["source"] == Json{{"node", "invert_1"}, {"port", "layer"}});
    // Nothing picked: on top.
    const auto levels = graphCanvasAddAdjustmentLayerOps(nodes, "", "hud", std::nullopt, "filter.levels");
    CHECK(levels[1]["node"]["parameters"] == Json{{"input_floor", 0.0}, {"input_ceiling", 255.0}, {"gamma", 1.0},
                                                  {"output_floor", 0.0}, {"output_ceiling", 255.0}});
    CHECK(levels[2]["parameters"]["layers"][4]["name"] == "Levels 1");
    applyOps(nodes, adjustment);
    auto rows = graphCanvasLayerRows(nodes, "hud");
    const auto adjusted = std::ranges::find(rows, std::string{"invert_1"}, &GraphCanvasStackRow::input);
    REQUIRE(adjusted != rows.end());
    CHECK(adjusted->kind == "adjustment");
    CHECK(adjusted->source == "invert_1_backdrop");
    CHECK(adjusted->filters == std::vector<std::string>{"invert_1"});
    CHECK(graphCanvasStackFor(nodes, "invert_1_backdrop") == "hud");
    // Its one filter is the adjustment: Delete takes it, not Remove filter.
    CHECK(graphCanvasRemoveFilterOps(nodes, "", "hud", 1, "invert_1").empty());
    const auto deleted = graphCanvasDeleteLayerOps(nodes, "", "hud", 1);
    CHECK(std::ranges::count_if(deleted, [](const Json &op) { return op["op"] == "remove_node"; }) == 2);

    // The ellipse (index 3 now) moved, then filtered: transform -> filter -> stack.
    GraphCanvasTransformValues moved;
    moved.translate = {8.0, 0.0};
    applyOps(nodes, graphCanvasSetTransformOps(nodes, "", "hud", 3, moved));
    const auto first = graphCanvasAddFilterOps(nodes, "", "hud", 3, "filter.hue_saturation");
    REQUIRE(first.size() == 2);
    CHECK(first[0]["node"]["id"] == "ellipse_1_hue_saturation");
    CHECK(first[0]["node"]["inputs"]["layer"] == Json{{"node", "ellipse_1_transform"}, {"port", "layer"}});
    CHECK(first[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "ellipse_1"},
                           {"source", {{"node", "ellipse_1_hue_saturation"}, {"port", "layer"}}}});
    applyOps(nodes, first);
    // A second one goes over the first.
    const auto second = graphCanvasAddFilterOps(nodes, "", "hud", 3, "filter.color_overlay");
    CHECK(second[0]["node"]["inputs"]["layer"]["node"] == "ellipse_1_hue_saturation");
    CHECK(second[0]["node"]["parameters"] == Json{{"color", {0.9, 0.2, 0.2, 1.0}}});
    applyOps(nodes, second);
    rows = graphCanvasLayerRows(nodes, "hud");
    const auto ellipse = std::ranges::find(rows, std::string{"ellipse_1"}, &GraphCanvasStackRow::input);
    REQUIRE(ellipse != rows.end());
    CHECK(ellipse->kind == "ellipse");
    CHECK(ellipse->source == "ellipse_1_fill");
    CHECK(ellipse->transform == "ellipse_1_transform");
    CHECK(ellipse->filters == std::vector<std::string>{"ellipse_1_hue_saturation", "ellipse_1_color_overlay"});
    CHECK(graphCanvasRowFor(nodes, "hud", "ellipse_1_color_overlay") == std::optional<std::size_t>{3});
    CHECK(graphCanvasStackFor(nodes, "ellipse_1_hue_saturation") == "hud");

    // Taking the lower one off: the upper reads the transform.
    const auto off = graphCanvasRemoveFilterOps(nodes, "", "hud", 3, "ellipse_1_hue_saturation");
    REQUIRE(off.size() == 2);
    CHECK(off[0] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "ellipse_1_color_overlay"}, {"port", "layer"},
                         {"source", {{"node", "ellipse_1_transform"}, {"port", "layer"}}}});
    CHECK(off[1] == Json{{"op", "remove_node"}, {"scope", "root"}, {"node", "ellipse_1_hue_saturation"}});
    // Back at the defaults the transform goes; the lowest filter reads the fill.
    const auto unmoved = graphCanvasSetTransformOps(nodes, "", "hud", 3, GraphCanvasTransformValues{});
    REQUIRE(unmoved.size() == 2);
    CHECK(unmoved[0] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "ellipse_1_hue_saturation"},
                             {"port", "layer"}, {"source", {{"node", "ellipse_1_fill"}, {"port", "layer"}}}});
    applyOps(nodes, unmoved);
    // A transform added to a filtered layer goes under its filters.
    const auto again = graphCanvasSetTransformOps(nodes, "", "hud", 3, moved);
    REQUIRE(again.size() == 2);
    CHECK(again[0]["node"]["inputs"]["layer"] == Json{{"node", "ellipse_1_fill"}, {"port", "layer"}});
    CHECK(again[1]["node"] == "ellipse_1_hue_saturation");
    CHECK(again[1]["port"] == "layer");
}

TEST_CASE("SG-3: Place in 3D scene is a canvas.place of the stack on a material pass's colour and depth, "
          "two units wide; only a document with a material pass has somewhere to place it",
          "[studio][canvas][sg3]") {
    auto nodes = document();
    CHECK(graphCanvasScenePasses(nodes).empty());
    auto pass = node("fwd", "render.material_pass", Json{{"material_contract", "forward_opaque_v1"}});
    connect(pass, "color", "scene_color", "resource");
    connect(pass, "depth", "scene_depth", "resource");
    pass.outputs = {GraphCanvasOutput{"color", "pelican.render.color_signal@1"},
                    GraphCanvasOutput{"depth", "pelican.render.depth@1"}};
    nodes.push_back(pass);
    const auto passes = graphCanvasScenePasses(nodes);
    REQUIRE(passes == std::vector<GraphCanvasScenePass>{{"fwd", "color", "depth"}});

    const auto ops = graphCanvasPlaceOps(nodes, "", "hud", passes.front());
    REQUIRE(ops.size() == 1);
    CHECK(ops[0] == Json{{"op", "add_node"}, {"scope", "root"},
                         {"node", {{"id", "hud_place"}, {"op", "canvas.place"},
                                   {"parameters", {{"position", {0.0, 0.0, 0.0}}, {"rotation", {0.0, 0.0, 0.0}},
                                                   {"scale", 1.0}, {"pixels_per_unit", 640.0}}},
                                   {"inputs", {{"layer", {{"node", "hud"}, {"port", "layer"}}},
                                               {"color", {{"node", "fwd"}, {"port", "color"}}},
                                               {"depth", {{"node", "fwd"}, {"port", "depth"}}}}},
                                   {"outputs", {"color"}}}}});
    // Without a depth image it is drawn without one.
    auto flat = nodes;
    std::erase_if(flat.back().inputs, [](const GraphCanvasPort &port) { return port.port == "depth"; });
    CHECK(graphCanvasScenePasses(flat).front().depth.empty());
    CHECK_FALSE(graphCanvasPlaceOps(flat, "", "hud", graphCanvasScenePasses(flat).front())[0]["node"]["inputs"].contains("depth"));

    applyOps(nodes, ops);
    CHECK(graphCanvasPlacesOf(nodes, "hud") == std::vector<std::string>{"hud_place"});
    CHECK(graphCanvasStackFor(nodes, "hud_place") == "hud");
    // The canvas.output stays.
    CHECK(graphCanvasOutputStacks(nodes) == std::vector<GraphCanvasOutputStack>{{"screen", "hud"}});
    // Its fields: the engine's defaults are left out, position always written.
    const auto turned = graphCanvasSetPlaceParameterOps(nodes, "", "hud_place", "rotation", Json::array({0, 30, 0}));
    REQUIRE(turned.size() == 1);
    CHECK(turned[0]["parameters"]["rotation"] == Json::array({0, 30, 0}));
    const auto unturned = graphCanvasSetPlaceParameterOps(nodes, "", "hud_place", "rotation", Json::array({0, 0, 0}));
    REQUIRE(unturned.size() == 1);
    CHECK_FALSE(unturned[0]["parameters"].contains("rotation"));
    CHECK(graphCanvasSetPlaceParameterOps(nodes, "", "hud_place", "depth_write", true)[0]["parameters"]["depth_write"] ==
          true);
    CHECK(graphCanvasSetPlaceParameterOps(nodes, "", "hud_place", "pixels_per_unit", 0.0).empty());
    CHECK(graphCanvasSetPlaceParameterOps(nodes, "", "hud_place", "position", Json::array({1, 2, 3}))[0]["parameters"]
              ["position"] == Json::array({1, 2, 3}));
}

TEST_CASE("CV-3a: + Filter offers Gaussian Blur, Drop Shadow and Outer Glow with Photoshop's defaults; "
          "+ Adjustment the colour filters alone",
          "[studio][canvas][cv3a]") {
    auto nodes = document();
    const auto &filters = graphCanvasFilterChoices();
    REQUIRE(filters.size() == 9);
    CHECK(filters[6] == GraphCanvasFilterChoice{"filter.blur", "Gaussian Blur"});
    CHECK(filters[7] == GraphCanvasFilterChoice{"filter.drop_shadow", "Drop Shadow"});
    CHECK(filters[8] == GraphCanvasFilterChoice{"filter.outer_glow", "Outer Glow"});
    const auto &adjustments = graphCanvasAdjustmentChoices();
    CHECK(std::ranges::none_of(adjustments, [](const GraphCanvasFilterChoice &choice) {
        return choice.op == "filter.blur" || choice.op == "filter.drop_shadow" || choice.op == "filter.outer_glow";
    }));
    // The fields start where Photoshop's do: a 1 px blur; a shadow 5 px away
    // at 120 degrees, size 5, black at 75 %; a glow size 5, (255, 255, 190) at 75 %.
    CHECK(graphCanvasFilterDefaults("filter.blur") == Json{{"radius", 1.0}});
    const auto shadow = graphCanvasFilterDefaults("filter.drop_shadow");
    CHECK(shadow == Json{{"offset", {2.5, 4.330127018922193}}, {"radius", 5.0}, {"spread", 0.0},
                         {"color", {0.0, 0.0, 0.0, 1.0}}, {"opacity", 0.75}});
    const auto glow = graphCanvasFilterDefaults("filter.outer_glow");
    CHECK(glow == Json{{"radius", 5.0}, {"spread", 0.0}, {"color", {1.0, 1.0, 190.0 / 255.0, 1.0}}, {"opacity", 0.75}});

    // + Filter puts one between the layer and the stack, as a colour filter.
    const auto ops = graphCanvasAddFilterOps(nodes, "", "hud", 2, "filter.drop_shadow");
    REQUIRE(ops.size() == 2);
    CHECK(ops[0]["node"]["id"] == "ellipse_1_drop_shadow");
    CHECK(ops[0]["node"]["op"] == "filter.drop_shadow");
    CHECK(ops[0]["node"]["parameters"] == shadow);
    CHECK(ops[0]["node"]["inputs"]["layer"] == Json{{"node", "ellipse_1_fill"}, {"port", "layer"}});
    CHECK(ops[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "ellipse_1"},
                         {"source", {{"node", "ellipse_1_drop_shadow"}, {"port", "layer"}}}});
    applyOps(nodes, ops);
    const auto rows = graphCanvasLayerRows(nodes, "hud");
    const auto ellipse = std::ranges::find(rows, std::string{"ellipse_1"}, &GraphCanvasStackRow::input);
    REQUIRE(ellipse != rows.end());
    CHECK(ellipse->filters == std::vector<std::string>{"ellipse_1_drop_shadow"});
    CHECK(graphCanvasFilterLabel("filter.drop_shadow") == "Drop Shadow");
    // A blur of the layers below (an adjustment) is no adjustment.
    CHECK(graphCanvasAddAdjustmentLayerOps(nodes, "", "hud", 0, "filter.blur").empty());
    CHECK_FALSE(graphCanvasAddAdjustmentLayerOps(nodes, "", "hud", 0, "filter.invert").empty());
    // The engine's refusals, in words.
    CHECK(graphCanvasRefusalSentence("graph refused (canvas.filter_backdrop_unsupported@1)").starts_with("A blur"));
    CHECK(graphCanvasRefusalSentence("graph refused (canvas.filter_size_exceeded@1)").starts_with("A blur"));
    CHECK(graphCanvasRefusalSentence("graph refused (canvas.range_unbounded@1)").starts_with("An effect's reach"));
}

// --- NB-2a: the model under the layer stack node (docs/design_node_bodies.md 2.3).

namespace {

// `document()` with a blend on the card and a mask on its stroke, so a move
// has every member of an element to carry.
std::vector<GraphCanvasNode> blendedDocument() {
    auto nodes = document();
    auto &hud = *std::ranges::find(nodes, std::string{"hud"}, &GraphCanvasNode::id);
    hud.parameters["layers"][0]["blend"] = "screen";
    hud.parameters["layers"][1]["mask"] = "card_mask";
    const auto lines = hud.inputs;
    hud.inputs = graphCanvasStackPorts(hud.parameters);
    for (auto &port : hud.inputs)
        for (const auto &line : lines)
            if (line.port == port.port) port = line;
    return nodes;
}

Json layersAfter(const Json &ops) {
    REQUIRE(ops.size() == 1);
    CHECK(ops[0]["op"] == "set_node_parameters");
    CHECK(ops[0]["node"] == "hud");
    return ops[0]["parameters"]["layers"];
}

} // namespace

TEST_CASE("NB-2a: a dragged row moves its whole layer to any place in one edit; Up and Down are its one-step "
          "case",
          "[studio][canvas][nb2]") {
    auto nodes = blendedDocument();
    const auto &hud = *std::ranges::find(nodes, std::string{"hud"}, &GraphCanvasNode::id);
    const auto before = hud.parameters["layers"];
    REQUIRE(before.size() == 4);

    // The bottom one to the top: the others keep their order under it.
    const auto top = graphCanvasMoveLayerToOps(nodes, "", "hud", 0, 3);
    CHECK(layersAfter(top) == Json::array({before[1], before[2], before[3], before[0]}));
    // The stack's other parameters as they were.
    CHECK(top[0]["parameters"]["size"] == hud.parameters["size"]);
    // The second from the top (index 2 of 4) to the bottom.
    const auto bottom = graphCanvasMoveLayerToOps(nodes, "", "hud", 2, 0);
    CHECK(layersAfter(bottom) == Json::array({before[2], before[0], before[1], before[3]}));
    // Each element went with its members: blend, mask, eye, opacity, name.
    CHECK(bottom[0]["parameters"]["layers"][0] ==
          Json{{"input", "ellipse_1"}, {"visible", false}, {"opacity", 0.5}});
    CHECK(top[0]["parameters"]["layers"][3] == Json{{"input", "rect_1"}, {"name", "Card"}, {"blend", "screen"}});
    CHECK(top[0]["parameters"]["layers"][0]["mask"] == "card_mask");

    // The lines stay on their ports: after the move the rows read the same.
    auto moved = nodes;
    applyOps(moved, bottom);
    const auto rows = graphCanvasLayerRows(moved, "hud");
    REQUIRE(rows.size() == 4);
    CHECK(rows[3].input == "ellipse_1");
    CHECK(rows[3].index == 0);
    CHECK(rows[3].source == "ellipse_1_fill");
    CHECK(rows[3].opacity_driven);
    CHECK(rows[2].blend == "screen");
    CHECK(rows[1].mask == "card_mask");

    // Nowhere to go: the same place, or past the end.
    CHECK(graphCanvasMoveLayerToOps(nodes, "", "hud", 1, 1).empty());
    CHECK(graphCanvasMoveLayerToOps(nodes, "", "hud", 4, 0).empty());
    CHECK(graphCanvasMoveLayerToOps(nodes, "", "hud", 0, 4).empty());
    CHECK(graphCanvasMoveLayerToOps(nodes, "", "missing", 0, 1).empty());

    // Up and Down are the same op as a move to the next place.
    for (std::size_t index = 0; index < 4; ++index) {
        CHECK(graphCanvasMoveLayerOps(nodes, "", "hud", index, 1) ==
              graphCanvasMoveLayerToOps(nodes, "", "hud", index, index + 1));
        if (index > 0)
            CHECK(graphCanvasMoveLayerOps(nodes, "", "hud", index, -1) ==
                  graphCanvasMoveLayerToOps(nodes, "", "hud", index, index - 1));
    }
    CHECK(graphCanvasMoveLayerOps(nodes, "", "hud", 0, -1).empty());
    CHECK(graphCanvasMoveLayerOps(nodes, "", "hud", 3, 1).empty());
    CHECK(graphCanvasMoveLayerOps(nodes, "", "hud", 1, 2).empty());
}

TEST_CASE("NB-2a: a line dropped between two rows is a layer at that place and its line; + layer is the drop "
          "on top",
          "[studio][canvas][nb2]") {
    const auto nodes = blendedDocument();
    const auto before = std::ranges::find(nodes, std::string{"hud"}, &GraphCanvasNode::id)->parameters["layers"];

    // Between the card (0) and its stroke (1) of four.
    const auto between = graphCanvasInsertLayerOps(nodes, "", "hud", 1, "ellipse_1_fill", "layer");
    REQUIRE(between.size() == 2);
    CHECK(between[0]["op"] == "set_node_parameters");
    CHECK(between[0]["parameters"]["layers"] ==
          Json::array({before[0], Json{{"input", "ellipse_1_fill"}, {"name", "ellipse_1_fill"}}, before[1],
                       before[2], before[3]}));
    CHECK(between[1] == Json{{"op", "set_input"}, {"scope", "root"}, {"node", "hud"}, {"port", "ellipse_1_fill"},
                             {"source", {{"node", "ellipse_1_fill"}, {"port", "layer"}}}});
    auto inserted = nodes;
    applyOps(inserted, between);
    const auto rows = graphCanvasLayerRows(inserted, "hud");
    REQUIRE(rows.size() == 5);
    CHECK(rows[3].index == 1);
    CHECK(rows[3].source == "ellipse_1_fill");
    CHECK(rows[3].kind == "ellipse");

    // At the bottom.
    const auto under = graphCanvasInsertLayerOps(nodes, "", "hud", 0, "rect_1_fill", "layer");
    REQUIRE(under.size() == 2);
    CHECK(under[0]["parameters"]["layers"][0] == Json{{"input", "rect_1_fill"}, {"name", "rect_1_fill"}});
    CHECK(under[0]["parameters"]["layers"][1] == before[0]);

    // On top (the size, or past it) is + layer's op.
    const auto connected = graphCanvasConnectLayerOps(nodes, "", "hud", "ellipse_1_fill", "layer");
    REQUIRE(connected.size() == 2);
    CHECK(graphCanvasInsertLayerOps(nodes, "", "hud", 4, "ellipse_1_fill", "layer") == connected);
    CHECK(graphCanvasInsertLayerOps(nodes, "", "hud", 99, "ellipse_1_fill", "layer") == connected);
    CHECK(connected[0]["parameters"]["layers"][4] == Json{{"input", "ellipse_1_fill"}, {"name", "ellipse_1_fill"}});

    // A taken input name is numbered, as + layer's is: rect_1 is a port already.
    const auto again = graphCanvasInsertLayerOps(nodes, "", "hud", 2, "rect_1", "geometry");
    REQUIRE(again.size() == 2);
    CHECK(again[0]["parameters"]["layers"][2]["input"] == "rect_1_2");
    CHECK(again[1]["source"] == Json{{"node", "rect_1"}, {"port", "geometry"}});

    // Nothing to read, or the stack itself.
    CHECK(graphCanvasInsertLayerOps(nodes, "", "hud", 1, "missing", "layer").empty());
    CHECK(graphCanvasInsertLayerOps(nodes, "", "hud", 1, "hud", "layer").empty());
    CHECK(graphCanvasInsertLayerOps(nodes, "", "missing", 1, "rect_1_fill", "layer").empty());
}

TEST_CASE("NB-2a: + Rect, + Ellipse, + Group, + Image and + Text go just above the picked row, on top "
          "without one",
          "[studio][canvas][nb2]") {
    const auto nodes = blendedDocument();
    const auto layerAt = [](const Json &ops, std::size_t at) {
        for (const auto &op : ops)
            if (op["op"] == "set_node_parameters") return op["parameters"]["layers"][at];
        return Json{};
    };
    const auto count = [](const Json &ops) {
        for (const auto &op : ops)
            if (op["op"] == "set_node_parameters") return op["parameters"]["layers"].size();
        return std::size_t{0};
    };

    // Above the card (0): at 1.
    const auto rect = graphCanvasAddShapeLayerOps(nodes, "", "hud", "rect", 0);
    CHECK(layerAt(rect, 1) == Json{{"input", "rect_2"}, {"name", "Rect 2"}});
    CHECK(layerAt(rect, 2)["input"] == "rect_1_stroke");
    CHECK(count(rect) == 5);
    // The rest of the list is as on top: only the place differs.
    auto on_top = graphCanvasAddShapeLayerOps(nodes, "", "hud", "rect");
    CHECK(layerAt(on_top, 4) == Json{{"input", "rect_2"}, {"name", "Rect 2"}});
    CHECK(rect[0] == on_top[0]);
    CHECK(rect[1] == on_top[1]);
    CHECK(rect[3] == on_top[3]);

    const auto ellipse = graphCanvasAddShapeLayerOps(nodes, "", "hud", "ellipse", 2);
    CHECK(layerAt(ellipse, 3) == Json{{"input", "ellipse_2"}, {"name", "Ellipse 2"}});

    const auto group = graphCanvasAddGroupLayerOps(nodes, "", "hud", 1);
    CHECK(layerAt(group, 2) == Json{{"input", "group_2"}, {"name", "Group 2"}});
    CHECK(layerAt(graphCanvasAddGroupLayerOps(nodes, "", "hud"), 4)["input"] == "group_2");

    const auto image =
        graphCanvasAddImageLayerOps(nodes, "", "hud", "project://assets/canvas/logo.png", {200.0, 100.0}, "logo", 2);
    CHECK(layerAt(image, 3) == Json{{"input", "image_1"}, {"name", "logo"}});

    const auto text = graphCanvasAddTextLayerOps(nodes, "", "hud", "Hi", "project://assets/fonts/arial.ttf", 48.0, 0);
    CHECK(layerAt(text, 1) == Json{{"input", "text_1"}, {"name", "Hi"}});

    // Above the top row, or a row past the top: on top.
    CHECK(layerAt(graphCanvasAddShapeLayerOps(nodes, "", "hud", "rect", 3), 4)["input"] == "rect_2");
    CHECK(layerAt(graphCanvasAddShapeLayerOps(nodes, "", "hud", "rect", 99), 4)["input"] == "rect_2");
    // + Adjustment's `above` is the same place.
    CHECK(layerAt(graphCanvasAddAdjustmentLayerOps(nodes, "", "hud", 2, "filter.invert"), 3)["input"] == "invert_1");
}

TEST_CASE("NB-2a: a row says its opacity port, where its > goes, and whether that stack is read only",
          "[studio][canvas][nb2]") {
    auto nodes = blendedDocument();
    CHECK(graphCanvasLayerOpacityPort("ellipse_1") == "ellipse_1.opacity");

    // An imported PSD as the top layer of hud.
    auto psd = node("logo_psd_1", "graph.instance");
    psd.definition = "logo_psd";
    nodes.push_back(std::move(psd));
    auto &hud = *std::ranges::find(nodes, std::string{"hud"}, &GraphCanvasNode::id);
    hud.parameters["layers"].push_back(Json{{"input", "logo_psd_1"}, {"name", "PSD: logo"}});
    const auto lines = hud.inputs;
    hud.inputs = graphCanvasStackPorts(hud.parameters);
    for (auto &port : hud.inputs) {
        for (const auto &line : lines)
            if (line.port == port.port) port = line;
        if (port.port == "logo_psd_1") {
            port.connected = true;
            port.source_node = "logo_psd_1";
            port.source_port = "layer";
        }
    }
    const auto rows = graphCanvasLayerRows(nodes, "hud");
    REQUIRE(rows.size() == 5);
    REQUIRE(rows[0].kind == "import");
    REQUIRE(rows[1].kind == "group");

    // A group: its own stack, in the same scope, as read only as hud.
    CHECK(graphCanvasRowEntry(rows[1], "", false) == GraphCanvasRowEntry{"", "group_1", false});
    CHECK(graphCanvasRowEntry(rows[1], "logo_psd", true) == GraphCanvasRowEntry{"logo_psd", "group_1", true});
    // An import: its definition, read only, at the stack no node of it reads.
    std::vector<GraphCanvasNode> inside;
    inside.push_back(node("layer_group", "canvas.stack", Json{{"size", {64, 64}}, {"layers", Json::array()}}));
    inside.push_back(node("psd_root", "canvas.stack", Json{{"size", {64, 64}}, {"layers", Json::array()}}));
    connect(inside.back(), "layer_group", "layer_group", "layer", std::string{graphCanvasLayerType});
    CHECK(graphCanvasRootStack(inside) == "psd_root");
    CHECK(graphCanvasRowEntry(rows[0], "", false, inside) == GraphCanvasRowEntry{"logo_psd", "psd_root", true});
    CHECK(graphCanvasRowEntry(rows[0], "", false) == GraphCanvasRowEntry{"logo_psd", "", true});
    // A shape goes nowhere.
    CHECK_FALSE(graphCanvasRowEntry(rows[2], "", false));

    // The definition a layer reads as an import is read only; another is not.
    CHECK(graphCanvasImportedDefinition(nodes, "logo_psd"));
    CHECK_FALSE(graphCanvasImportedDefinition(nodes, "other"));
    CHECK_FALSE(graphCanvasImportedDefinition(nodes, ""));
    CHECK_FALSE(graphCanvasImportedDefinition(document(), "logo_psd"));

    // The mask's port and its line, as the row already says.
    CHECK(rows[3].mask == "card_mask");
    CHECK(rows[3].mask_source.empty());
}

TEST_CASE("SDK-1b: one stack tool decides where a layer goes, and which layer the ops added",
          "[studio][canvas][sdk1b]") {
    const auto nodes = document();
    const LayerStackTool no_pick{nodes, "", "hud", ""};
    CHECK_FALSE(no_pick.above().has_value());
    // The picked row is the index in `layers` (bottom first), not the row order.
    for (const auto &[picked, expected] :
         std::vector<std::pair<std::string, std::optional<std::size_t>>>{
             {"rect_1", 0}, {"ellipse_1", 2}, {"group_1", 3}, {"gone", std::nullopt}}) {
        INFO(picked);
        CHECK(LayerStackTool{nodes, "", "hud", picked}.above() == expected);
    }

    const LayerStackTool card{nodes, "", "hud", "ellipse_1"};
    for (const std::string what : {"rect", "ellipse", "group"}) {
        const auto expected = what == "group"
                                  ? graphCanvasAddGroupLayerOps(nodes, "", "hud", 2)
                                  : graphCanvasAddShapeLayerOps(nodes, "", "hud", what, 2);
        CHECK(card.addOps(what) == expected);
        CHECK(card.addOpsAt(what, std::nullopt) ==
              (what == "group" ? graphCanvasAddGroupLayerOps(nodes, "", "hud", std::nullopt)
                               : graphCanvasAddShapeLayerOps(nodes, "", "hud", what, std::nullopt)));
    }
    const auto adjustment = "adjustment:" + graphCanvasAdjustmentChoices().front().op;
    CHECK(card.addOps(adjustment) ==
          graphCanvasAddAdjustmentLayerOps(nodes, "", "hud", 2, graphCanvasAdjustmentChoices().front().op));
    CHECK(card.addOps("text").empty());
    CHECK(card.addOps("image").empty());
    CHECK(card.addOps("adjustment:not.a.filter").empty());

    const auto added = card.addOps("rect");
    const auto input = card.addedBy(added);
    REQUIRE_FALSE(input.empty());
    const auto rows = graphCanvasLayerRows(nodes, "hud");
    CHECK(std::ranges::find(rows, input, &GraphCanvasStackRow::input) == rows.end());
    CHECK(card.addedBy(graphCanvasSetLayerOps(nodes, "", "hud", 0, "name", "Renamed")).empty());
    Json elsewhere = Json::array();
    elsewhere.push_back(Json{{"op", "set_node_parameters"},
                             {"scope", "/elsewhere"},
                             {"node", "other"},
                             {"parameters", {{"layers", Json::array({{{"input", "new"}}})}}}});
    CHECK(card.addedBy(elsewhere).empty());
    CHECK(card.addedBy(Json::array()).empty());
}
