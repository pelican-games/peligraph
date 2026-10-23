// CV-3a: the neighbourhood filters (filter.blur, filter.drop_shadow,
// filter.outer_glow), value edges into the colour filters' numbers, and the
// fixed-size image of canvas.output `size` -- the lowering and the CPU
// reference, without a GPU.
#include "../src/project/canvas/canvasblend.hpp"
#include "../src/project/canvas/canvaslowering.hpp"
#include "../src/project/canvas/canvasreference.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using Catch::Matchers::WithinAbs;

void validate(const Json &document) {
    const auto &registry = Pelican::builtinGraphModuleRegistry();
    (void)Pelican::validateGraphPorts(Pelican::expandGraphDocument(document), registry.typeRegistry(),
                                      Pelican::GraphOperationContractResolver{[&](const Json &node) {
                                          return registry.resolveOperation(node);
                                      }});
}

const Json *findNode(const Json &document, const std::string &id) {
    for (const auto &node : document.at("nodes"))
        if (node.at("id") == id) return &node;
    return nullptr;
}

const Json &nodeById(const Json &document, const std::string &id) {
    const auto *found = findNode(document, id);
    if (found == nullptr) FAIL("no node " << id);
    return *found;
}

std::size_t rasters(const Json &document) {
    return static_cast<std::size_t>(std::count_if(document.at("nodes").begin(), document.at("nodes").end(),
                                                  [](const Json &node) { return node.at("op") == "render.raster"; }));
}

Json color(const std::string &id, std::array<double, 4> rgba) {
    return {{"id", id}, {"op", "canvas.color"}, {"parameters", {{"color", {rgba[0], rgba[1], rgba[2], rgba[3]}}}},
            {"outputs", Json::array({"layer"})}};
}

Json image(const std::string &id, std::array<double, 2> offset, std::array<double, 2> size) {
    return {{"id", id},
            {"op", "canvas.image"},
            {"parameters", {{"asset", "project://card.png"}, {"offset", {offset[0], offset[1]}}, {"size", {size[0], size[1]}}}},
            {"outputs", Json::array({"layer"})}};
}

Json rect(const std::string &id, std::array<double, 2> size, std::array<double, 2> center) {
    return {{"id", id},
            {"op", "shape.rect"},
            {"parameters", {{"size", {size[0], size[1]}}, {"center", {center[0], center[1]}}}},
            {"outputs", Json::array({"geometry"})}};
}

Json fill(const std::string &id, const std::string &geometry, std::array<double, 4> rgba) {
    return {{"id", id},
            {"op", "shape.fill"},
            {"parameters", {{"color", {rgba[0], rgba[1], rgba[2], rgba[3]}}}},
            {"inputs", {{"geometry", {{"node", geometry}, {"port", "geometry"}}}}},
            {"outputs", Json::array({"layer"})}};
}

Json filter(const std::string &id, const std::string &op, const std::string &input, Json parameters = Json::object()) {
    Json node{{"id", id}, {"op", op}, {"inputs", {{"layer", {{"node", input}, {"port", "layer"}}}}},
              {"outputs", Json::array({"layer"})}};
    if (!parameters.empty()) node["parameters"] = std::move(parameters);
    return node;
}

Json constant(const std::string &id, double value) {
    return {{"id", id}, {"op", "value.constant"}, {"parameters", {{"value", value}}}, {"outputs", Json::array({"value"})}};
}

Json edge(const std::string &node) { return {{"node", node}, {"port", "value"}}; }

Json stack(const std::string &id, const std::vector<std::string> &inputs, std::array<double, 2> size,
           Json extra = Json::object()) {
    Json layers = Json::array(), edges = Json::object();
    for (const auto &input : inputs) {
        layers.push_back({{"input", input}, {"name", input}});
        edges[input] = {{"node", input}, {"port", "layer"}};
    }
    Json parameters{{"size", {size[0], size[1]}}, {"layers", layers}};
    parameters.update(extra);
    return {{"id", id}, {"op", "canvas.stack"}, {"parameters", parameters}, {"inputs", edges},
            {"outputs", Json::array({"layer"})}};
}

Json document(Json nodes, const std::string &top, Json output = {{"fit", "stretch"}}) {
    nodes.push_back({{"id", "out"}, {"op", "canvas.output"}, {"parameters", std::move(output)},
                     {"inputs", {{"layer", {{"node", top}, {"port", "layer"}}}}}, {"outputs", Json::array({"image"})}});
    return {{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()}, {"nodes", nodes},
            {"outputs", Json::object()}};
}

template <typename Body> std::string refusal(Body body) {
    try {
        body();
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code() + " " + error.detail().dump();
    }
    return "accepted";
}

std::vector<Json> diagnosticsOf(const Pelican::CanvasLowering &lowering, const std::string &code) {
    std::vector<Json> found;
    for (const auto &diagnostic : lowering.diagnostics)
        if (diagnostic.at("code") == code) found.push_back(diagnostic);
    return found;
}

// A 6 x 4 picture: opaque white but for a transparent first column.
Pelican::CanvasReferenceImages cardImages() {
    Pelican::CanvasReferenceImage card{6, 4, {}};
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 6; ++x)
            card.rgba.insert(card.rgba.end(), {255, 255, 255, static_cast<std::uint8_t>(x == 0 ? 0 : 255)});
    return {{"project://card.png", card}};
}

const Pelican::CanvasOutput &onlyOutput(const Pelican::CanvasIr &ir) {
    REQUIRE(ir.outputs.size() == 1);
    return ir.outputs.front();
}

std::array<double, 4> pixel(const Pelican::CanvasImage &image, std::uint32_t x, std::uint32_t y) {
    const auto *p = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

} // namespace

TEST_CASE("CV-3a the effect arithmetic: the normal CDF and its inverse, spread, coverage, the halved sigma",
          "[canvas][cv3a]") {
    CHECK_THAT(Pelican::canvasNormalCdf(0.0), WithinAbs(0.5, 1e-7));
    CHECK_THAT(Pelican::canvasNormalCdf(1.959964), WithinAbs(0.975, 1e-6));
    CHECK_THAT(Pelican::canvasNormalCdf(-1.0) + Pelican::canvasNormalCdf(1.0), WithinAbs(1.0, 1e-7));
    // The two approximations meet to 2e-5 (1e-4 past 3 sigma, where erf's
    // 1.5e-7 is most of the tail).
    for (double x = -3.5; x <= 3.5; x += 0.25)
        CHECK_THAT(Pelican::canvasProbit(Pelican::canvasNormalCdf(x)), WithinAbs(x, std::abs(x) > 3.0 ? 1e-4 : 2e-5));
    // No spread is the blurred alpha itself.
    for (const auto b : {0.0, 0.1, 0.5, 0.93, 1.0}) CHECK(Pelican::canvasSpreadAlpha(b, 9.0, 0.0) == b);
    // A straight edge blurred with sigma = S / 3 and spread by s is Photoshop's
    // (spread by s S, then faded with (1 - s) S / 3), exactly.
    const double size = 30.0;
    for (const auto spread : {0.2, 0.5, 0.8})
        for (double x = -10.0; x <= 30.0; x += 2.5) {
            const auto blurred = Pelican::canvasNormalCdf(-x / (size / 3.0));
            const auto expected = Pelican::canvasNormalCdf(-(x - spread * size) / ((1.0 - spread) * size / 3.0));
            CHECK_THAT(Pelican::canvasSpreadAlpha(blurred, size, spread), WithinAbs(expected, 2e-4));
        }
    // From a distance: half at spread * S, a third of sigma out past it 0.37.
    CHECK_THAT(Pelican::canvasEffectCoverage(0.25 * 12.0, 0.0, 12.0, 0.25), WithinAbs(0.5, 1e-7));
    CHECK_THAT(Pelican::canvasEffectCoverage(0.25 * 12.0 + 3.0, 0.0, 12.0, 0.25),
               WithinAbs(Pelican::canvasNormalCdf(-1.0), 1e-7));
    // No radius: the pixel's antialiasing alone (as wide as a fill's).
    CHECK_THAT(Pelican::canvasEffectCoverage(0.0, 1.0, 0.0, 0.0), WithinAbs(0.5, 1e-7));
    CHECK(Pelican::canvasEffectCoverage(1.0, 1.0, 0.0, 0.0) < 0.01);
    // The halving's and the read back's blur come off the sigma.
    CHECK(Pelican::canvasBlurLevelSigma(7.0, 0) == 7.0);
    CHECK_THAT(Pelican::canvasBlurLevelSigma(40.0, 2), WithinAbs(std::sqrt(1600.0 - (15.0 / 12.0 + 16.0 / 6.0)) / 4.0, 1e-9));
    CHECK(Pelican::canvasBlurLevelSigma(0.5, 3) == 0.0);
    CHECK(Pelican::canvasBlurWeight(0, 0.0) == 1.0);
    CHECK(Pelican::canvasBlurWeight(1, 0.0) == 0.0);
}

TEST_CASE("CV-3a an image's shadow, glow and blur are drawn through images: two passes each, one more per halving",
          "[canvas][cv3a]") {
    const auto with = [](Json filter_node) {
        return document(Json::array({color("back", {0.1, 0.1, 0.1, 1.0}), image("card", {8, 8}, {24, 16}),
                                     std::move(filter_node), stack("root", {"back", "fx"}, {64, 32})}),
                        "root");
    };
    const auto plain = document(Json::array({color("back", {0.1, 0.1, 0.1, 1.0}), image("fx", {8, 8}, {24, 16}),
                                             stack("root", {"back", "fx"}, {64, 32})}),
                                "root");
    const auto base = Pelican::lowerCanvasNodes(plain);
    REQUIRE(rasters(base.document) == 1);

    // A drop shadow: the card's alpha blurred across and then down (sigma 10 / 3,
    // no halving), read under the card.
    const auto shadow = with(filter("fx", "filter.drop_shadow", "card", {{"offset", {3, 2}}, {"radius", 10}}));
    validate(shadow);
    const auto lowered = Pelican::lowerCanvasNodes(shadow);
    validate(lowered.document);
    CHECK(rasters(lowered.document) == rasters(base.document) + 2);
    const auto split = diagnosticsOf(lowered, "canvas.draw_split");
    REQUIRE(split.size() == 1);
    CHECK(split.front() == Json{{"code", "canvas.draw_split"}, {"output", "out"}, {"reason", "neighbourhood_filter"},
                                {"node", "fx"}, {"op", "filter.drop_shadow"}, {"why", "no_distance"},
                                {"passes", 2}, {"levels", 0}});
    // Named as the canvas's other nodes; the images are fixed, with an extent scale beside.
    const auto &across = nodeById(lowered.document, "out$canvas_blur0_across");
    const auto &down = nodeById(lowered.document, "out$canvas_blur0");
    CHECK(down.at("inputs").at("t0") == Json{{"node", "out$canvas_blur0_across"}, {"port", "image"}});
    CHECK(across.at("inputs").at("t0") == Json{{"node", "out$canvas_asset0"}, {"port", "resource"}});
    const auto &image_parameters = nodeById(lowered.document, "out$canvas_blur0_image").at("parameters");
    CHECK(image_parameters.at("extent_scale") == 1.0);
    // The card's 24 x 16 at (8, 8), 3 sigma + 1 around it (11), clipped to
    // what the canvas shows plus the same (here: nothing clipped).
    const auto &ir_blur = onlyOutput(lowered.ir).blurs.at(0);
    CHECK(ir_blur.rect == std::array<double, 4>{-3.0, -3.0, 46.0, 38.0});
    CHECK(image_parameters.at("width") == 46);
    CHECK(image_parameters.at("height") == 38);
    CHECK(ir_blur.taps == 10);
    // The main draw: the shadow image, then the card.
    std::vector<std::string> kinds;
    for (const auto &item : lowered.items) kinds.push_back(item.at("kind"));
    CHECK(kinds == std::vector<std::string>{"color", "image_effect", "image_rgba"});
    CHECK(lowered.items.at(1).at("effect") == Json{{"node", "fx"}, {"op", "filter.drop_shadow"}});

    // A blur reads its own image alone; its radius 2 is sigma 2.
    const auto blurred = Pelican::lowerCanvasNodes(with(filter("fx", "filter.blur", "card", {{"radius", 2}})));
    CHECK(rasters(blurred.document) == rasters(base.document) + 2);
    CHECK(diagnosticsOf(blurred, "canvas.draw_split").at(0).at("why") == "blur");
    CHECK(onlyOutput(blurred.ir).blurs.at(0).taps == 6);
    kinds.clear();
    for (const auto &item : blurred.items) kinds.push_back(item.at("kind"));
    CHECK(kinds == std::vector<std::string>{"color", "image_rgba"});

    // A glow of size 120 reaches past 32 pixels: halved twice, sigma 40 / 4 less the halving's.
    const auto glow = Pelican::lowerCanvasNodes(with(filter("fx", "filter.outer_glow", "card", {{"radius", 120}})));
    CHECK(rasters(glow.document) == rasters(base.document) + 4);
    const auto glow_split = diagnosticsOf(glow, "canvas.draw_split").at(0);
    CHECK(glow_split.at("passes") == 4);
    CHECK(glow_split.at("levels") == 2);
    CHECK(findNode(glow.document, "out$canvas_blur0_down1") != nullptr);
    CHECK(findNode(glow.document, "out$canvas_blur0_down2") != nullptr);
    CHECK(nodeById(glow.document, "out$canvas_blur0_down2").at("inputs").at("t0") ==
          Json{{"node", "out$canvas_blur0_down1"}, {"port", "image"}});
    const auto &glow_blur = onlyOutput(glow.ir).blurs.at(0);
    CHECK(glow_blur.levels == 2);
    CHECK(static_cast<int>(glow_blur.rect[2]) % 4 == 0);
    CHECK(static_cast<int>(glow_blur.rect[3]) % 4 == 0);
    CHECK(glow_blur.taps == static_cast<std::uint32_t>(std::ceil(3.0 * Pelican::canvasBlurLevelSigma(40.0, 2) - 1e-9)));
    validate(glow.document);
}

TEST_CASE("CV-3a a shape's shadow and glow come from its distance in the same draw", "[canvas][cv3a]") {
    const auto glowing = document(Json::array({color("back", {0.0, 0.0, 0.0, 1.0}), rect("box", {20, 20}, {32, 32}),
                                               fill("box_fill", "box", {1, 1, 1, 1}),
                                               filter("fx", "filter.outer_glow", "box_fill",
                                                      {{"radius", 9}, {"color", {1, 1, 1}}, {"opacity", 1}}),
                                               stack("root", {"back", "fx"}, {64, 64})}),
                                  "root");
    validate(glowing);
    const auto lowered = Pelican::lowerCanvasNodes(glowing);
    validate(lowered.document);
    CHECK(rasters(lowered.document) == 1);
    CHECK(diagnosticsOf(lowered, "canvas.draw_split").empty());
    std::vector<std::string> kinds;
    for (const auto &item : lowered.items) kinds.push_back(item.at("kind"));
    CHECK(kinds == std::vector<std::string>{"color", "rect_fill_effect", "rect_fill"});
    // 3 pixels out from the box's edge (x 22..42): Phi(-3 / sigma), sigma 3
    // and the pixel's antialiasing beside it.
    const auto picture = Pelican::renderCanvasReference(onlyOutput(lowered.ir), 64, 64);
    const auto glow = pixel(picture, 44, 32);
    const auto expected = Pelican::canvasEffectCoverage(2.5, 1.0, 9.0, 0.0);
    CHECK_THAT(glow[0], WithinAbs(expected, 1e-5));
    CHECK_THAT(glow[3], WithinAbs(1.0, 1e-6));
    // Inside, the box itself.
    CHECK_THAT(pixel(picture, 32, 32)[0], WithinAbs(1.0, 1e-6));
    // Far out, the background.
    CHECK_THAT(pixel(picture, 60, 60)[0], WithinAbs(0.0, 1e-6));

    // Under a layer's opacity the shadow and the shape are one group: the
    // shadow does not show through.
    auto faded = glowing;
    for (auto &node : faded.at("nodes"))
        if (node.at("id") == "root") node.at("parameters").at("layers").at(1)["opacity"] = 0.5;
    const auto faded_lowering = Pelican::lowerCanvasNodes(faded);
    CHECK(onlyOutput(faded_lowering.ir).groups.size() == 1);
    CHECK(rasters(faded_lowering.document) == 2);
}

TEST_CASE("CV-3a a radius an edge drives needs max_radius; the frame table carries it", "[canvas][cv3a]") {
    const auto driven = [](Json parameters) {
        auto node = filter("fx", "filter.blur", "card", std::move(parameters));
        node.at("inputs")["radius"] = edge("radius_value");
        return document(Json::array({constant("radius_value", 1.0), image("card", {8, 8}, {24, 16}), std::move(node),
                                     stack("root", {"fx"}, {64, 32})}),
                        "root");
    };
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(driven({})); }) ==
          R"(canvas.range_unbounded@1 {"node":"fx","parameter":"radius"})");
    const auto authored = driven({{"max_radius", 8}});
    validate(authored);
    const auto lowered = Pelican::lowerCanvasNodes(authored);
    validate(lowered.document);
    // The passes are made for 8 (sigma 8: 24 taps); the radius comes from the frame table.
    const auto &blur = onlyOutput(lowered.ir).blurs.at(0);
    CHECK(blur.taps == 24);
    CHECK(blur.radius_limit == 8.0);
    CHECK(nodeById(lowered.document, "out$canvas_blur0_set_0").at("inputs").at("value") == edge("radius_value"));
    CHECK(nodeById(lowered.document, "out$canvas_blur0_set_0").at("parameters").at("target") == "out$canvas_blur0_frame");
    const auto &down = nodeById(lowered.document, "out$canvas_blur0");
    CHECK(down.at("inputs").contains("frame"));
    CHECK_FALSE(down.at("inputs").contains("bake"));
    // Two radii, two pictures; past max_radius it is drawn at it.
    const auto &output = onlyOutput(lowered.ir);
    const auto images = cardImages();
    const auto at = [&](double radius) {
        return Pelican::renderCanvasReference(output, 64, 32, {{{"radius_value", "value"}, radius}}, images);
    };
    const auto one = at(1.0), six = at(6.0);
    CHECK(pixel(one, 8, 16)[3] != pixel(six, 8, 16)[3]);
    CHECK(at(8.0).pixels == at(20.0).pixels);
    // An authored radius past the limit is drawn at it, and said.
    const auto clamped = Pelican::lowerCanvasNodes(driven({{"radius", 12}, {"max_radius", 8}}));
    REQUIRE(diagnosticsOf(clamped, "canvas.radius_clamped").size() == 1);
    CHECK(diagnosticsOf(clamped, "canvas.radius_clamped").front() ==
          Json{{"code", "canvas.radius_clamped"}, {"node", "fx"}, {"radius", 12.0}, {"max_radius", 8.0}});
    // Past Photoshop's 250, refused.
    CHECK_THROWS_AS(Pelican::lowerCanvasNodes(driven({{"max_radius", 300}})), std::invalid_argument);
}

TEST_CASE("CV-3a the blur the CPU reference draws is the passes': across, then down", "[canvas][cv3a]") {
    // The card four times its size at (16, 8): x 20..40 opaque (its first
    // column, x 16..20, transparent), y 8..24; blurred with sigma 1.5.
    const auto authored = document(Json::array({image("card", {16, 8}, {24, 16}),
                                                filter("fx", "filter.blur", "card", {{"radius", 1.5}}),
                                                stack("root", {"fx"}, {64, 32})}),
                                   "root");
    const auto ir = Pelican::buildCanvasIr(authored);
    const auto &output = onlyOutput(ir);
    REQUIRE(output.blurs.size() == 1);
    CHECK(output.blurs.front().taps == 5);
    const auto picture = Pelican::renderCanvasReference(output, 64, 32, {}, cardImages());
    // Far from the card: nothing. Inside, 4 sigma from its edges: all of it.
    CHECK(pixel(picture, 2, 2)[3] == 0.0f);
    CHECK(pixel(picture, 50, 2)[3] == 0.0f);
    CHECK_THAT(pixel(picture, 30, 16)[3], WithinAbs(1.0, 1e-3));
    // Across its top and bottom edges, the Gaussian's CDF: half on the edge.
    CHECK_THAT(pixel(picture, 30, 7)[3], WithinAbs(Pelican::canvasNormalCdf(-0.5 / 1.5), 0.02));
    CHECK_THAT(pixel(picture, 30, 8)[3], WithinAbs(Pelican::canvasNormalCdf(0.5 / 1.5), 0.02));
    CHECK_THAT(pixel(picture, 30, 24)[3], WithinAbs(Pelican::canvasNormalCdf(-0.5 / 1.5), 0.02));
    // And so across its right one, the bilinear read of the card's last texel aside.
    CHECK(pixel(picture, 40, 16)[3] > 0.2);
    CHECK(pixel(picture, 40, 16)[3] < 0.6);
}

TEST_CASE("CV-3a colour filters take value edges into the frame table; brightness_contrast's curve takes none",
          "[canvas][cv3a]") {
    const auto with = [](Json filter_node, std::vector<Json> values) {
        Json nodes = Json::array();
        for (auto &value : values) nodes.push_back(std::move(value));
        nodes.push_back(color("red", {1.0, 0.0, 0.0, 1.0}));
        nodes.push_back(std::move(filter_node));
        nodes.push_back(stack("root", {"f"}, {4, 4}, {{"blend_space", "srgb"}}));
        return document(std::move(nodes), "root");
    };
    const auto centre = [](const Json &authored, Pelican::CanvasValues values) {
        const auto ir = Pelican::buildCanvasIr(authored);
        const auto picture = Pelican::renderCanvasReference(onlyOutput(ir), 4, 4, values);
        return pixel(picture, 1, 1);
    };
    // Hue / saturation: the hue an edge drives (180: red to cyan).
    auto hue = filter("f", "filter.hue_saturation", "red", {{"hue", 0}});
    hue.at("inputs")["hue"] = edge("hue_value");
    const auto hued = with(hue, {constant("hue_value", 180.0)});
    validate(hued);
    const auto lowered = Pelican::lowerCanvasNodes(hued);
    validate(lowered.document);
    // An sRGB stack under the linear output is composited first: group 0's draw.
    CHECK(nodeById(lowered.document, "out$canvas_g0_set_0").at("inputs").at("value") == edge("hue_value"));
    CHECK(nodeById(lowered.document, "out$canvas_g0_set_0").at("parameters").at("target") == "out$canvas_g0_frame");
    // The item's sources: its four colour numbers, then the filter's seven --
    // the hue (the second) from frame entry 0.
    const auto &table = nodeById(lowered.document, "out$canvas_g0_items").at("parameters").at("initial_f32");
    const auto &bake = nodeById(lowered.document, "out$canvas_g0_bake").at("parameters").at("initial_f32");
    const auto sources = static_cast<std::size_t>(table.at(2).get<double>());
    CHECK(bake.at(sources + 4 + 1) == 1.0);
    CHECK(bake.at(sources + 4 + 2) == 0.0);
    const auto cyan = centre(hued, {{{"hue_value", "value"}, 180.0}});
    CHECK_THAT(cyan[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(cyan[1], WithinAbs(1.0, 1e-6));
    CHECK_THAT(cyan[2], WithinAbs(1.0, 1e-6));
    const auto still = centre(hued, {{{"hue_value", "value"}, 0.0}});
    CHECK_THAT(still[0], WithinAbs(1.0, 1e-6));
    CHECK_THAT(still[1], WithinAbs(0.0, 1e-6));

    // Levels: the master's output ceiling (255 -> 51 halves... of red: 0.2).
    auto levels = filter("f", "filter.levels", "red");
    levels.at("inputs")["output_ceiling"] = edge("ceiling");
    levels.at("inputs")["channels.green.output_floor"] = edge("floor");
    const auto leveled = with(levels, {constant("ceiling", 51.0), constant("floor", 0.0)});
    validate(leveled);
    const auto dim = centre(leveled, {{{"ceiling", "value"}, 51.0}, {{"floor", "value"}, 255.0}});
    CHECK_THAT(dim[0], WithinAbs(std::pow((0.2 + 0.055) / 1.055, 2.4), 1e-6));
    CHECK_THAT(dim[1], WithinAbs(std::pow((0.2 + 0.055) / 1.055, 2.4), 1e-6));

    // Colour overlay: its alpha an edge drives.
    auto overlay = filter("f", "filter.color_overlay", "red", {{"color", {0, 0, 1, 0}}});
    overlay.at("inputs")["color.a"] = edge("mix");
    const auto overlaid = with(overlay, {constant("mix", 1.0)});
    validate(overlaid);
    const auto blue = centre(overlaid, {{{"mix", "value"}, 1.0}});
    CHECK_THAT(blue[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(blue[2], WithinAbs(1.0, 1e-6));

    // Gradient map: a stop's colour and position, by the authored stop.
    auto map = filter("f", "filter.gradient_map", "red",
                      {{"stops", Json::array({{{"position", 0.0}, {"color", {0, 0, 0}}},
                                              {{"position", 1.0}, {"color", {1, 1, 1}}}})}});
    map.at("inputs")["stops.1.color.g"] = edge("green");
    map.at("inputs")["stops.0.position"] = edge("start");
    const auto mapped = with(map, {constant("green", 0.0), constant("start", 0.0)});
    validate(mapped);
    // Red's luminosity is 0.3: a third of the way to (1, 0, 1).
    const auto purple = centre(mapped, {{{"green", "value"}, 0.0}, {{"start", "value"}, 0.0}});
    CHECK_THAT(purple[1], WithinAbs(0.0, 1e-6));
    CHECK_THAT(purple[0], WithinAbs(std::pow((0.3 + 0.055) / 1.055, 2.4), 1e-6));
    // With the first stop moved past it, the map is the first stop's black.
    const auto black = centre(mapped, {{{"green", "value"}, 0.0}, {{"start", "value"}, 0.5}});
    CHECK_THAT(black[0], WithinAbs(0.0, 1e-6));
    const auto lowered_map = Pelican::lowerCanvasNodes(mapped);
    std::set<std::string> sinks;
    for (const auto &node : lowered_map.document.at("nodes"))
        if (node.at("op") == "render.scalar") sinks.insert(node.at("inputs").at("value").at("node"));
    CHECK(sinks == std::set<std::string>{"green", "start"});

    // Brightness / contrast is baked into its curve: an edge is refused by name.
    auto contrast = filter("f", "filter.brightness_contrast", "red", {{"brightness", 10}});
    contrast.at("inputs")["contrast"] = edge("contrast_value");
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(with(contrast, {constant("contrast_value", 20.0)})); }) ==
          R"(canvas.filter_edge_unsupported@1 {"node":"f","port":"contrast"})");
}

// (A canvas.place's groups, fixed at the stack's size, take the same path:
// canvas_place_test checks theirs.)
TEST_CASE("CV-3a canvas.output size lowers to a fixed image with extent_scale 1", "[canvas][cv3a]") {
    const auto authored = document(Json::array({color("back", {0.2, 0.3, 0.4, 1.0}), stack("root", {"back"}, {64, 32})}),
                                   "root", {{"size", {128u, 64u}}});
    const auto lowered = Pelican::lowerCanvasNodes(authored);
    const auto &parameters = nodeById(lowered.document, "out$canvas_image").at("parameters");
    CHECK(parameters.at("width") == 128);
    CHECK(parameters.at("height") == 64);
    CHECK(parameters.at("extent_scale") == 1.0);
    // Without a size the image follows the frame's extent.
    const auto relative = Pelican::lowerCanvasNodes(
        document(Json::array({color("back", {0.2, 0.3, 0.4, 1.0}), stack("root", {"back"}, {64, 32})}), "root"));
    const auto &scaled = nodeById(relative.document, "out$canvas_image").at("parameters");
    CHECK_FALSE(scaled.contains("width"));
    CHECK(scaled.at("extent_scale") == 1.0);
}

TEST_CASE("CV-3a neighbourhood filters refuse by name", "[canvas][cv3a]") {
    // A backdrop blurred: an adjustment layer cannot read around itself.
    const auto backdrop = document(Json::array({color("back", {0.2, 0.3, 0.4, 1.0}),
                                                {{"id", "bd"}, {"op", "canvas.backdrop"}, {"outputs", Json::array({"layer"})}},
                                                filter("fx", "filter.blur", "bd", {{"radius", 3}}),
                                                stack("root", {"back", "fx"}, {16, 16})}),
                                   "root");
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(backdrop); }) ==
          R"(canvas.filter_backdrop_unsupported@1 {"node":"fx","op":"filter.blur"})");
    // An unknown filter stays unknown.
    const auto unknown = document(Json::array({image("card", {0, 0}, {6, 4}), filter("fx", "filter.motion_blur", "card"),
                                               stack("root", {"fx"}, {16, 16})}),
                                  "root");
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(unknown); }) ==
          R"(canvas.op_unsupported@1 {"node":"fx","op":"filter.motion_blur"})");
    // Out of range.
    const auto spread = document(Json::array({image("card", {0, 0}, {6, 4}),
                                              filter("fx", "filter.drop_shadow", "card", {{"spread", 1.5}}),
                                              stack("root", {"fx"}, {16, 16})}),
                                 "root");
    CHECK_THROWS_AS(Pelican::lowerCanvasNodes(spread), std::invalid_argument);
    // A blur of no radius is its layer: no pass.
    const auto none = Pelican::lowerCanvasNodes(document(
        Json::array({image("card", {0, 0}, {6, 4}), filter("fx", "filter.blur", "card", {{"radius", 0}}),
                     stack("root", {"fx"}, {16, 16})}),
        "root"));
    CHECK(rasters(none.document) == 1);
    CHECK(onlyOutput(none.ir).blurs.empty());
}
