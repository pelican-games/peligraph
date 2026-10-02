#pragma once

#include "canvaslowering.hpp"
#include "fontfile.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// CV-4: what the canvas lowering does for shape.text -- reading the font a
// document names, setting its text, and baking the glyphs an output uses into
// an MTSDF atlas (distancefield.hpp). Only the making side links it: a lowered
// document reads the atlas as an image and never the font.
//
// The atlas: one per output and font face, a grid of `cell` x `cell` texels
// per glyph in glyph-index order, row-major from the top left. A texel holds
// the MTSDF distances (RGB the multi-channel pseudo-distances, A the true
// distance; texels, positive outside) as 8-bit `d / (2 * range) + 0.5`,
// range = canvas_glyph_range. A glyph is fitted into its cell with `range`
// texels of room on each side: at (cell - 2 range) texels per em, or smaller
// for a glyph whose box is larger than an em.

inline constexpr double canvas_glyph_range = 4.0;
inline constexpr std::uint32_t canvas_glyph_cell_latin = 32;
inline constexpr std::uint32_t canvas_glyph_cell_cjk = 48;
// Codepoints from here on (CJK radicals onwards) ask for the larger cell.
inline constexpr char32_t canvas_glyph_cjk_first = 0x2E80;

// UTF-8 to codepoints; a malformed sequence is refused (std::invalid_argument).
std::u32string decodeCanvasUtf8(std::string_view text, const std::string &what);

enum class CanvasTextAlign : std::uint8_t { left, center, right };

struct CanvasTextStyle {
    // The authored size (1 em in stack units): only `box_width` reads it.
    double size = 0.0;
    CanvasTextAlign align = CanvasTextAlign::left;
    double line_height = 1.2; // em per line
    double tracking = 0.0;    // 1/1000 em added after each glyph but the last of a line
    double box_width = 0.0;   // stack units; > 0 wraps
};

// One glyph set: its pen position (the baseline's left end) in em, +y down,
// relative to the text's origin.
struct CanvasPlacedGlyph {
    char32_t codepoint = 0;
    int glyph = 0;
    double x = 0.0;
    double y = 0.0;
};

// Left to right with the face's kerning, `\n` starts a line (`\r` is
// dropped), each line aligned on its own: to the origin, or within the box
// when box_width > 0. A box wraps at the last space that fits (the space is
// dropped), else between any two characters (scripts written without
// spaces). A codepoint the face has no glyph for is
// canvas.glyph_missing@1 {node, codepoint}.
std::vector<CanvasPlacedGlyph> layoutCanvasText(const FontFile &font, std::u32string_view text,
                                                const CanvasTextStyle &style, const std::string &node);

// The fonts one lowering reads, each (reference, face) opened once.
class CanvasFontLibrary {
public:
    struct Face {
        std::string reference;
        int index = 0;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        // SHA-256 of the bytes: the memo's key for the face's glyphs.
        std::string digest;
        std::unique_ptr<FontFile> file;
        std::map<int, FontGlyph> glyphs;
        const FontGlyph &glyph(int glyph_index);
    };

    explicit CanvasFontLibrary(CanvasAssetReader reader);

    // canvas.font_missing@1 {node, font, reason} when the reader has no such
    // file (or there is no reader); canvas.font_unreadable@1 {node, font,
    // face, reason} when it is not a font or has no such face.
    std::size_t open(const std::string &reference, int face, const std::string &node);
    Face &face(std::size_t index) { return *faces_.at(index); }

private:
    CanvasAssetReader reader_;
    std::vector<std::unique_ptr<Face>> faces_;
};

// Where a glyph sits in its cell: texel = (scale x + x0, -scale y + y0) for a
// font-unit point (x, y). Depends on the glyph's box, the face's units per em
// and the cell alone.
struct CanvasGlyphPlacement {
    double scale = 0.0;
    double x0 = 0.0;
    double y0 = 0.0;
};
CanvasGlyphPlacement canvasGlyphPlacement(const FontGlyph &glyph, int units_per_em, std::uint32_t cell);

// Bakes the glyphs (glyph indices, ascending, each with an outline) of one
// face into an atlas: CanvasAtlasPixels plus where each glyph's cell is
// (column, row). Generation runs on several threads, one glyph at a time;
// the bytes depend on the face, the glyphs and the cell alone. Each glyph's
// field and each whole atlas are kept in a process-wide memo, so lowering
// the same document again generates nothing.
struct CanvasBakedAtlas {
    std::shared_ptr<const CanvasAtlasPixels> pixels;
    std::map<int, std::array<std::uint32_t, 2>> cells;
};
CanvasBakedAtlas bakeCanvasAtlas(CanvasFontLibrary::Face &face, const std::vector<int> &glyphs, std::uint32_t cell);

// The memo, for tests: what it holds and how much it generated.
struct CanvasTextMemoStats {
    std::size_t glyph_fields = 0;     // fields held
    std::size_t atlases = 0;          // atlases held
    std::size_t fields_generated = 0; // since the process started
    std::size_t atlases_encoded = 0;  // since the process started
};
CanvasTextMemoStats canvasTextMemoStats();
void clearCanvasTextMemo();

} // namespace Pelican
