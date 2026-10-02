#include "fontfile.hpp"

// The implementation stays private to this file, so another library that
// carries its own copy of stb_truetype links beside it.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace Pelican {
namespace {

std::uint16_t readU16(const std::uint8_t *p) { return std::uint16_t((p[0] << 8) | p[1]); }

std::uint32_t readU32(const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

void appendUtf8(std::string &out, char32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// Name ID 1 from the Windows Unicode BMP record (UTF-16BE), English first,
// then any language.
std::string familyName(const stbtt_fontinfo &info) {
    int bytes = 0;
    const char *name = stbtt_GetFontNameString(&info, &bytes, STBTT_PLATFORM_ID_MICROSOFT,
                                               STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
    if (name == nullptr) return {};
    std::string out;
    const auto *p = reinterpret_cast<const std::uint8_t *>(name);
    for (int i = 0; i + 1 < bytes; i += 2) {
        char32_t unit = readU16(p + i);
        if (unit >= 0xD800 && unit < 0xDC00 && i + 3 < bytes) {
            const char32_t low = readU16(p + i + 2);
            unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            i += 2;
        }
        appendUtf8(out, unit);
    }
    return out;
}

Outline glyphOutline(const stbtt_fontinfo &info, int glyph) {
    stbtt_vertex *vertices = nullptr;
    const int count = stbtt_GetGlyphShape(&info, glyph, &vertices);
    Outline outline;
    Point2 pen{};
    Point2 start{};
    const auto close = [&]() {
        if (outline.contours.empty() || outline.contours.back().empty()) return;
        if (pen.x != start.x || pen.y != start.y) {
            OutlineSegment closing{SegmentKind::line, {pen, start}};
            outline.contours.back().push_back(closing);
        }
        pen = start;
    };
    for (int i = 0; i < count; ++i) {
        const auto &v = vertices[i];
        const Point2 to{double(v.x), double(v.y)};
        switch (v.type) {
        case STBTT_vmove:
            close();
            if (!outline.contours.empty() && outline.contours.back().empty()) outline.contours.pop_back();
            outline.contours.emplace_back();
            pen = start = to;
            break;
        case STBTT_vline:
            if (outline.contours.empty()) outline.contours.emplace_back();
            outline.contours.back().push_back({SegmentKind::line, {pen, to}});
            pen = to;
            break;
        case STBTT_vcurve:
            if (outline.contours.empty()) outline.contours.emplace_back();
            outline.contours.back().push_back({SegmentKind::quadratic, {pen, Point2{double(v.cx), double(v.cy)}, to}});
            pen = to;
            break;
        case STBTT_vcubic:
            if (outline.contours.empty()) outline.contours.emplace_back();
            outline.contours.back().push_back({SegmentKind::cubic,
                                               {pen, Point2{double(v.cx), double(v.cy)},
                                                Point2{double(v.cx1), double(v.cy1)}, to}});
            pen = to;
            break;
        default:
            break;
        }
    }
    close();
    if (!outline.contours.empty() && outline.contours.back().empty()) outline.contours.pop_back();
    stbtt_FreeShape(&info, vertices);
    return outline;
}

bool isComposite(const stbtt_fontinfo &info, int glyph) {
    if (info.cff.size != 0) return false;
    const int offset = stbtt__GetGlyfOffset(&info, glyph);
    if (offset < 0) return false;
    return static_cast<std::int16_t>(readU16(info.data + offset)) < 0;
}

// ---- GPOS pair kerning ----
//
// stbtt_GetGlyphKernAdvance reads GPOS only through plain type-2 lookups
// with x-advance-only records, takes every type-2 lookup whatever its
// feature, and ignores 'kern' whenever GPOS exists; Arial keeps all of its
// pair lookups behind extension lookups (type 9), so stb reports no kerning
// for it. This reader follows extension lookups, takes the lookups of the
// 'kern' feature only, reads any value format (the first glyph's XAdvance),
// and falls back to the 'kern' table when GPOS has no 'kern' feature.

int valueRecordSize(int format) { return 2 * std::popcount(unsigned(format) & 0xFFu); }

// Byte offset of XAdvance in a value record, or -1 when the format has none.
int xAdvanceOffset(int format) {
    if (!(format & 0x0004)) return -1;
    return 2 * std::popcount(unsigned(format) & 0x0003u);
}

struct GposKerning {
    std::vector<const std::uint8_t *> pair_tables; // PairPos subtables of the 'kern' lookups, in lookup order
    std::vector<int> lookup_of_table;               // the lookup each belongs to
};

GposKerning gposKerning(const stbtt_fontinfo &info) {
    GposKerning result;
    if (!info.gpos) return result;
    const auto *gpos = info.data + info.gpos;
    if (readU16(gpos) != 1) return result;
    const auto *features = gpos + readU16(gpos + 6);
    const auto *lookups = gpos + readU16(gpos + 8);
    std::vector<int> kern_lookups;
    for (int f = 0, count = readU16(features); f < count; ++f) {
        const auto *record = features + 2 + 6 * f;
        if (std::memcmp(record, "kern", 4) != 0) continue;
        const auto *feature = features + readU16(record + 4);
        for (int k = 0, n = readU16(feature + 2); k < n; ++k) kern_lookups.push_back(readU16(feature + 4 + 2 * k));
    }
    std::sort(kern_lookups.begin(), kern_lookups.end());
    kern_lookups.erase(std::unique(kern_lookups.begin(), kern_lookups.end()), kern_lookups.end());
    const int lookup_count = readU16(lookups);
    for (const int index : kern_lookups) {
        if (index >= lookup_count) continue;
        const auto *lookup = lookups + readU16(lookups + 2 + 2 * index);
        const int type = readU16(lookup);
        if (type != 2 && type != 9) continue;
        for (int s = 0, n = readU16(lookup + 4); s < n; ++s) {
            const auto *table = lookup + readU16(lookup + 6 + 2 * s);
            if (type == 9) {
                if (readU16(table) != 1 || readU16(table + 2) != 2) continue;
                table += readU32(table + 4);
            }
            const int format = readU16(table);
            if (format != 1 && format != 2) continue;
            result.pair_tables.push_back(table);
            result.lookup_of_table.push_back(index);
        }
    }
    return result;
}

// The x-advance adjustment of one PairPos subtable for (left, right), or
// nullopt when the subtable does not apply (so the lookup tries its next).
std::optional<int> pairAdjustment(const std::uint8_t *table, int left, int right) {
    const int format = readU16(table);
    const int coverage = stbtt__GetCoverageIndex(const_cast<std::uint8_t *>(table) + readU16(table + 2), left);
    if (coverage < 0) return std::nullopt;
    const int format1 = readU16(table + 4);
    const int format2 = readU16(table + 6);
    const int advance = xAdvanceOffset(format1);
    if (format == 1) {
        if (coverage >= readU16(table + 8)) return std::nullopt;
        const auto *set = table + readU16(table + 10 + 2 * coverage);
        const int record = 2 + valueRecordSize(format1) + valueRecordSize(format2);
        int lo = 0, hi = readU16(set) - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const auto *entry = set + 2 + record * mid;
            const int second = readU16(entry);
            if (second < right) lo = mid + 1;
            else if (second > right) hi = mid - 1;
            else return advance < 0 ? 0 : int(std::int16_t(readU16(entry + 2 + advance)));
        }
        return std::nullopt;
    }
    const int class1 = stbtt__GetGlyphClass(const_cast<std::uint8_t *>(table) + readU16(table + 8), left);
    const int class2 = stbtt__GetGlyphClass(const_cast<std::uint8_t *>(table) + readU16(table + 10), right);
    const int class1_count = readU16(table + 12);
    const int class2_count = readU16(table + 14);
    if (class1 < 0 || class1 >= class1_count || class2 < 0 || class2 >= class2_count) return 0;
    const int record = valueRecordSize(format1) + valueRecordSize(format2);
    const auto *entry = table + 16 + record * (class1 * class2_count + class2);
    return advance < 0 ? 0 : int(std::int16_t(readU16(entry + advance)));
}

} // namespace

struct FontFile::State {
    stbtt_fontinfo info{};
    std::string family;
    int units_per_em = 0;
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    GposKerning gpos;
};

FontFile::FontFile(std::span<const std::uint8_t> bytes, int face_index) : state(std::make_unique<State>()) {
    if (bytes.empty()) throw std::invalid_argument("font file is empty");
    if (face_index < 0 || face_index >= fontFaceCount(bytes))
        throw std::invalid_argument("font has no face " + std::to_string(face_index));
    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), face_index);
    if (offset < 0 || static_cast<std::size_t>(offset) >= bytes.size())
        throw std::invalid_argument("font has no face " + std::to_string(face_index));
    if (!stbtt_InitFont(&state->info, bytes.data(), offset))
        throw std::invalid_argument("font face " + std::to_string(face_index) + " does not parse");
    if (state->info.head == 0) throw std::invalid_argument("font face has no head table");
    state->family = familyName(state->info);
    state->units_per_em = readU16(state->info.data + state->info.head + 18);
    if (state->units_per_em <= 0) throw std::invalid_argument("font face has no units per em");
    stbtt_GetFontVMetrics(&state->info, &state->ascent, &state->descent, &state->line_gap);
    state->gpos = gposKerning(state->info);
}

FontFile::~FontFile() = default;
FontFile::FontFile(FontFile &&) noexcept = default;
FontFile &FontFile::operator=(FontFile &&) noexcept = default;

const std::string &FontFile::family() const { return state->family; }
bool FontFile::cff() const { return state->info.cff.size != 0; }
int FontFile::unitsPerEm() const { return state->units_per_em; }
int FontFile::ascent() const { return state->ascent; }
int FontFile::descent() const { return state->descent; }
int FontFile::lineGap() const { return state->line_gap; }

int FontFile::glyphIndex(char32_t codepoint) const {
    return stbtt_FindGlyphIndex(&state->info, static_cast<int>(codepoint));
}

int FontFile::advance(int glyph_index) const {
    int advance = 0, left_side_bearing = 0;
    stbtt_GetGlyphHMetrics(&state->info, glyph_index, &advance, &left_side_bearing);
    return advance;
}

FontGlyph FontFile::glyph(int glyph_index) const {
    FontGlyph entry;
    entry.glyph_index = glyph_index;
    stbtt_GetGlyphHMetrics(&state->info, glyph_index, &entry.advance, &entry.left_side_bearing);
    if (!stbtt_IsGlyphEmpty(&state->info, glyph_index)) {
        stbtt_GetGlyphBox(&state->info, glyph_index, &entry.bounds[0], &entry.bounds[1], &entry.bounds[2],
                          &entry.bounds[3]);
        entry.outline = glyphOutline(state->info, glyph_index);
    }
    entry.composite = isComposite(state->info, glyph_index);
    return entry;
}

int FontFile::kerning(int left_glyph, int right_glyph) const {
    const auto &gpos = state->gpos;
    if (gpos.pair_tables.empty())
        return state->info.kern ? stbtt__GetGlyphKernInfoAdvance(&state->info, left_glyph, right_glyph) : 0;
    int total = 0;
    int applied_lookup = -1;
    for (std::size_t i = 0; i < gpos.pair_tables.size(); ++i) {
        const int lookup = gpos.lookup_of_table[i];
        if (lookup == applied_lookup) continue; // a lookup applies its first matching subtable only
        if (const auto amount = pairAdjustment(gpos.pair_tables[i], left_glyph, right_glyph)) {
            total += *amount;
            applied_lookup = lookup;
        }
    }
    return total;
}

int fontFaceCount(std::span<const std::uint8_t> bytes) {
    // stbtt reads the first bytes of the header without a length.
    if (bytes.size() < 12) return 0;
    return std::max(stbtt_GetNumberOfFonts(bytes.data()), 0);
}

FontFace loadFontFace(std::span<const std::uint8_t> bytes, int face_index, std::span<const char32_t> codepoints) {
    const FontFile file(bytes, face_index);
    FontFace face;
    face.family = file.family();
    face.cff = file.cff();
    face.units_per_em = file.unitsPerEm();
    face.ascent = file.ascent();
    face.descent = file.descent();
    face.line_gap = file.lineGap();
    std::vector<char32_t> wanted(codepoints.begin(), codepoints.end());
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (const auto codepoint : wanted) {
        const int glyph = file.glyphIndex(codepoint);
        if (glyph == 0) {
            face.missing.push_back(codepoint);
            continue;
        }
        auto entry = file.glyph(glyph);
        entry.codepoint = codepoint;
        face.glyphs.push_back(std::move(entry));
    }
    for (const auto &left : face.glyphs)
        for (const auto &right : face.glyphs)
            if (const int amount = file.kerning(left.glyph_index, right.glyph_index); amount != 0)
                face.kerning.push_back({left.codepoint, right.codepoint, amount});
    return face;
}

} // namespace Pelican
