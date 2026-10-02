#pragma once

#include "distancefield.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

// DF-1 / CV-4: reading a font file (TrueType / OpenType-CFF / collection):
// metrics, glyph outlines in font units (y up), and pair kerning. Parsing is
// stb_truetype's; this file owns the stb implementation
// (STB_TRUETYPE_IMPLEMENTATION). Only the making side reads fonts: the
// canvas lowering bakes what a document uses into an atlas, and nothing that
// ships reads a font.
//
// Kerning: the GPOS lookups of the 'kern' feature (pair adjustment, formats 1
// and 2, also behind extension lookups; the first glyph's XAdvance), else the
// 'kern' table. stb's own stbtt_GetGlyphKernAdvance misses extension lookups
// (all of Arial's) and takes type-2 lookups of any feature, so it is not used.
// No script / language selection, no contextual or mark positioning.

struct FontGlyph {
    char32_t codepoint = 0;
    int glyph_index = 0;
    int advance = 0;
    int left_side_bearing = 0;
    std::array<int, 4> bounds{}; // x0, y0, x1, y1 (font units); all 0 for an empty glyph
    Outline outline;             // font units, y up
    bool composite = false;      // built from other glyphs (TrueType glyf only)
};

struct FontKerningPair {
    char32_t left = 0;
    char32_t right = 0;
    int amount = 0; // font units, added to the left glyph's advance
};

// One face of a font file. It reads `bytes` in place: the caller keeps them
// alive (and unchanged) for as long as the face is used. Not for concurrent
// use.
class FontFile {
public:
    // Throws std::invalid_argument for a face that does not exist or does not parse.
    FontFile(std::span<const std::uint8_t> bytes, int face_index);
    ~FontFile();
    FontFile(FontFile &&) noexcept;
    FontFile &operator=(FontFile &&) noexcept;

    // Name ID 1 (Windows Unicode first), UTF-8; may be empty.
    const std::string &family() const;
    // CFF outlines (cubic) rather than glyf (quadratic).
    bool cff() const;
    int unitsPerEm() const;
    int ascent() const;
    int descent() const;
    int lineGap() const;
    // 0 when the face has no glyph for the codepoint.
    int glyphIndex(char32_t codepoint) const;
    // The advance of a glyph, in font units.
    int advance(int glyph_index) const;
    // Metrics, bounds and outline of a glyph (an empty glyph has no contour).
    FontGlyph glyph(int glyph_index) const;
    // The kerning between two glyphs, in font units.
    int kerning(int left_glyph, int right_glyph) const;

private:
    struct State;
    std::unique_ptr<State> state;
};

// The number of faces in a file (1 for a plain ttf / otf, 0 for no font).
int fontFaceCount(std::span<const std::uint8_t> bytes);

struct FontFace {
    std::string family;
    bool cff = false;
    int units_per_em = 0;
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    std::vector<FontGlyph> glyphs;        // by codepoint
    std::vector<char32_t> missing;        // requested codepoints the font has no glyph for
    std::vector<FontKerningPair> kerning; // non-zero pairs among the loaded glyphs, by (left, right)
};

// Every requested glyph of one face at once (FontFile's reads, gathered).
// Throws std::invalid_argument for a face that does not exist or does not parse.
FontFace loadFontFace(std::span<const std::uint8_t> bytes, int face_index, std::span<const char32_t> codepoints);

} // namespace Pelican
