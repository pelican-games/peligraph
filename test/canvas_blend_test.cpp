// CV-2b: blend modes, canvas.backdrop and the colour filters in the
// pelican.canvas lowering and its CPU reference, without a GPU.
#include "../src/project/canvas/canvasblend.hpp"
#include "../src/project/canvas/canvaslowering.hpp"
#include "../src/project/canvas/canvasreference.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>

namespace {
using Json = nlohmann::json;
using Pelican::CanvasBlend;
using Pelican::CanvasRgb;

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

bool hasNode(const Json &document, const std::string &id) {
    for (const auto &node : document.at("nodes"))
        if (node.at("id") == id) return true;
    return false;
}

Json color(const std::string &id, std::array<double, 4> rgba) {
    return {{"id", id}, {"op", "canvas.color"}, {"parameters", {{"color", {rgba[0], rgba[1], rgba[2], rgba[3]}}}},
            {"outputs", Json::array({"layer"})}};
}

Json image(const std::string &id, const std::string &asset, std::array<double, 2> size, Json extra = Json::object()) {
    Json parameters{{"asset", asset}, {"offset", {0, 0}}, {"size", {size[0], size[1]}}};
    parameters.update(extra);
    return {{"id", id}, {"op", "canvas.image"}, {"parameters", parameters}, {"outputs", Json::array({"layer"})}};
}

Json backdrop(const std::string &id) {
    return {{"id", id}, {"op", "canvas.backdrop"}, {"outputs", Json::array({"layer"})}};
}

Json filter(const std::string &id, const std::string &op, const std::string &input, Json parameters = Json::object()) {
    Json node{{"id", id}, {"op", op}, {"inputs", {{"layer", {{"node", input}, {"port", "layer"}}}}},
              {"outputs", Json::array({"layer"})}};
    if (!parameters.empty()) node["parameters"] = std::move(parameters);
    return node;
}

Json stack(const std::string &id, const Json &layers, Json parameters = Json::object()) {
    Json inputs = Json::object();
    for (const auto &layer : layers) {
        inputs[layer.at("input").get<std::string>()] = {{"node", layer.at("input")}, {"port", "layer"}};
        if (layer.contains("mask")) inputs[layer.at("mask").get<std::string>()] = {{"node", layer.at("mask")}, {"port", "layer"}};
    }
    parameters["layers"] = layers;
    return {{"id", id}, {"op", "canvas.stack"}, {"parameters", parameters}, {"inputs", inputs},
            {"outputs", Json::array({"layer"})}};
}

Json document(Json nodes, const std::string &top) {
    nodes.push_back({{"id", "out"}, {"op", "canvas.output"}, {"parameters", {{"size", {4u, 4u}}}},
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

double srgbToLinear(double c) { return c > 0.04045 ? std::pow((c + 0.055) / 1.055, 2.4) : c / 12.92; }

bool near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }

bool nearRgb(const CanvasRgb &a, const CanvasRgb &b, double tolerance = 1e-4) {
    return near(a[0], b[0], tolerance) && near(a[1], b[1], tolerance) && near(a[2], b[2], tolerance);
}

// Pixel (1, 1) of the CPU reference: RGBA, linear, premultiplied.
std::array<double, 4> centre(const Json &authored, const Pelican::CanvasReferenceImages &images = {}) {
    const auto ir = Pelican::buildCanvasIr(authored);
    REQUIRE(ir.outputs.size() == 1);
    const auto image = Pelican::renderCanvasReference(ir.outputs.front(), 4, 4, {}, images);
    const auto *p = &image.pixels[(1 * 4 + 1) * 4];
    return {p[0], p[1], p[2], p[3]};
}

const CanvasRgb backdrop_color{0.2, 0.5, 0.8};
const CanvasRgb source_color{0.6, 0.4, 0.3};
} // namespace

TEST_CASE("CV-2b blend modes compute W3C's B(Cb, Cs) and Photoshop's extra modes", "[canvas][cv2b]") {
    const auto blend = [](const char *name) {
        const auto mode = Pelican::canvasBlendFromName(name);
        REQUIRE(mode.has_value());
        return Pelican::canvasBlendColor(*mode, backdrop_color, source_color);
    };
    // Cb = (0.2, 0.5, 0.8), Cs = (0.6, 0.4, 0.3), each worked by hand from the definitions.
    CHECK(nearRgb(blend("normal"), {0.6, 0.4, 0.3}));
    CHECK(nearRgb(blend("multiply"), {0.12, 0.2, 0.24}));
    CHECK(nearRgb(blend("screen"), {0.68, 0.7, 0.86}));
    CHECK(nearRgb(blend("overlay"), {0.24, 0.4, 0.72}));
    CHECK(nearRgb(blend("darken"), {0.2, 0.4, 0.3}));
    CHECK(nearRgb(blend("lighten"), {0.6, 0.5, 0.8}));
    CHECK(nearRgb(blend("color_dodge"), {0.5, 0.5 / 0.6, 1.0}));
    CHECK(nearRgb(blend("color_burn"), {0.0, 0.0, 1.0 - 0.2 / 0.3}));
    CHECK(nearRgb(blend("hard_light"), {0.36, 0.4, 0.48}));
    CHECK(nearRgb(blend("soft_light"), {0.2496, 0.45, 0.736}));
    CHECK(nearRgb(blend("difference"), {0.4, 0.1, 0.5}));
    CHECK(nearRgb(blend("exclusion"), {0.56, 0.5, 0.62}));
    // Non-separable: Lum(Cb) = 0.443, Lum(Cs) = 0.449, Sat(Cb) = 0.6, Sat(Cs) = 0.3.
    CHECK(nearRgb(blend("hue"), {0.745, 0.345, 0.145}));
    CHECK(nearRgb(blend("saturation"), {0.3215, 0.4715, 0.6215}));
    CHECK(nearRgb(blend("color"), {0.594, 0.394, 0.294}));
    CHECK(nearRgb(blend("luminosity"), {0.206, 0.506, 0.806}));
    // Photoshop's modes W3C does not have.
    CHECK(nearRgb(blend("linear_dodge"), {0.8, 0.9, 1.0}));
    CHECK(nearRgb(blend("linear_burn"), {0.0, 0.0, 0.1}));
    CHECK(nearRgb(blend("subtract"), {0.0, 0.1, 0.5}));
    CHECK(nearRgb(blend("divide"), {0.2 / 0.6, 1.0, 1.0}));
    CHECK(nearRgb(blend("vivid_light"), {0.25, 0.375, 1.0 - 0.2 / 0.6}));
    CHECK(nearRgb(blend("linear_light"), {0.4, 0.3, 0.4}));
    CHECK(nearRgb(blend("pin_light"), {0.2, 0.5, 0.6}));
    CHECK(nearRgb(blend("hard_mix"), {0.0, 0.0, 1.0}));
    CHECK(nearRgb(blend("darker_color"), {0.2, 0.5, 0.8}));
    CHECK(nearRgb(blend("lighter_color"), {0.6, 0.4, 0.3}));
    // ClipColor: a colour whose luminosity is pushed past white comes back to white.
    CHECK(nearRgb(Pelican::canvasBlendColor(CanvasBlend::color, {1.0, 1.0, 1.0}, {1.0, 0.0, 0.0}), {1.0, 1.0, 1.0}));

    // The vocabulary, in the order an editor lists it; dissolve is not in it.
    const auto &names = Pelican::canvasBlendNames();
    REQUIRE(names.size() == 26);
    CHECK(names.front() == "normal");
    CHECK(names.back() == "lighter_color");
    CHECK_FALSE(Pelican::canvasBlendFromName("dissolve").has_value());
}

TEST_CASE("CV-2b a blend composites source-over with the backdrop's alpha, in the stack's blend space",
          "[canvas][cv2b]") {
    // Two half-transparent colours, the top one multiplied, in a linear stack.
    const auto authored = document(
        Json::array({color("under", {0.2, 0.5, 0.8, 0.5}), color("over", {0.6, 0.4, 0.3, 0.5}),
                     stack("root", Json::array({{{"input", "under"}}, {{"input", "over"}, {"blend", "multiply"}}}),
                           {{"size", {4, 4}}})}),
        "root");
    validate(authored);
    const auto pixel = centre(authored);
    const double ab = 0.5, as = 0.5;
    for (std::size_t c = 0; c < 3; ++c) {
        const auto cb = srgbToLinear(backdrop_color[c]), cs = srgbToLinear(source_color[c]);
        // as ((1 - ab) Cs + ab B) + (1 - as) ab Cb
        const auto expected = as * ((1.0 - ab) * cs + ab * cb * cs) + (1.0 - as) * ab * cb;
        CHECK(near(pixel[c], expected));
    }
    CHECK(near(pixel[3], 0.75));

    // In an sRGB stack B is taken of the sRGB values; the output decodes the result.
    auto srgb = authored;
    srgb.at("nodes").at(2).at("parameters")["blend_space"] = "srgb";
    const auto encoded = centre(srgb);
    for (std::size_t c = 0; c < 3; ++c) {
        const auto cb = backdrop_color[c], cs = source_color[c];
        const auto premultiplied = as * ((1.0 - ab) * cs + ab * cb * cs) + (1.0 - as) * ab * cb;
        CHECK(near(encoded[c], srgbToLinear(premultiplied / 0.75) * 0.75));
    }
}

TEST_CASE("CV-2b an adjustment layer mixes the filtered backdrop in by its opacity and mask", "[canvas][cv2b]") {
    const auto authored = document(
        Json::array({color("paper", {0.2, 0.5, 0.8, 1.0}), backdrop("bd"), filter("invert", "filter.invert", "bd"),
                     image("half", "project://half.png", {4, 4}, {{"channel", "luminance"}}),
                     stack("root",
                           Json::array({{{"input", "paper"}}, {{"input", "invert"}, {"opacity", 0.25}, {"mask", "half"}}}),
                           {{"size", {4, 4}}})}),
        "root");
    validate(authored);
    // The mask: white on the left half, black on the right.
    Pelican::CanvasReferenceImage half{4, 4, {}};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const std::uint8_t v = x < 2 ? 255 : 0;
            half.rgba.insert(half.rgba.end(), {v, v, v, 255});
        }
    const auto ir = Pelican::buildCanvasIr(authored);
    REQUIRE(ir.outputs.front().items.size() == 2);
    CHECK(ir.outputs.front().items[1].source == Pelican::CanvasItemSource::backdrop);
    REQUIRE(ir.outputs.front().items[1].filters.size() == 1);
    const auto image = Pelican::renderCanvasReference(ir.outputs.front(), 4, 4, {}, {{"project://half.png", half}});
    const auto *left = &image.pixels[(1 * 4 + 1) * 4];
    const auto *right = &image.pixels[(1 * 4 + 2) * 4];
    for (std::size_t c = 0; c < 3; ++c) {
        const auto cb = srgbToLinear(backdrop_color[c]);
        // Cb + m (F(Cb) - Cb), alpha kept.
        CHECK(near(left[c], cb + 0.25 * ((1.0 - cb) - cb)));
        CHECK(near(right[c], cb));
    }
    CHECK(near(left[3], 1.0));
    CHECK(near(right[3], 1.0));

    // Lowered: the backdrop item draws atop, in a draw of its own that reads a
    // copy of the image the first draw left.
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    CHECK(hasNode(lowering.document, "out$canvas_back0"));
    CHECK(hasNode(lowering.document, "out$canvas_back0_image"));
    const auto &copy = nodeById(lowering.document, "out$canvas_back0");
    CHECK(copy.at("inputs").at("t0") == Json{{"node", "out$canvas_draw0"}, {"port", "image"}});
    const auto &draw = nodeById(lowering.document, "out");
    CHECK(draw.at("parameters").at("color_load_op") == "load");
    CHECK(draw.at("inputs").at("image") == Json{{"node", "out$canvas_draw0"}, {"port", "image"}});
    bool reads_copy = false;
    for (const auto &[port, edge] : draw.at("inputs").items())
        reads_copy = reads_copy || edge == Json{{"node", "out$canvas_back0"}, {"port", "image"}};
    CHECK(reads_copy);
    CHECK(draw.at("parameters").at("raster_state").at("color_attachments").at(0).at("blend").at("color").at("src") ==
          "dst_alpha");
    // The backdrop's kind (9) with style 1 (normal, filter.invert).
    const auto &table = nodeById(lowering.document, "out$canvas_items").at("parameters").at("initial_f32");
    CHECK(table.at(0).get<double>() == 9.0 + 256.0);
    std::string fragment;
    for (const auto &source : lowering.generated_sources)
        if (source.stage == "frag" && source.text.find("canvasStyle") != std::string::npos) fragment = source.text;
    // CV-3a: a filter reads its numbers as the item's parameters (canvasParam).
    CHECK(fragment.find("canvasFilter0(c, it, r)") != std::string::npos);
    CHECK(fragment.find("CANVAS_BACKDROP_SLOT") != std::string::npos);
    bool copies = false;
    for (const auto &diagnostic : lowering.diagnostics)
        if (diagnostic.at("code") == "canvas.backdrop_copies") {
            copies = true;
            CHECK(diagnostic == Json{{"code", "canvas.backdrop_copies"}, {"output", "out"}, {"count", 1}});
        }
    CHECK(copies);
    validate(lowering.document);
}

TEST_CASE("CV-2b a pass-through group's blend reads the parent's backdrop, an isolated group's its own",
          "[canvas][cv2b]") {
    const auto with = [](bool pass_through) {
        Json inner_layer{{"input", "group"}};
        if (pass_through) inner_layer["pass_through"] = true;
        return document(Json::array({color("paper", {0.2, 0.5, 0.8, 1.0}), color("tint", {0.6, 0.4, 0.3, 1.0}),
                                     stack("group", Json::array({{{"input", "tint"}, {"blend", "multiply"}}})),
                                     stack("root", Json::array({{{"input", "paper"}}, inner_layer}), {{"size", {4, 4}}})}),
                        "root");
    };
    const auto through = centre(with(true));
    const auto isolated = centre(with(false));
    for (std::size_t c = 0; c < 3; ++c) {
        CHECK(near(through[c], srgbToLinear(backdrop_color[c]) * srgbToLinear(source_color[c])));
        // Alone in its group, the multiply has nothing below it: it draws as normal.
        CHECK(near(isolated[c], srgbToLinear(source_color[c])));
    }
    // The isolated group is composited on its own (not flattened into the root).
    const auto ir = Pelican::buildCanvasIr(with(false));
    CHECK(ir.outputs.front().groups.size() == 1);
    const auto lowering = Pelican::lowerCanvasNodes(with(false));
    for (const auto &diagnostic : lowering.diagnostics) CHECK(diagnostic.at("code") != "canvas.backdrop_copies");
}

TEST_CASE("CV-2b each backdrop read is one copy, counted in the diagnostics", "[canvas][cv2b]") {
    const auto authored = document(
        Json::array({color("a", {0.2, 0.5, 0.8, 1.0}), color("b", {0.6, 0.4, 0.3, 0.5}), color("c", {0.9, 0.9, 0.1, 0.5}),
                     color("d", {0.1, 0.1, 0.1, 0.5}),
                     stack("root",
                           Json::array({{{"input", "a"}, {"blend", "screen"}}, {{"input", "b"}, {"blend", "multiply"}},
                                        {{"input", "c"}}, {{"input", "d"}, {"blend", "difference"}}}),
                           {{"size", {4, 4}}})}),
        "root");
    const auto lowering = Pelican::lowerCanvasNodes(authored);
    // The bottom layer's screen has nothing to read; the multiply and the difference do.
    Json copies;
    for (const auto &diagnostic : lowering.diagnostics)
        if (diagnostic.at("code") == "canvas.backdrop_copies") copies = diagnostic;
    CHECK(copies == Json{{"code", "canvas.backdrop_copies"}, {"output", "out"}, {"count", 2}});
    CHECK(hasNode(lowering.document, "out$canvas_back0"));
    CHECK(hasNode(lowering.document, "out$canvas_back1"));
    CHECK_FALSE(hasNode(lowering.document, "out$canvas_back2"));
    // Draws: [a], [b, c], [d]; c (normal) shares the multiply's draw.
    CHECK(hasNode(lowering.document, "out$canvas_draw0"));
    CHECK(hasNode(lowering.document, "out$canvas_draw1"));
    CHECK_FALSE(hasNode(lowering.document, "out$canvas_draw2"));
    CHECK(nodeById(lowering.document, "out$canvas_back1").at("inputs").at("t0") ==
          Json{{"node", "out$canvas_draw1"}, {"port", "image"}});
    // The trace names the blends.
    std::set<std::string> blends;
    for (const auto &item : lowering.items)
        if (item.contains("blend")) blends.insert(item.at("blend").get<std::string>());
    CHECK(blends == std::set<std::string>{"multiply", "difference"});
    validate(lowering.document);
}

TEST_CASE("CV-2b colour filters on known colours", "[canvas][cv2b]") {
    const auto apply = [](const Json &node, const CanvasRgb &c, bool srgb = true) {
        return Pelican::applyCanvasFilter(Pelican::parseCanvasFilter(node), c, srgb);
    };
    const auto make = [](const std::string &op, Json parameters = Json::object()) {
        return filter("f", op, "x", std::move(parameters));
    };
    CHECK(nearRgb(apply(make("filter.invert"), backdrop_color), {0.8, 0.5, 0.2}));
    // Levels: 10..240 in, gamma 1.5, 5..250 out, on the master.
    const auto level = [](double x) {
        const auto o = std::pow(std::clamp((x - 10.0 / 255.0) / (230.0 / 255.0), 0.0, 1.0), 1.0 / 1.5);
        return o * (245.0 / 255.0) + 5.0 / 255.0;
    };
    const Json levels{{"input_floor", 10}, {"input_ceiling", 240}, {"gamma", 1.5}, {"output_floor", 5},
                      {"output_ceiling", 250}};
    CHECK(nearRgb(apply(make("filter.levels", levels), backdrop_color),
                  {level(0.2), level(0.5), level(0.8)}));
    // A channel's record goes first, then the master.
    const Json red_only{{"channels", {{"red", {{"input_floor", 0}, {"input_ceiling", 200}}}}}};
    CHECK(nearRgb(apply(make("filter.levels", red_only), backdrop_color), {0.2 * 255.0 / 200.0, 0.5, 0.8}));
    // Hue / saturation, in Photoshop's units.
    CHECK(nearRgb(apply(make("filter.hue_saturation", {{"hue", 180}}), {1.0, 0.0, 0.0}), {0.0, 1.0, 1.0}));
    CHECK(nearRgb(apply(make("filter.hue_saturation", {{"saturation", -100}}), backdrop_color), {0.5, 0.5, 0.5}));
    CHECK(nearRgb(apply(make("filter.hue_saturation", {{"lightness", 50}}), backdrop_color), {0.6, 0.75, 0.9}));
    CHECK(nearRgb(apply(make("filter.hue_saturation", {{"hue", 30}, {"colorize", {{"hue", 120}, {"saturation", 100}}}}),
                        backdrop_color),
                  {0.0, 1.0, 0.0}));
    // Brightness / contrast at zero is the identity; a brightness lifts the middle.
    const auto still = apply(make("filter.brightness_contrast", {{"brightness", 0}, {"contrast", 0}}), backdrop_color);
    CHECK(nearRgb(still, backdrop_color, 1e-6));
    const auto lifted = apply(make("filter.brightness_contrast", {{"brightness", 50}, {"contrast", 0}}), {0.5, 0.5, 0.5});
    CHECK(lifted[0] > 0.55);
    // Gradient map: black to white by the luminosity (0.443).
    const Json stops{{"stops", Json::array({{{"position", 0.0}, {"color", {0, 0, 0}}},
                                            {{"position", 1.0}, {"color", {1, 1, 1}}}})}};
    CHECK(nearRgb(apply(make("filter.gradient_map", stops), backdrop_color), {0.443, 0.443, 0.443}));
    auto reversed = stops;
    reversed["reversed"] = true;
    CHECK(nearRgb(apply(make("filter.gradient_map", reversed), backdrop_color), {0.557, 0.557, 0.557}));
    // Colour overlay replaces the colour; its alpha mixes it in.
    CHECK(nearRgb(apply(make("filter.color_overlay", {{"color", {1, 0, 0, 1}}}), backdrop_color), {1.0, 0.0, 0.0}));
    CHECK(nearRgb(apply(make("filter.color_overlay", {{"color", {1, 0, 0, 0.5}}}), backdrop_color), {0.6, 0.25, 0.4}));
}

TEST_CASE("CV-2b a filter on an image, a shape or a group works on its colour", "[canvas][cv2b]") {
    // Two overlapping colours in a stack, inverted as one: the group is composited first.
    const auto authored = document(
        Json::array({color("a", {0.2, 0.5, 0.8, 1.0}), color("b", {0.6, 0.4, 0.3, 0.5}),
                     stack("pair", Json::array({{{"input", "a"}}, {{"input", "b"}}}), {{"blend_space", "srgb"}}),
                     filter("invert", "filter.invert", "pair"),
                     stack("root", Json::array({{{"input", "invert"}}}), {{"size", {4, 4}}, {"blend_space", "srgb"}})}),
        "root");
    validate(authored);
    const auto ir = Pelican::buildCanvasIr(authored);
    REQUIRE(ir.outputs.front().items.size() == 1);
    CHECK(ir.outputs.front().items.front().source == Pelican::CanvasItemSource::group);
    const auto pixel = centre(authored);
    for (std::size_t c = 0; c < 3; ++c) {
        const auto mixed = 0.5 * source_color[c] + 0.5 * backdrop_color[c];
        CHECK(near(pixel[c], srgbToLinear(1.0 - mixed)));
    }
    // One colour, filtered alone: no group.
    const auto single = document(
        Json::array({color("a", {0.2, 0.5, 0.8, 1.0}), filter("over", "filter.color_overlay", "a", {{"color", {0, 1, 0, 1}}}),
                     stack("root", Json::array({{{"input", "over"}}}), {{"size", {4, 4}}})}),
        "root");
    const auto single_ir = Pelican::buildCanvasIr(single);
    REQUIRE(single_ir.outputs.front().items.size() == 1);
    CHECK(single_ir.outputs.front().items.front().source == Pelican::CanvasItemSource::color);
    const auto green = centre(single);
    CHECK(near(green[0], 0.0));
    CHECK(near(green[1], 1.0));
    CHECK(near(green[2], 0.0));
    // The lowering passes no copy for a filter that reads no backdrop.
    const auto lowering = Pelican::lowerCanvasNodes(single);
    CHECK_FALSE(hasNode(lowering.document, "out$canvas_back0"));
    validate(lowering.document);
}

TEST_CASE("CV-2b refuses by name", "[canvas][cv2b]") {
    const auto base = [](Json filter_node) {
        return document(Json::array({color("a", {0.2, 0.5, 0.8, 1.0}), backdrop("bd"), filter_node,
                                     stack("root", Json::array({{{"input", "a"}}, {{"input", "f"}, {"name", "Adjust"}}}),
                                           {{"size", {4, 4}}})}),
                        "root");
    };
    CHECK(refusal([&] {
              (void)Pelican::lowerCanvasNodes(base(filter("f", "filter.hue_saturation", "bd",
                                                          {{"hue", 10}, {"ranges", Json::array({Json::object()})}})));
          }) == R"(canvas.filter_parameter_unsupported@1 {"node":"f","parameter":"ranges"})");
    CHECK(refusal([&] {
              (void)Pelican::lowerCanvasNodes(
                  base(filter("f", "filter.brightness_contrast", "bd", {{"brightness", 10}, {"legacy", true}})));
          }) == R"(canvas.filter_parameter_unsupported@1 {"node":"f","parameter":"legacy"})");
    CHECK(refusal([&] {
              (void)Pelican::lowerCanvasNodes(base(filter(
                  "f", "filter.gradient_map", "bd",
                  {{"stops", Json::array({{{"position", 0}, {"color", {0, 0, 0}}, {"midpoint", 0.3}}})}})));
          }) == R"(canvas.filter_parameter_unsupported@1 {"node":"f","parameter":"midpoint"})");
    CHECK(refusal([&] {
              auto node = filter("f", "filter.brightness_contrast", "bd", {{"brightness", 10}});
              node.at("inputs")["brightness"] = {{"node", "a"}, {"port", "layer"}};
              (void)Pelican::lowerCanvasNodes(base(node));
          }) == R"(canvas.filter_edge_unsupported@1 {"node":"f","port":"brightness"})");
    CHECK(refusal([&] {
              auto authored = base(filter("f", "filter.invert", "bd"));
              authored.at("nodes").at(3).at("parameters").at("layers").at(1)["blend"] = "dissolve";
              (void)Pelican::lowerCanvasNodes(authored);
          }) == R"(canvas.blend_unsupported@1 {"blend":"dissolve","layer":"Adjust","node":"root"})");
    CHECK(refusal([&] {
              (void)Pelican::lowerCanvasNodes(base(filter("f", "filter.gaussian_blur", "bd")));
          }) == R"(canvas.op_unsupported@1 {"node":"f","op":"filter.gaussian_blur"})");
}
