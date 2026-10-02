// CV-4: shape.text without a GPU -- the in-house distance fields (DF-1's
// checks on small synthetic outlines), the font reader on a TrueType font
// built here byte by byte, the layout, and the lowering's atlas, items and
// named failures.
#include "../src/project/authoringlowering.hpp"
#include "../src/project/canvas/canvaslowering.hpp"
#include "../src/project/canvas/canvasreference.hpp"
#include "../src/project/canvas/canvastext.hpp"
#include "../src/project/canvas/distancefield.hpp"
#include "../src/project/canvas/fontfile.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphmodule.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Json = nlohmann::json;
using Catch::Matchers::WithinAbs;

// --- a TrueType font, built in the test ---------------------------------------

struct MiniPoint {
    int x = 0;
    int y = 0;
    bool on = true;
};

struct MiniGlyph {
    char32_t codepoint = 0;
    int advance = 0;
    std::vector<std::vector<MiniPoint>> contours; // y up, TrueType winding
};

struct Bytes {
    std::vector<std::uint8_t> data;
    void u16(int value) {
        data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }
    void u32(std::uint32_t value) {
        u16(static_cast<int>(value >> 16));
        u16(static_cast<int>(value & 0xFFFF));
    }
    void put16(std::size_t at, int value) {
        data[at] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        data[at + 1] = static_cast<std::uint8_t>(value & 0xFF);
    }
};

// head / hhea / maxp / hmtx / cmap (format 4) / loca / glyf, and a 'kern'
// table (format 0) for the pairs given. Glyph 0 is an empty .notdef; glyph
// k + 1 is glyphs[k].
std::vector<std::byte> buildTrueType(const std::vector<MiniGlyph> &glyphs,
                                     const std::vector<std::array<int, 3>> &kerning) {
    const int count = static_cast<int>(glyphs.size()) + 1;
    std::map<std::string, Bytes> tables;
    // glyf and loca.
    Bytes glyf, loca;
    std::array<int, 4> font_box{0, 0, 0, 0};
    std::vector<int> left_bearings(count, 0);
    loca.u32(0); // .notdef is empty
    loca.u32(0);
    for (int index = 1; index < count; ++index) {
        const auto &glyph = glyphs[index - 1];
        if (!glyph.contours.empty()) {
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
            for (const auto &contour : glyph.contours)
                for (const auto &point : contour) {
                    x0 = std::min(x0, point.x), y0 = std::min(y0, point.y);
                    x1 = std::max(x1, point.x), y1 = std::max(y1, point.y);
                }
            left_bearings[index] = x0;
            font_box = {std::min(font_box[0], x0), std::min(font_box[1], y0), std::max(font_box[2], x1),
                        std::max(font_box[3], y1)};
            glyf.u16(static_cast<int>(glyph.contours.size()));
            glyf.u16(x0), glyf.u16(y0), glyf.u16(x1), glyf.u16(y1);
            int end = -1;
            for (const auto &contour : glyph.contours) glyf.u16(end += static_cast<int>(contour.size()));
            glyf.u16(0); // no instructions
            for (const auto &contour : glyph.contours)
                for (const auto &point : contour) glyf.data.push_back(point.on ? 1 : 0);
            // Coordinates as 16-bit deltas (flags 0x02 / 0x10 and 0x04 / 0x20 clear).
            int x = 0;
            for (const auto &contour : glyph.contours)
                for (const auto &point : contour) glyf.u16(point.x - x), x = point.x;
            int y = 0;
            for (const auto &contour : glyph.contours)
                for (const auto &point : contour) glyf.u16(point.y - y), y = point.y;
            while (glyf.data.size() % 4 != 0) glyf.data.push_back(0);
        }
        loca.u32(static_cast<std::uint32_t>(glyf.data.size()));
    }
    tables["glyf"] = glyf;
    tables["loca"] = loca;
    Bytes head;
    head.u32(0x00010000), head.u32(0x00010000), head.u32(0), head.u32(0x5F0F3CF5);
    head.u16(0), head.u16(1000); // flags, unitsPerEm
    for (int k = 0; k < 4; ++k) head.u32(0);  // created, modified
    for (const auto value : font_box) head.u16(value);
    head.u16(0), head.u16(8), head.u16(2), head.u16(1), head.u16(0); // macStyle .. indexToLocFormat (long), glyphDataFormat
    tables["head"] = head;
    Bytes hhea;
    hhea.u32(0x00010000), hhea.u16(800), hhea.u16(-200), hhea.u16(0), hhea.u16(1000);
    for (int k = 0; k < 11; ++k) hhea.u16(0); // minLeftSideBearing .. metricDataFormat
    hhea.u16(count);
    tables["hhea"] = hhea;
    Bytes maxp;
    maxp.u32(0x00005000), maxp.u16(count);
    tables["maxp"] = maxp;
    Bytes hmtx;
    hmtx.u16(500), hmtx.u16(0);
    for (int index = 1; index < count; ++index) hmtx.u16(glyphs[index - 1].advance), hmtx.u16(left_bearings[index]);
    tables["hmtx"] = hmtx;
    // cmap: one segment per codepoint (idDelta), then the closing 0xFFFF.
    std::vector<std::pair<int, int>> mapped;
    for (int index = 1; index < count; ++index) mapped.emplace_back(static_cast<int>(glyphs[index - 1].codepoint), index);
    std::sort(mapped.begin(), mapped.end());
    const int segments = static_cast<int>(mapped.size()) + 1;
    int search = 1, selector = 0;
    while (search * 2 <= segments) search *= 2, ++selector;
    Bytes cmap;
    cmap.u16(0), cmap.u16(1), cmap.u16(3), cmap.u16(1), cmap.u32(12);
    cmap.u16(4), cmap.u16(16 + 8 * segments), cmap.u16(0), cmap.u16(2 * segments), cmap.u16(2 * search),
        cmap.u16(selector), cmap.u16(2 * segments - 2 * search);
    for (const auto &[codepoint, glyph] : mapped) cmap.u16(codepoint);
    cmap.u16(0xFFFF), cmap.u16(0);
    for (const auto &[codepoint, glyph] : mapped) cmap.u16(codepoint);
    cmap.u16(0xFFFF);
    for (const auto &[codepoint, glyph] : mapped) cmap.u16((glyph - codepoint) & 0xFFFF);
    cmap.u16(1);
    for (int k = 0; k < segments; ++k) cmap.u16(0);
    tables["cmap"] = cmap;
    if (!kerning.empty()) {
        auto pairs = kerning;
        std::sort(pairs.begin(), pairs.end());
        Bytes kern;
        kern.u16(0), kern.u16(1);
        kern.u16(0), kern.u16(14 + 6 * static_cast<int>(pairs.size())), kern.u16(1);
        kern.u16(static_cast<int>(pairs.size())), kern.u16(6), kern.u16(0), kern.u16(0);
        for (const auto &[left, right, amount] : pairs) kern.u16(left), kern.u16(right), kern.u16(amount);
        tables["kern"] = kern;
    }
    // The table directory, then the tables, each 4-byte aligned.
    Bytes file;
    const int table_count = static_cast<int>(tables.size());
    file.u32(0x00010000), file.u16(table_count), file.u16(0), file.u16(0), file.u16(0);
    std::size_t offset = 12 + 16 * tables.size();
    for (const auto &[tag, table] : tables) {
        for (const char c : tag) file.data.push_back(static_cast<std::uint8_t>(c));
        file.u32(0), file.u32(static_cast<std::uint32_t>(offset)), file.u32(static_cast<std::uint32_t>(table.data.size()));
        offset += (table.data.size() + 3) / 4 * 4;
    }
    for (const auto &[tag, table] : tables) {
        file.data.insert(file.data.end(), table.data.begin(), table.data.end());
        while (file.data.size() % 4 != 0) file.data.push_back(0);
    }
    std::vector<std::byte> bytes(file.data.size());
    std::transform(file.data.begin(), file.data.end(), bytes.begin(), [](std::uint8_t b) { return std::byte{b}; });
    return bytes;
}

std::vector<MiniPoint> rectContour(int x0, int y0, int x1, int y1) {
    // Clockwise with y up: up the left side, across the top, down the right.
    return {{x0, y0}, {x0, y1}, {x1, y1}, {x1, y0}};
}

// A, B, O, space, X, U+6C38 (as a box) with kerning A B -120 (font units, 1000 per em).
const std::vector<std::byte> &testFont() {
    static const auto bytes = [] {
        std::vector<MiniGlyph> glyphs;
        glyphs.push_back({U'A', 1000, {{{50, 0}, {500, 700}, {950, 0}}}});
        glyphs.push_back({U'B', 700, {rectContour(100, 0, 600, 700)}});
        glyphs.push_back({U'O', 800,
                          {{{100, 350}, {100, 650, false}, {400, 650}, {700, 650, false}, {700, 350}, {700, 50, false},
                            {400, 50}, {100, 50, false}},
                           {{250, 350}, {250, 200, false}, {400, 200}, {550, 200, false}, {550, 350}, {550, 500, false},
                            {400, 500}, {250, 500, false}}}});
        glyphs.push_back({U' ', 300, {}});
        glyphs.push_back({U'X', 800, {rectContour(100, 250, 700, 450), rectContour(300, 0, 500, 700)}});
        glyphs.push_back({U'永', 1000, {rectContour(100, -50, 900, 750)}});
        return buildTrueType(glyphs, {{1, 2, -120}});
    }();
    return bytes;
}

std::span<const std::uint8_t> fontSpan(const std::vector<std::byte> &bytes) {
    return {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()};
}

// The lowering's reader over a few in-memory files.
Pelican::CanvasAssetReader memoryReader(std::map<std::string, std::vector<std::byte>> files) {
    auto shared = std::make_shared<std::map<std::string, std::shared_ptr<const std::vector<std::byte>>>>();
    for (auto &[name, bytes] : files)
        (*shared)[name] = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    return [shared](std::string_view reference) -> std::shared_ptr<const std::vector<std::byte>> {
        const auto found = shared->find(std::string{reference});
        return found == shared->end() ? nullptr : found->second;
    };
}

const Pelican::CanvasAssetReader &testReader() {
    static const auto reader = memoryReader({{"project://fonts/mini.ttf", testFont()},
                                             {"project://fonts/broken.ttf", std::vector<std::byte>(64, std::byte{7})}});
    return reader;
}

std::string refusal(const std::function<void()> &call) {
    try {
        call();
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code() + " " + error.detail().dump();
    }
    return "accepted";
}

// --- distance fields on synthetic outlines --------------------------------

Pelican::Outline polygon(std::initializer_list<std::vector<Pelican::Point2>> contours) {
    Pelican::Outline outline;
    for (const auto &points : contours) {
        std::vector<Pelican::OutlineSegment> contour;
        for (std::size_t k = 0; k < points.size(); ++k)
            contour.push_back({Pelican::SegmentKind::line, {points[k], points[(k + 1) % points.size()]}});
        outline.contours.push_back(std::move(contour));
    }
    return outline;
}

// A ring of four quadratics around (cx, cy), the control points on the square.
std::vector<Pelican::OutlineSegment> quadraticRing(double cx, double cy, double r, bool reverse) {
    const Pelican::Point2 on[4] = {{cx - r, cy}, {cx, cy - r}, {cx + r, cy}, {cx, cy + r}};
    const Pelican::Point2 off[4] = {{cx - r, cy - r}, {cx + r, cy - r}, {cx + r, cy + r}, {cx - r, cy + r}};
    std::vector<Pelican::OutlineSegment> contour;
    for (int k = 0; k < 4; ++k) contour.push_back({Pelican::SegmentKind::quadratic, {on[k], off[k], on[(k + 1) % 4]}});
    if (reverse) {
        std::vector<Pelican::OutlineSegment> reversed;
        for (auto at = contour.rbegin(); at != contour.rend(); ++at)
            reversed.push_back({Pelican::SegmentKind::quadratic, {at->points[2], at->points[1], at->points[0]}});
        return reversed;
    }
    return contour;
}

struct Quality {
    double mse = 0.0;
    double wrong = 0.0; // fraction of pixels off by more than half
};

// An MTSDF of `outline` (cell coordinates) upscaled k times and held against
// the outline's exact coverage, with `reconstruct` turning a bilinear sample
// into a distance (texels).
Quality measure(const Pelican::Outline &outline, int cell, int k, const std::function<float(const float *)> &reconstruct,
                bool quantized) {
    auto field = Pelican::multiChannelDistanceField(outline, cell, cell, Pelican::Affine2{}, 4.0f);
    if (quantized)
        for (auto &value : field) value = (std::round((value / 8.0f + 0.5f) * 255.0f) / 255.0f - 0.5f) * 8.0f;
    const int size = cell * k;
    Pelican::Affine2 upscale;
    upscale.xx = upscale.yy = k;
    const auto reference =
        Pelican::outlineCoverage(Pelican::transformOutline(outline, upscale, 1e-3), size, size, 16);
    Quality quality;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double u = (x + 0.5) / k - 0.5, v = (y + 0.5) / k - 0.5;
            const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
            const auto tx = static_cast<float>(u - x0), ty = static_cast<float>(v - y0);
            const auto texel = [&](int tx_, int ty_) {
                tx_ = std::clamp(tx_, 0, cell - 1), ty_ = std::clamp(ty_, 0, cell - 1);
                return field.data() + (static_cast<std::size_t>(ty_) * cell + tx_) * 4;
            };
            float rgba[4];
            for (int c = 0; c < 4; ++c) {
                const auto top = texel(x0, y0)[c] + (texel(x0 + 1, y0)[c] - texel(x0, y0)[c]) * tx;
                const auto bottom = texel(x0, y0 + 1)[c] + (texel(x0 + 1, y0 + 1)[c] - texel(x0, y0 + 1)[c]) * tx;
                rgba[c] = top + (bottom - top) * ty;
            }
            const auto coverage = std::clamp(0.5 - reconstruct(rgba) * k, 0.0, 1.0);
            const auto error = coverage - reference[static_cast<std::size_t>(y) * size + x];
            quality.mse += error * error;
            quality.wrong += std::abs(error) > 0.5 ? 1.0 : 0.0;
        }
    quality.mse /= double(size) * size;
    quality.wrong /= double(size) * size;
    return quality;
}

// --- documents ------------------------------------------------------------

Json textNode(const std::string &id, Json parameters) {
    return {{"id", id}, {"op", "shape.text"}, {"parameters", std::move(parameters)}, {"outputs", Json::array({"geometry"})}};
}

Json paintNode(const std::string &id, const std::string &op, const std::string &geometry, Json parameters) {
    return {{"id", id}, {"op", op}, {"parameters", std::move(parameters)},
            {"inputs", {{"geometry", {{"node", geometry}, {"port", "geometry"}}}}}, {"outputs", Json::array({"layer"})}};
}

// A canvas of `size` with the layers given (paint ids), bottom to top.
Json canvasDocument(Json nodes, const std::vector<std::string> &layers, std::array<double, 2> size = {256, 128}) {
    Json entries = Json::array(), inputs = Json::object();
    for (const auto &layer : layers) {
        entries.push_back({{"input", layer}});
        inputs[layer] = {{"node", layer}, {"port", "layer"}};
    }
    nodes.push_back({{"id", "root"}, {"op", "canvas.stack"},
                     {"parameters", {{"size", {size[0], size[1]}}, {"layers", entries}}}, {"inputs", inputs},
                     {"outputs", Json::array({"layer"})}});
    nodes.push_back({{"id", "out"}, {"op", "canvas.output"}, {"parameters", Json::object()},
                     {"inputs", {{"layer", {{"node", "root"}, {"port", "layer"}}}}}, {"outputs", Json::array({"image"})}});
    return {{"schema", "pelican.graph"}, {"version", 1}, {"nodes", std::move(nodes)}, {"outputs", Json::object()}};
}

Json textDocument(Json text_parameters, Json stroke = nullptr) {
    Json nodes = Json::array({textNode("title", std::move(text_parameters)),
                              paintNode("title_fill", "shape.fill", "title", {{"color", {1, 1, 1, 1}}})});
    std::vector<std::string> layers{"title_fill"};
    if (!stroke.is_null()) {
        nodes.push_back(paintNode("title_stroke", "shape.stroke", "title", std::move(stroke)));
        layers.push_back("title_stroke");
    }
    return canvasDocument(std::move(nodes), layers);
}

Json defaultText(const std::string &text = "AB") {
    return {{"font", "project://fonts/mini.ttf"}, {"text", text}, {"size", 64}, {"origin", {16, 96}}};
}

} // namespace

TEST_CASE("CV-4 a coverage image becomes a signed distance field within DF-1's bounds", "[canvas][cv4]") {
    // A circle of radius 30 at a subpixel centre, coverage 8 x 8 supersampled.
    const int size = 96;
    const auto exact = [](double x, double y) { return std::hypot(x - 48.3, y - 47.6) - 30.0; };
    std::vector<float> coverage(static_cast<std::size_t>(size) * size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 8; ++sy)
                for (int sx = 0; sx < 8; ++sx) inside += exact(x + (sx + 0.5) / 8, y + (sy + 0.5) / 8) < 0.0;
            coverage[static_cast<std::size_t>(y) * size + x] = inside / 64.0f;
        }
    const auto error = [&](Pelican::CoverageSeed seed) {
        const auto field = Pelican::signedDistanceFromCoverage(coverage, size, size, 16.0f, seed);
        double sum = 0.0, worst = 0.0;
        int count = 0;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                const auto truth = exact(x + 0.5, y + 0.5);
                if (std::abs(truth) >= 8.0) continue;
                const auto e = std::abs(field[static_cast<std::size_t>(y) * size + x] - truth);
                sum += e, worst = std::max(worst, e), ++count;
            }
        return std::pair{sum / count, worst};
    };
    const auto [mean, worst] = error(Pelican::CoverageSeed::edge_point);
    const auto [linear_mean, linear_worst] = error(Pelican::CoverageSeed::linear);
    (void)linear_worst;
    INFO("edge_point mean " << mean << " max " << worst << ", linear mean " << linear_mean);
    // DF-1 measured 0.016 / 0.30 on a radius-40 circle at 16 x 16.
    CHECK(mean < 0.05);
    CHECK(worst < 0.5);
    CHECK(mean < linear_mean);
    CHECK(Pelican::signedDistanceFromCoverage(coverage, size, size, 16.0f) ==
          Pelican::signedDistanceFromCoverage(coverage, size, size, 16.0f));
    CHECK_THROWS_AS(Pelican::signedDistanceFromCoverage(coverage, size, size - 1, 16.0f), std::invalid_argument);
}

TEST_CASE("CV-4 an outline becomes an MTSDF that keeps corners, ignores overlaps and repeats itself",
          "[canvas][cv4]") {
    const auto rect = polygon({{{6, 8}, {26, 8}, {26, 24}, {6, 24}}});
    const auto triangle = polygon({{{5, 27}, {16, 5}, {27, 27}}});
    Pelican::Outline ring;
    ring.contours = {quadraticRing(16, 16, 11, false), quadraticRing(16, 16, 5, true)};
    const auto overlap = polygon({{{6, 12}, {26, 12}, {26, 20}, {6, 20}}, {{12, 6}, {20, 6}, {20, 26}, {12, 26}}});
    const auto clamped = [](const float *rgba) { return Pelican::reconstructMtsdfNear(rgba, 0.5f, 1.0f); };
    const auto true_only = [](const float *rgba) { return rgba[3]; };
    for (const auto &[name, outline] : std::vector<std::pair<std::string, Pelican::Outline>>{
             {"rect", rect}, {"triangle", triangle}, {"ring", ring}, {"overlap", overlap}}) {
        const auto mtsdf = measure(outline, 32, 4, clamped, true);
        INFO(name << ": MTSDF (8 bit) mse " << mtsdf.mse << " wrong " << mtsdf.wrong);
        // DF-1 on Arial at 32 x 32, x4: mse 1.9e-5, no wrong pixel.
        CHECK(mtsdf.mse < 2e-4);
        CHECK(mtsdf.wrong < 1e-3);
        if (name == "triangle" || name == "rect") {
            // The corners: the true distance alone rounds them off.
            const auto sdf = measure(outline, 32, 4, true_only, false);
            INFO(name << ": A only mse " << sdf.mse);
            CHECK(mtsdf.mse < sdf.mse);
        }
    }
    // Overlapping contours: the pieces inside the union are dropped, so the
    // true distance at the centre is to the union's corner, not the inner edges.
    Pelican::MtsdfStats stats;
    const auto field = Pelican::multiChannelDistanceField(overlap, 32, 32, Pelican::Affine2{}, 4.0f, {}, &stats);
    CHECK(stats.removed_pieces > 0);
    Pelican::MtsdfOptions kept;
    kept.remove_overlaps = false;
    const auto unremoved = Pelican::multiChannelDistanceField(overlap, 32, 32, Pelican::Affine2{}, 4.0f, kept);
    // Texel (16, 13), centre (16.5, 13.5), is 1.5 below the horizontal bar's
    // top edge, but that edge runs inside the vertical bar there: the union's
    // nearest point is the inner corner (20, 12).
    CHECK_THAT(field[(13 * 32 + 16) * 4 + 3], WithinAbs(-std::sqrt(14.5), 1e-4));
    CHECK_THAT(unremoved[(13 * 32 + 16) * 4 + 3], WithinAbs(-1.5, 1e-4));
    // Deterministic: the same bytes every time.
    CHECK(Pelican::multiChannelDistanceField(ring, 32, 32, Pelican::Affine2{}, 4.0f) ==
          Pelican::multiChannelDistanceField(ring, 32, 32, Pelican::Affine2{}, 4.0f));
}

TEST_CASE("CV-4 a TrueType font is read: metrics, outlines, kerning", "[canvas][cv4]") {
    const auto &bytes = testFont();
    REQUIRE(Pelican::fontFaceCount(fontSpan(bytes)) == 1);
    const Pelican::FontFile font{fontSpan(bytes), 0};
    CHECK(font.unitsPerEm() == 1000);
    CHECK(font.ascent() == 800);
    CHECK(font.descent() == -200);
    CHECK_FALSE(font.cff());
    const auto a = font.glyphIndex(U'A'), b = font.glyphIndex(U'B'), o = font.glyphIndex(U'O');
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(font.glyphIndex(U'Z') == 0);
    CHECK(font.kerning(a, b) == -120);
    CHECK(font.kerning(b, a) == 0);
    const auto glyph = font.glyph(o);
    CHECK(glyph.advance == 800);
    CHECK(glyph.bounds == std::array<int, 4>{100, 50, 700, 650});
    REQUIRE(glyph.outline.contours.size() == 2);
    CHECK(glyph.outline.contours[0].size() == 4);
    CHECK(std::all_of(glyph.outline.contours[0].begin(), glyph.outline.contours[0].end(), [](const auto &segment) {
        return segment.kind == Pelican::SegmentKind::quadratic;
    }));
    CHECK(font.glyph(font.glyphIndex(U' ')).outline.contours.empty());
    const std::u32string wanted = U"ABZ";
    const auto face = Pelican::loadFontFace(fontSpan(bytes), 0, {wanted.data(), wanted.size()});
    CHECK(face.glyphs.size() == 2);
    CHECK(face.missing == std::vector<char32_t>{U'Z'});
    REQUIRE(face.kerning.size() == 1);
    CHECK(face.kerning[0].amount == -120);
    CHECK_THROWS_AS(Pelican::FontFile(fontSpan(bytes), 1), std::invalid_argument);
    const std::vector<std::byte> garbage(64, std::byte{7});
    CHECK_THROWS_AS(Pelican::FontFile(fontSpan(garbage), 0), std::invalid_argument);
}

TEST_CASE("CV-4 text is set with kerning, lines, alignment and wrapping", "[canvas][cv4]") {
    const Pelican::FontFile font{fontSpan(testFont()), 0};
    const auto set = [&](const std::u32string &text, Pelican::CanvasTextStyle style = {}) {
        if (style.size == 0.0) style.size = 100.0;
        return Pelican::layoutCanvasText(font, text, style, "title");
    };
    // Kerning: B moves 0.12 em toward A; tracking adds after each glyph.
    auto placed = set(U"AB");
    REQUIRE(placed.size() == 2);
    CHECK_THAT(placed[1].x, WithinAbs(0.88, 1e-12));
    Pelican::CanvasTextStyle tracked;
    tracked.tracking = 50;
    CHECK_THAT(set(U"AB", tracked)[1].x, WithinAbs(0.93, 1e-12));
    // Lines: `\n` starts one line_height (em) down.
    placed = set(U"A\nBO");
    REQUIRE(placed.size() == 3);
    CHECK(placed[1].x == 0.0);
    CHECK_THAT(placed[1].y, WithinAbs(1.2, 1e-12));
    CHECK_THAT(placed[2].x, WithinAbs(0.7, 1e-12));
    // Alignment, each line on its own: centre and right about the origin.
    Pelican::CanvasTextStyle centred;
    centred.align = Pelican::CanvasTextAlign::center;
    placed = set(U"A\nBO", centred);
    CHECK_THAT(placed[0].x, WithinAbs(-0.5, 1e-12));
    CHECK_THAT(placed[1].x, WithinAbs(-0.75, 1e-12));
    Pelican::CanvasTextStyle right;
    right.align = Pelican::CanvasTextAlign::right;
    CHECK_THAT(set(U"BO", right)[0].x, WithinAbs(-1.5, 1e-12));
    // A box wraps at the last space that fits (and drops it), and aligns in
    // the box: 3.6 em at size 100 holds "BO BO" (3.3 em) and the space after.
    Pelican::CanvasTextStyle boxed;
    boxed.box_width = 360.0;
    boxed.align = Pelican::CanvasTextAlign::center;
    placed = set(U"BO BO BO", boxed);
    REQUIRE(placed.size() == 7);
    std::vector<double> lines;
    for (const auto &glyph : placed) lines.push_back(glyph.y);
    CHECK(lines == std::vector<double>{0.0, 0.0, 0.0, 0.0, 0.0, 1.2, 1.2});
    CHECK_THAT(placed[0].x, WithinAbs(0.15, 1e-12));
    CHECK_THAT(placed[5].x, WithinAbs(1.05, 1e-12));
    CHECK(placed[5].codepoint == U'B');
    // Without a space in reach, between any two characters.
    boxed.box_width = 250.0;
    boxed.align = Pelican::CanvasTextAlign::left;
    placed = set(U"永永永永永", boxed);
    REQUIRE(placed.size() == 5);
    CHECK(placed[1].y == 0.0);
    CHECK_THAT(placed[2].y, WithinAbs(1.2, 1e-12));
    CHECK(placed[2].x == 0.0);
    CHECK_THAT(placed[4].y, WithinAbs(2.4, 1e-12));
    // A character the face does not have.
    CHECK(refusal([&] { (void)set(U"AZ"); }) == R"(canvas.glyph_missing@1 {"codepoint":90,"node":"title"})");
}

TEST_CASE("CV-4 shape.text lowers to glyph items reading a baked atlas", "[canvas][cv4]") {
    const auto document = textDocument(defaultText("AB O"), {{"color", {0, 0, 0, 1}}, {"width", 4}});
    // The contract: shape.text's geometry feeds the paints, origin and size are ports.
    {
        const auto &registry = Pelican::builtinGraphModuleRegistry();
        (void)Pelican::validateGraphPorts(Pelican::expandGraphDocument(document), registry.typeRegistry(),
                                          Pelican::GraphOperationContractResolver{[&](const Json &node) {
                                              return registry.resolveOperation(node);
                                          }});
    }
    const auto lowering = Pelican::lowerCanvasNodes(document, {}, testReader());
    REQUIRE(lowering.ir.outputs.size() == 1);
    const auto &output = lowering.ir.outputs.at(0);
    // A, B, O filled, then stroked; the space draws nothing.
    REQUIRE(output.items.size() == 6);
    CHECK(Pelican::canvasItemKind(output.items[0]) == Pelican::canvas_kind_glyph_fill);
    CHECK(Pelican::canvasItemKind(output.items[3]) == Pelican::canvas_kind_glyph_stroke);
    CHECK(std::string{Pelican::canvasItemKindName(Pelican::canvas_kind_glyph_stroke)} == "glyph_stroke");
    CHECK(output.items[0].nodes == std::vector<std::string>{"title_fill", "title", "root"});
    // One atlas: three glyphs on a 2 x 2 grid of 32-texel cells.
    REQUIRE(output.atlases.size() == 1);
    const auto &atlas = output.atlases[0];
    CHECK(atlas.cell == 32);
    CHECK(atlas.pixels->width == 64);
    CHECK(atlas.pixels->height == 64);
    CHECK(atlas.path == "canvas_generated/" + atlas.stem + ".png");
    CHECK(atlas.asset == "project://" + atlas.path);
    CHECK(atlas.stem.starts_with("canvas_out_font0_"));
    // The item: B (glyph 2, cell (1, 0)) at pen 0.88 em. Its 500 x 700 box is
    // centred in 24 texels per em, (cell - 2 range): x0 = 7.6, y0 = 24.4.
    const auto &b = output.items[1];
    REQUIRE(b.glyph);
    CHECK(b.glyph->glyph == 2);
    CHECK(b.texture.asset == atlas.asset);
    CHECK(b.texture.linear);
    const auto p = [&](std::size_t index) { return b.parameters.at(index).value; };
    CHECK(p(0) == 16.0);
    CHECK(p(1) == 96.0);
    CHECK(p(2) == 64.0);
    CHECK_THAT(p(3), WithinAbs(0.88 - 7.6 / 24.0, 1e-9));
    CHECK_THAT(p(4), WithinAbs(-24.4 / 24.0, 1e-9));
    CHECK_THAT(p(5), WithinAbs(32.0 / 24.0, 1e-9));
    CHECK_THAT(p(6), WithinAbs(32.0 / 24.0, 1e-9));
    CHECK(p(7) == 0.5);
    CHECK(p(8) == 0.0);
    CHECK(p(9) == 0.5);
    CHECK(p(10) == 0.5);
    CHECK(p(11) == 32.0);
    // The glyph's box lies in its cell with the distance range to spare.
    const auto cell_x0 = p(0) + p(2) * p(3), cell_y0 = p(1) + p(2) * p(4), cell_size = p(2) * p(5);
    const auto glyph_x0 = 16.0 + 64.0 * (0.88 + 0.1), glyph_x1 = 16.0 + 64.0 * (0.88 + 0.6);
    const auto glyph_y0 = 96.0 - 64.0 * 0.7, glyph_y1 = 96.0;
    const auto room = 4.0 / 24.0 * 64.0;
    CHECK(glyph_x0 - cell_x0 >= room - 1e-9);
    CHECK(cell_x0 + cell_size - glyph_x1 >= room - 1e-9);
    CHECK(glyph_y0 - cell_y0 >= room - 1e-9);
    CHECK(cell_y0 + cell_size - glyph_y1 >= room - 1e-9);
    // The atlas texel at B's cell centre is deep inside (A < 0).
    const auto &rgba = atlas.pixels->rgba;
    CHECK(rgba[((16 * 64) + 32 + 16) * 4 + 3] < 128);
    // The lowered document reads the atlas as a linear render.image, and the
    // generated sources carry its PNG.
    const auto &nodes = lowering.document.at("nodes");
    const auto asset = std::find_if(nodes.begin(), nodes.end(), [](const Json &node) {
        return node.at("id") == Pelican::canvasAssetNodeId("out", 0);
    });
    REQUIRE(asset != nodes.end());
    CHECK(asset->at("parameters").at("source") ==
          Json{{"asset", atlas.asset}, {"color_space", "linear"}, {"alpha", "straight"}, {"mips", "none"}});
    const auto source = std::find_if(lowering.generated_sources.begin(), lowering.generated_sources.end(),
                                     [](const auto &entry) { return entry.stage == "atlas"; });
    REQUIRE(source != lowering.generated_sources.end());
    CHECK(source->path == atlas.path);
    CHECK((source->bytes == atlas.pixels->png));
    CHECK(source->text.empty());
    const auto fragment = std::find_if(lowering.generated_sources.begin(), lowering.generated_sources.end(),
                                       [](const auto &entry) { return entry.stage == "frag"; });
    REQUIRE(fragment != lowering.generated_sources.end());
    CHECK(fragment->text.find("canvasGlyphDistance") != std::string::npos);
    CHECK(fragment->text.find("case 8u: { // glyph_stroke") != std::string::npos);

    // The CPU reference draws the glyphs from the same atlas: B's middle is
    // covered, the space between B and O is not.
    const auto image = Pelican::renderCanvasReference(output, 256, 128);
    const auto alpha = [&](int x, int y) { return image.pixels[(static_cast<std::size_t>(y) * 256 + x) * 4 + 3]; };
    CHECK(alpha(95, 74) > 0.99f);
    CHECK(alpha(95, 20) == 0.0f);
    CHECK(alpha(120, 74) == 0.0f);
}

TEST_CASE("CV-4 the atlas depends on the document alone, and the memo changes nothing", "[canvas][cv4]") {
    const auto document = textDocument(defaultText("OXAB"));
    Pelican::clearCanvasTextMemo();
    const auto first = Pelican::lowerCanvasNodes(document, {}, testReader());
    const auto made = Pelican::canvasTextMemoStats();
    CHECK(made.glyph_fields == 4);
    CHECK(made.atlases == 1);
    // Again: nothing is generated or encoded, and the bytes are the same.
    const auto second = Pelican::lowerCanvasNodes(document, {}, testReader());
    const auto again = Pelican::canvasTextMemoStats();
    CHECK(again.fields_generated == made.fields_generated);
    CHECK(again.atlases_encoded == made.atlases_encoded);
    CHECK(second.document.dump() == first.document.dump());
    CHECK((second.ir.outputs[0].atlases[0].pixels->png == first.ir.outputs[0].atlases[0].pixels->png));
    // Without the memo: generated anew, the same bytes.
    Pelican::clearCanvasTextMemo();
    const auto fresh = Pelican::lowerCanvasNodes(document, {}, testReader());
    CHECK(Pelican::canvasTextMemoStats().fields_generated == made.fields_generated + 4);
    CHECK((fresh.ir.outputs[0].atlases[0].pixels->png == first.ir.outputs[0].atlases[0].pixels->png));
    CHECK(fresh.document.dump() == first.document.dump());
    // "BA" alone reuses A's and B's fields; its atlas is its own.
    const auto fields = Pelican::canvasTextMemoStats().fields_generated;
    const auto smaller = Pelican::lowerCanvasNodes(textDocument(defaultText("BA")), {}, testReader());
    CHECK(Pelican::canvasTextMemoStats().fields_generated == fields);
    CHECK((smaller.ir.outputs[0].atlases[0].pixels->png != first.ir.outputs[0].atlases[0].pixels->png));
}

TEST_CASE("CV-4 a CJK character asks for the larger cell, atlas_cell fixes it", "[canvas][cv4]") {
    const auto cjk = Pelican::lowerCanvasNodes(textDocument(defaultText("A永")), {}, testReader());
    CHECK(cjk.ir.outputs[0].atlases[0].cell == 48);
    auto parameters = defaultText("AB");
    parameters["atlas_cell"] = 64;
    const auto fixed = Pelican::lowerCanvasNodes(textDocument(parameters), {}, testReader());
    CHECK(fixed.ir.outputs[0].atlases[0].cell == 64);
    CHECK(fixed.ir.outputs[0].items[0].parameters.at(11).value == 64.0);
    parameters["atlas_cell"] = 8;
    CHECK_THROWS_AS(Pelican::lowerCanvasNodes(textDocument(parameters), {}, testReader()), std::invalid_argument);
}

TEST_CASE("CV-4 shape.text refuses by name and cuts a stroke the atlas cannot reach", "[canvas][cv4]") {
    // No reader, no file, not a font, no such face, no such glyph.
    CHECK(refusal([] { (void)Pelican::lowerCanvasNodes(textDocument(defaultText())); }) ==
          R"(canvas.font_missing@1 {"font":"project://fonts/mini.ttf","node":"title","reason":"this lowering reads no files"})");
    auto parameters = defaultText();
    parameters["font"] = "project://fonts/none.ttf";
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(textDocument(parameters), {}, testReader()); }) ==
          R"(canvas.font_missing@1 {"font":"project://fonts/none.ttf","node":"title","reason":"no such file in the project"})");
    parameters["font"] = "project://fonts/broken.ttf";
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(textDocument(parameters), {}, testReader()); })
              .starts_with(R"(canvas.font_unreadable@1 {"face":0,"font":"project://fonts/broken.ttf","node":"title")"));
    parameters = defaultText();
    parameters["face"] = 2;
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(textDocument(parameters), {}, testReader()); })
              .starts_with("canvas.font_unreadable@1"));
    CHECK(refusal([&] { (void)Pelican::lowerCanvasNodes(textDocument(defaultText("AZ")), {}, testReader()); }) ==
          R"(canvas.glyph_missing@1 {"codepoint":90,"node":"title"})");
    parameters = defaultText();
    parameters["text"] = "\xC3";
    CHECK_THROWS_AS(Pelican::lowerCanvasNodes(textDocument(parameters), {}, testReader()), std::invalid_argument);
    parameters = defaultText();
    parameters["kerning"] = true;
    CHECK_THROWS_AS(Pelican::lowerCanvasNodes(textDocument(parameters), {}, testReader()), std::invalid_argument);

    // A centred stroke reaches 3 texels each side (the 4th is the edge): at
    // 24 texels per em and size 64, 16 stack units wide; 40 is cut to it.
    const auto lowering = Pelican::lowerCanvasNodes(
        textDocument(defaultText("AB"), {{"color", {0, 0, 0, 1}}, {"width", 40}}), {}, testReader());
    REQUIRE(lowering.diagnostics.size() == 1);
    const auto &diagnostic = lowering.diagnostics.at(0);
    CHECK(diagnostic.at("code") == "canvas.glyph_range_exceeded");
    CHECK(diagnostic.at("node") == "title_stroke");
    CHECK(diagnostic.at("width") == 40.0);
    CHECK_THAT(diagnostic.at("range").get<double>(), WithinAbs(4.0 * 64.0 / 24.0, 1e-9));
    CHECK_THAT(diagnostic.at("limit").get<double>(), WithinAbs(16.0, 1e-9));
    const auto &stroke = lowering.ir.outputs[0].items.at(2);
    CHECK_THAT(stroke.parameters.at(Pelican::canvas_glyph_parameter_count + 4).value, WithinAbs(16.0, 1e-9));
    // Within reach, nothing is said.
    CHECK(Pelican::lowerCanvasNodes(textDocument(defaultText("AB"), {{"color", {0, 0, 0, 1}}, {"width", 16}}), {},
                                    testReader())
              .diagnostics.empty());
}

TEST_CASE("CV-4 the authoring hook reads the font through its context and writes the atlas", "[canvas][cv4]") {
    const auto root = std::filesystem::temp_directory_path() / "pelican_cv4_project";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "fonts");
    {
        std::ofstream out(root / "fonts" / "mini.ttf", std::ios::binary);
        out.write(reinterpret_cast<const char *>(testFont().data()), static_cast<std::streamsize>(testFont().size()));
    }
    const auto context = Pelican::projectFileLoweringContext(
        [&](std::string_view reference) -> std::optional<std::filesystem::path> {
            constexpr std::string_view scheme = "project://";
            if (!reference.starts_with(scheme)) return std::nullopt;
            return root / std::string{reference.substr(scheme.size())};
        });
    // The same buffer while the file is unchanged.
    CHECK(context.read_asset("project://fonts/mini.ttf") == context.read_asset("project://fonts/mini.ttf"));
    CHECK(context.read_asset("project://fonts/none.ttf") == nullptr);

    const auto document = textDocument(defaultText("AB"));
    CHECK_THROWS_AS(Pelican::lowerAuthoringDocument(document), Pelican::GraphRegistryError);
    const auto lowering = Pelican::lowerAuthoringDocument(document, context);
    const auto atlas = std::find_if(lowering.generated_sources.begin(), lowering.generated_sources.end(),
                                    [](const auto &source) { return source.path.ends_with(".png"); });
    REQUIRE(atlas != lowering.generated_sources.end());
    const auto &report = lowering.reports.at("canvas").at("outputs").at(0);
    REQUIRE(report.at("atlases").size() == 1);
    const auto path = report.at("atlases").at(0).at("path").get<std::string>();
    CHECK(atlas->path == path);
    const auto stem = std::filesystem::path{path}.stem().string();
    CHECK(path == ".pelican/generated/canvas/" + stem + "/" + stem + ".png");
    CHECK(report.at("kinds") == Json::array({"glyph_fill"}));
    CHECK(report.at("generated").at(0) ==
          Json{{"stem", stem}, {"stage", "atlas"}, {"path", path}, {"bytes", atlas->bytes.size()}});
    // Written as bytes; written once.
    Pelican::writeAuthoringGeneratedSources(lowering, root);
    const auto written = root / path;
    REQUIRE(std::filesystem::is_regular_file(written));
    std::ifstream in(written, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    REQUIRE(bytes.size() == atlas->bytes.size());
    CHECK(std::equal(bytes.begin(), bytes.end(), reinterpret_cast<const char *>(atlas->bytes.data())));
    CHECK(bytes.starts_with("\x89PNG"));
    const auto first_time = std::filesystem::last_write_time(written);
    Pelican::writeAuthoringGeneratedSources(lowering, root);
    CHECK(std::filesystem::last_write_time(written) == first_time);
    std::filesystem::remove_all(root);
}
