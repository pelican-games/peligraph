// CV-6a: canvas.place and layer depths -- the lowering and the placement's
// arithmetic, without a GPU.
#include "../src/project/canvas/canvaslowering.hpp"
#include "../src/project/canvas/canvasplace.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>

namespace {
using Json = nlohmann::json;
using Catch::Approx;

Json image(const std::string &id, bool depth) {
    const Json type = depth ? Json{{"semantic", "pelican.render.depth@1"},
                                   {"arguments", {{"representation", "device"}, {"space", "projection"}}}}
                            : Json{{"semantic", "pelican.render.color_signal@1"},
                                   {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
    return {{"id", id},
            {"op", "render.image"},
            {"parameters", {{"type", type},
                            {"format", depth ? "D32_SFLOAT" : "R16G16B16A16_SFLOAT"},
                            {"extent_scale", 1.0},
                            {"usage", depth ? Json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})
                                            : Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
            {"inputs", Json::object()},
            {"outputs", Json::array({"resource"})}};
}

// A board (400 x 200) placed in 3D: a background colour at depth 0, a card at
// depth 20, a half-transparent group at depth 10 (its inner layer's depth 5
// is ignored), a pass-through group at depth 30 whose layers add 4 and 0, and
// a last layer at depth -5, drawn first. position.y follows a curve.
Json placedDocument() {
    auto document = Json::parse(R"json({
  "schema": "pelican.graph", "version": 1, "definitions": {},
  "nodes": [
    {"id": "clock", "op": "value.input", "parameters": {"name": "time"}, "inputs": {}, "outputs": ["value"]},
    {"id": "lift", "op": "value.curve",
     "parameters": {"keys": [{"time": 0, "value": 0}, {"time": 1, "value": 2}]},
     "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]},
    {"id": "back", "op": "canvas.color", "parameters": {"color": [0.1, 0.1, 0.2, 1.0]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "card", "op": "shape.rect", "parameters": {"size": [100, 60], "center": [120, 80]}, "inputs": {}, "outputs": ["geometry"]},
    {"id": "card_fill", "op": "shape.fill", "parameters": {"color": [1, 0.5, 0, 1]},
     "inputs": {"geometry": {"node": "card", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "a", "op": "shape.ellipse", "parameters": {"size": [40, 40], "center": [300, 50]}, "inputs": {}, "outputs": ["geometry"]},
    {"id": "a_fill", "op": "shape.fill", "parameters": {"color": [0, 1, 0, 1]},
     "inputs": {"geometry": {"node": "a", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "b", "op": "shape.rect", "parameters": {"size": [40, 40], "center": [320, 70]}, "inputs": {}, "outputs": ["geometry"]},
    {"id": "b_fill", "op": "shape.fill", "parameters": {"color": [0, 0, 1, 1]},
     "inputs": {"geometry": {"node": "b", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "group", "op": "canvas.stack",
     "parameters": {"size": [400, 200], "layers": [{"input": "a", "name": "inner_a", "depth": 5}, {"input": "b"}]},
     "inputs": {"a": {"node": "a_fill", "port": "layer"}, "b": {"node": "b_fill", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "c", "op": "shape.ellipse", "parameters": {"size": [30, 30], "center": [60, 150]}, "inputs": {}, "outputs": ["geometry"]},
    {"id": "c_fill", "op": "shape.fill", "parameters": {"color": [1, 1, 1, 1]},
     "inputs": {"geometry": {"node": "c", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "d", "op": "shape.rect", "parameters": {"size": [30, 30], "center": [100, 150]}, "inputs": {}, "outputs": ["geometry"]},
    {"id": "d_fill", "op": "shape.fill", "parameters": {"color": [1, 0, 1, 1]},
     "inputs": {"geometry": {"node": "d", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "through", "op": "canvas.stack",
     "parameters": {"size": [400, 200], "layers": [{"input": "c", "name": "c", "depth": 4}, {"input": "d", "name": "d"}]},
     "inputs": {"c": {"node": "c_fill", "port": "layer"}, "d": {"node": "d_fill", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "low", "op": "shape.rect", "parameters": {"size": [20, 20], "center": [200, 100]}, "inputs": {}, "outputs": ["geometry"]},
    {"id": "low_fill", "op": "shape.fill", "parameters": {"color": [0.5, 0.5, 0.5, 1]},
     "inputs": {"geometry": {"node": "low", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "board", "op": "canvas.stack",
     "parameters": {"size": [400, 200], "layers": [
       {"input": "back", "name": "back"},
       {"input": "card", "name": "card", "depth": 20},
       {"input": "group", "name": "group", "depth": 10, "opacity": 0.5},
       {"input": "through", "name": "through", "depth": 30, "pass_through": true},
       {"input": "low", "name": "low", "depth": -5}]},
     "inputs": {"back": {"node": "back", "port": "layer"}, "card": {"node": "card_fill", "port": "layer"},
                "group": {"node": "group", "port": "layer"}, "through": {"node": "through", "port": "layer"},
                "low": {"node": "low_fill", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "sign", "op": "canvas.place",
     "parameters": {"position": [0, 1.2, 0], "rotation": [0, 0, 0], "scale": 1.0, "pixels_per_unit": 400,
                    "anchor": [0.5, 0.5]},
     "inputs": {"layer": {"node": "board", "port": "layer"},
                "color": {"node": "scene_color", "port": "resource"},
                "depth": {"node": "scene_depth", "port": "resource"},
                "position.y": {"node": "lift", "port": "value"}},
     "outputs": ["color"]},
    {"id": "after", "op": "render.fullscreen",
     "parameters": {"shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"},
                    "ports": {"source": {"kind": "image", "access": "read", "binding": "input", "input_index": 0,
                                         "type": {"semantic": "pelican.render.color_signal@1",
                                                  "arguments": {"reference": "scene", "transfer": "linear",
                                                                "range": "extended"}}}}},
     "inputs": {"source": {"node": "sign", "port": "color"}}, "outputs": []}
  ],
  "outputs": {}
})json");
    document.at("nodes").insert(document.at("nodes").begin(), image("scene_depth", true));
    document.at("nodes").insert(document.at("nodes").begin(), image("scene_color", false));
    return document;
}

void validate(const Json &document) {
    const auto &registry = Pelican::builtinGraphModuleRegistry();
    (void)Pelican::validateGraphPorts(Pelican::expandGraphDocument(document), registry.typeRegistry(),
                                      Pelican::GraphOperationContractResolver{[&](const Json &node) {
                                          return registry.resolveOperation(node);
                                      }});
}

const Json &nodeById(const Json &document, const std::string &id) {
    for (const auto &node : document.at("nodes"))
        if (node.at("id") == id) return node;
    FAIL("no node " << id);
    throw std::logic_error("unreachable");
}

Json &mutableNode(Json &document, const std::string &id) {
    for (auto &node : document.at("nodes"))
        if (node.at("id") == id) return node;
    throw std::logic_error("no node " + id);
}

template <typename Edit> std::string refusal(Edit edit) {
    auto document = placedDocument();
    edit(document);
    try {
        (void)Pelican::lowerCanvasNodes(document);
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code() + " " + error.detail().dump();
    }
    return "accepted";
}

const Pelican::CanvasOutput &placeOf(const Pelican::CanvasLowering &lowering) {
    for (const auto &output : lowering.ir.outputs)
        if (output.placement) return output;
    FAIL("no canvas.place in the lowering");
    throw std::logic_error("unreachable");
}
} // namespace

TEST_CASE("CV-6a canvas.place lowers to a raster onto the colour image, tested against the depth", "[canvas][cv6a]") {
    const auto authored = placedDocument();
    validate(authored);
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    const auto &document = lowering.document;
    validate(document);

    std::set<std::string> ids;
    for (const auto &node : document.at("nodes")) ids.insert(node.at("id").get<std::string>());
    REQUIRE(ids == std::set<std::string>{"scene_color", "scene_depth", "clock", "lift", "after", "sign$canvas_g0_image",
                                         "sign$canvas_g0_items", "sign$canvas_g0_bake", "sign$canvas_g0",
                                         "sign$canvas_items", "sign$canvas_bake", "sign$canvas_frame",
                                         "sign$canvas_set_0", "sign"});
    const auto &raster = nodeById(document, "sign");
    REQUIRE(raster.at("op") == "render.raster");
    REQUIRE(raster.at("outputs") == Json::array({"color", "depth"}));
    REQUIRE(raster.at("inputs").at("color") == Json{{"node", "scene_color"}, {"port", "resource"}});
    REQUIRE(raster.at("inputs").at("depth") == Json{{"node", "scene_depth"}, {"port", "resource"}});
    const auto &parameters = raster.at("parameters");
    REQUIRE(parameters.at("color_load_op") == "load");
    REQUIRE(parameters.at("depth_load_op") == "load");
    REQUIRE_FALSE(parameters.contains("clear_color"));
    const auto &state = parameters.at("raster_state");
    REQUIRE(state.at("depth_test") == true);
    REQUIRE(state.at("depth_write") == false);
    REQUIRE(state.at("depth_compare") == "less_equal");
    REQUIRE(state.at("color_attachments").at(0).at("blend").at("color") ==
            Json{{"src", "one"}, {"dst", "one_minus_src_alpha"}, {"op", "add"}});
    REQUIRE(parameters.at("ports").at("depth").at("attachment") == "depth");
    REQUIRE(parameters.at("ports").at("color").at("attachment") == "color");
    // The group is an image of the stack's size, one pixel per stack unit.
    const auto &group_image = nodeById(document, "sign$canvas_g0_image").at("parameters");
    REQUIRE(group_image.at("width") == 400);
    REQUIRE(group_image.at("height") == 200);
    // position.y reads the curve through the frame table.
    REQUIRE(nodeById(document, "sign$canvas_set_0").at("inputs").at("value") == Json{{"node", "lift"}, {"port", "value"}});

    // Back to front: by depth, then the stack's order. The half-transparent
    // group is one item at its layer's depth; the pass-through group's layers
    // add its depth to theirs.
    const auto &place = placeOf(lowering);
    REQUIRE(place.items.size() == 6);
    std::vector<std::pair<std::string, double>> order;
    for (const auto &item : place.items) order.emplace_back(item.nodes.front(), item.depth);
    REQUIRE(order == std::vector<std::pair<std::string, double>>{
                         {"low_fill", -5.0}, {"back", 0.0}, {"group", 10.0}, {"card_fill", 20.0},
                         {"d_fill", 30.0}, {"c_fill", 34.0}});
    REQUIRE(place.items.at(2).source == Pelican::CanvasItemSource::group);
    REQUIRE(place.items.at(2).opacity == 0.5);
    REQUIRE(lowering.diagnostics ==
            Json::array({{{"code", "canvas.depth_flattened"}, {"node", "group"}, {"layer", "inner_a"}},
                         {{"code", "canvas.depth_order"}, {"node", "board"}, {"layer", "group"}},
                         {{"code", "canvas.depth_order"}, {"node", "through"}, {"layer", "d"}},
                         {{"code", "canvas.depth_order"}, {"node", "board"}, {"layer", "low"}}}));

    // The depth sits after each item's own parameters: a constant, then the
    // edge-driven part.
    const auto &table = nodeById(document, "sign$canvas_items").at("parameters").at("initial_f32");
    const auto &bake = nodeById(document, "sign$canvas_bake").at("parameters").at("initial_f32");
    REQUIRE(table.at(5 * 16) == 2); // c: ellipse_fill
    const auto c_offset = static_cast<std::size_t>(table.at(5 * 16 + 1).get<double>());
    REQUIRE(bake.at(c_offset + 9) == 34.0);
    REQUIRE(bake.at(c_offset + 10) == 0.0);

    const auto &vertex = lowering.generated_sources.at(lowering.generated_sources.size() - 2);
    REQUIRE(vertex.stage == "vert");
    REQUIRE(vertex.text.find("canvasPlaceRotation") != std::string::npos);
    REQUIRE(vertex.text.find("const float PLACE_UNITS = 400.0;") != std::string::npos);
    REQUIRE(vertex.text.find("pelicanFrame.projection * pelicanFrame.view") != std::string::npos);
    REQUIRE(vertex.text.find("One side only") == std::string::npos);

    // Deterministic.
    REQUIRE(Pelican::lowerCanvasNodes(authored).document.dump() == document.dump());

    // canvas.output ignores the depths, without a word.
    auto flat = authored;
    for (auto &node : flat.at("nodes"))
        if (node.at("id") == "sign")
            node = {{"id", "sign"}, {"op", "canvas.output"}, {"parameters", Json::object()},
                    {"inputs", {{"layer", {{"node", "board"}, {"port", "layer"}}}}}, {"outputs", Json::array({"image"})}};
    mutableNode(flat, "after").at("inputs").at("source") = {{"node", "sign"}, {"port", "image"}};
    const auto output = Pelican::lowerCanvasNodes(flat);
    REQUIRE(output.diagnostics == Json::array());
    for (const auto &item : output.ir.outputs.at(0).items) REQUIRE(item.depth == 0.0);
}

TEST_CASE("CV-6a depth_write, one-sided placement and edge-driven depths", "[canvas][cv6a]") {
    auto authored = placedDocument();
    auto &sign = mutableNode(authored, "sign");
    sign.at("parameters")["depth_write"] = true;
    sign.at("parameters")["double_sided"] = false;
    mutableNode(authored, "board").at("inputs")["card.depth"] = {{"node", "lift"}, {"port", "value"}};
    validate(authored);
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    const auto &document = lowering.document;
    validate(document);
    // The colour draw, then the depth draw keeping the place's id.
    const auto &colour = nodeById(document, "sign$canvas_draw0");
    const auto &depth = nodeById(document, "sign");
    REQUIRE(colour.at("parameters").at("raster_state").at("depth_write") == false);
    REQUIRE(depth.at("parameters").at("raster_state").at("depth_write") == true);
    REQUIRE(depth.at("parameters").at("raster_state").at("color_attachments").at(0) == Json{{"write_mask", "none"}});
    REQUIRE(depth.at("inputs").at("color") == Json{{"node", "sign$canvas_draw0"}, {"port", "color"}});
    REQUIRE(depth.at("inputs").at("depth") == Json{{"node", "sign$canvas_draw0"}, {"port", "depth"}});
    REQUIRE(depth.at("inputs").at("items") == colour.at("inputs").at("items"));
    const auto &sources = lowering.generated_sources;
    REQUIRE(sources.back().text.find("if (color.a < 0.5) discard;") != std::string::npos);
    REQUIRE(sources.at(sources.size() - 3).text.find("if (color.a <= 0.0) discard;") != std::string::npos);
    REQUIRE(sources.at(sources.size() - 2).text.find("One side only") != std::string::npos);
    // The card's depth follows the curve: the edge part reads the frame table.
    const auto &place = placeOf(lowering);
    const auto card = std::find_if(place.items.begin(), place.items.end(),
                                   [](const Pelican::CanvasItem &item) { return item.nodes.front() == "card_fill"; });
    REQUIRE(card != place.items.end());
    REQUIRE(card->depth == 0.0);
    REQUIRE(card->depth_edge.has_value());
    REQUIRE(card->depth_edge->edge == Pelican::CanvasValueSource{"lift", "value"});

    // depth_write without a depth edge is refused.
    auto no_depth = placedDocument();
    mutableNode(no_depth, "sign").at("parameters")["depth_write"] = true;
    mutableNode(no_depth, "sign").at("inputs").erase("depth");
    REQUIRE_THROWS_AS(Pelican::lowerCanvasNodes(no_depth), std::invalid_argument);
    // Without a depth edge: no depth test, no depth port.
    auto untested = placedDocument();
    mutableNode(untested, "sign").at("inputs").erase("depth");
    const auto lowered = Pelican::lowerCanvasNodes(untested);
    const auto &raster = nodeById(lowered.document, "sign");
    REQUIRE(raster.at("outputs") == Json::array({"color"}));
    REQUIRE(raster.at("parameters").at("raster_state").at("depth_test") == false);
    REQUIRE_FALSE(raster.at("parameters").at("ports").contains("depth"));
}

TEST_CASE("CV-6a canvas.place refuses by name", "[canvas][cv6a]") {
    REQUIRE(refusal([](Json &document) { mutableNode(document, "sign").at("inputs").erase("layer"); }) ==
            R"(canvas.place_unconnected@1 {"node":"sign","port":"layer"})");
    REQUIRE(refusal([](Json &document) { mutableNode(document, "sign").at("inputs").erase("color"); }) ==
            R"(canvas.place_unconnected@1 {"node":"sign","port":"color"})");
    REQUIRE(refusal([](Json &document) {
                document.at("nodes").push_back({{"id", "sign$canvas_items"}, {"op", "value.constant"},
                                                {"parameters", {{"value", 1}}}, {"inputs", Json::object()},
                                                {"outputs", Json::array({"value"})}});
            }) == R"(canvas.reserved_id@1 {"id":"sign$canvas_items"})");
    // CV-2b: a layer drawn straight into the scene that reads its backdrop.
    REQUIRE(refusal([](Json &document) {
                mutableNode(document, "board").at("parameters").at("layers").at(1)["blend"] = "multiply";
            }) == R"(canvas.place_backdrop_unsupported@1 {"layer":"card","node":"board"})");
    REQUIRE(refusal([](Json &document) {
                document.at("nodes").push_back({{"id", "bd"}, {"op", "canvas.backdrop"}, {"inputs", Json::object()},
                                                {"outputs", Json::array({"layer"})}});
                auto &board = mutableNode(document, "board");
                board.at("parameters").at("layers").push_back({{"input", "bd"}, {"name", "bd"}});
                board.at("inputs")["bd"] = {{"node", "bd"}, {"port", "layer"}};
            }) == R"(canvas.place_backdrop_unsupported@1 {"layer":"bd","node":"board"})");
    // Inside a nested stack, composited first, a blend is drawn.
    REQUIRE(refusal([](Json &document) {
                mutableNode(document, "group").at("parameters").at("layers").at(1)["blend"] = "multiply";
            }) == "accepted");
}

TEST_CASE("CV-6a a placed item's colour filters sit after its depth", "[canvas][cv6a][cv2b]") {
    auto authored = placedDocument();
    authored.at("nodes").push_back({{"id", "tint"},
                                    {"op", "filter.color_overlay"},
                                    {"parameters", {{"color", {0.25, 0.5, 0.75, 1.0}}}},
                                    {"inputs", {{"layer", {{"node", "card_fill"}, {"port", "layer"}}}}},
                                    {"outputs", Json::array({"layer"})}});
    mutableNode(authored, "board").at("inputs").at("card") = {{"node", "tint"}, {"port", "layer"}};
    validate(authored);
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    validate(lowering.document);
    const auto &place = placeOf(lowering);
    REQUIRE(place.items.at(3).nodes.front() == "card_fill");
    REQUIRE(place.items.at(3).filters.size() == 1);
    const auto &table = nodeById(lowering.document, "sign$canvas_items").at("parameters").at("initial_f32");
    const auto &bake = nodeById(lowering.document, "sign$canvas_bake").at("parameters").at("initial_f32");
    // A rect fill (kind 0) with style 1; its ten parameters, the depth's two,
    // then the filter's numbers.
    REQUIRE(table.at(3 * 16) == 256);
    const auto offset = static_cast<std::size_t>(table.at(3 * 16 + 1).get<double>());
    REQUIRE(bake.at(offset + 10) == 20.0);
    REQUIRE(bake.at(offset + 11) == 0.0);
    const auto &constants = place.items.at(3).filters.front().constants;
    for (std::size_t index = 0; index < constants.size(); ++index)
        REQUIRE(bake.at(offset + 12 + index) == Approx(constants[index]));
    const auto &sources = lowering.generated_sources;
    const auto &vertex = sources.at(sources.size() - 2).text;
    const auto &fragment = sources.back().text;
    REQUIRE(vertex.find("uint kind = uint(it[0].x + 0.5) % 256u;") != std::string::npos);
    REQUIRE(fragment.find("case 0u: return 12u;") != std::string::npos);
    REQUIRE(fragment.find("color = canvasStyle(it, color);") != std::string::npos);
    REQUIRE(fragment.find("canvasBackdrop") == std::string::npos);
}

TEST_CASE("CV-6a the placement's matrix and an item's corners", "[canvas][cv6a]") {
    Pelican::CanvasOutput output;
    output.id = "sign";
    output.size = {400, 200};
    Pelican::CanvasPlacement placement;
    placement.pixels_per_unit = 100;
    placement.anchor = {0.0, 1.0}; // bottom left
    placement.position = {Pelican::CanvasParameter{1.0}, Pelican::CanvasParameter{2.0}, Pelican::CanvasParameter{3.0}};
    placement.rotation = {Pelican::CanvasParameter{90.0}, Pelican::CanvasParameter{90.0}, Pelican::CanvasParameter{0.0}};
    placement.scale = Pelican::CanvasParameter{2.0};
    output.placement = placement;
    const auto m = Pelican::canvasPlacementMatrix(output);
    const auto at = [&](double x, double y, double z) {
        return std::array<double, 3>{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
                                     m[2] * x + m[6] * y + m[10] * z + m[14]};
    };
    const auto near = [](std::array<double, 3> a, std::array<double, 3> b) {
        for (std::size_t k = 0; k < 3; ++k)
            if (a[k] != Approx(b[k]).margin(1e-9)) return false;
        return true;
    };
    // The anchor (bottom left) is the position.
    REQUIRE(near(at(0, 200, 0), {1, 2, 3}));
    // One unit right (+x, 100 stack units, times scale 2): X turns nothing,
    // then Y by 90 turns +x to -z.
    REQUIRE(near(at(100, 200, 0), {1, 2, 1}));
    // One unit up (the stack's -y): X by 90 turns +y to +z, then Y turns +z to +x.
    REQUIRE(near(at(0, 100, 0), {3, 2, 3}));
    // One unit of depth (toward the viewer, +z): X turns it to -y.
    REQUIRE(near(at(0, 200, 100), {1, 0, 3}));
    // An edge-driven number reads the values; without one it is refused.
    output.placement->position[0] = Pelican::CanvasParameter{0.0, Pelican::CanvasValueSource{"x", "value"}};
    REQUIRE_THROWS_AS(Pelican::canvasPlacementMatrix(output), std::invalid_argument);
    const auto moved = Pelican::canvasPlacementMatrix(output, {{Pelican::CanvasValueSource{"x", "value"}, 5.0}});
    // The anchor (0, 200) is now at x = 5.
    REQUIRE(moved[4] * 200 + moved[12] == Approx(5.0));

    // A rect item at depth 50, seen through identity matrices: clip = world.
    output.placement = Pelican::CanvasPlacement{};
    output.placement->pixels_per_unit = 200; // the 400 x 200 board is 2 x 1, centred
    Pelican::CanvasItem card;
    card.parameters = {Pelican::CanvasParameter{100.0}, Pelican::CanvasParameter{50.0}, Pelican::CanvasParameter{300.0},
                       Pelican::CanvasParameter{50.0},  Pelican::CanvasParameter{0.0},  Pelican::CanvasParameter{0.0},
                       Pelican::CanvasParameter{1.0},   Pelican::CanvasParameter{1.0},  Pelican::CanvasParameter{1.0},
                       Pelican::CanvasParameter{1.0}};
    card.depth = 50;
    output.items = {card};
    const Pelican::CanvasMatrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto corners = Pelican::canvasPlacedItemCorners(output, 0, identity, identity, {800, 400});
    REQUIRE(corners.in_front);
    REQUIRE(corners.depth == 50);
    // Top left of the card: stack (250, 25) -> world (0.25, 0.375, 0.25).
    REQUIRE(corners.stack[0] == std::array<double, 2>{250, 25});
    REQUIRE(near(corners.world[0], {0.25, 0.375, 0.25}));
    REQUIRE(corners.pixel[0][0] == Approx(500));
    REQUIRE(corners.pixel[0][1] == Approx(275));
    // Bottom right: stack (350, 75) -> world (0.75, 0.125, 0.25).
    REQUIRE(near(corners.world[2], {0.75, 0.125, 0.25}));

    // Seen as the engine sees it (docs/design_coordinate_convention.md): a
    // camera at z = 3 without a rotation looks down -z, and the projection
    // flips clip y. The card, right of and above the board's middle, is right
    // of and above the frame's middle, and its stack top left is the frame's.
    const Pelican::CanvasMatrix view{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -3, 1};
    const double t = std::tan(0.7853981633974483 / 2.0), near_plane = 0.1, far_plane = 100.0;
    Pelican::CanvasMatrix projection{};
    projection[0] = 1.0 / (2.0 * t);
    projection[5] = -1.0 / t;
    projection[10] = far_plane / (near_plane - far_plane);
    projection[11] = -1.0;
    projection[14] = -(far_plane * near_plane) / (far_plane - near_plane);
    const auto seen = Pelican::canvasPlacedItemCorners(output, 0, view, projection, {800, 400});
    REQUIRE(seen.in_front);
    REQUIRE(seen.pixel[0][0] > 400.0);
    REQUIRE(seen.pixel[0][1] < 200.0);
    REQUIRE(seen.pixel[1][0] > seen.pixel[0][0]); // stack +x is the frame's right
    REQUIRE(seen.pixel[3][1] > seen.pixel[0][1]); // stack +y is the frame's down
}
