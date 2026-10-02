#include "canvastext.hpp"

#include "../graphcomposition.hpp"
#include "distancefield.hpp"

#include <picosha2.h>

// The atlas PNG; the implementation stays private to this file.
#define STB_IMAGE_WRITE_STATIC
#define STBI_MSC_SECURE_CRT
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <compare>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Pelican {
namespace {

bool isSpace(char32_t c) { return c == U' ' || c == U'\t' || c == 0x3000; }

// --- the process-wide memo ---------------------------------------------------

struct FieldKey {
    std::string digest;
    int face = 0;
    int glyph = 0;
    std::uint32_t cell = 0;
    double range = 0.0;
    auto operator<=>(const FieldKey &) const = default;
};

struct AtlasKey {
    std::string digest;
    int face = 0;
    std::uint32_t cell = 0;
    std::vector<int> glyphs;
    auto operator<=>(const AtlasKey &) const = default;
};

// Bounded so a long editing session does not grow it without end: past a
// limit a table starts over (only speed is lost).
constexpr std::size_t memo_field_limit = 65536;
constexpr std::size_t memo_atlas_limit = 64;
constexpr std::size_t memo_digest_limit = 32;

struct Memo {
    std::mutex mutex;
    std::map<FieldKey, std::shared_ptr<const std::vector<std::uint8_t>>> fields;
    std::map<AtlasKey, std::shared_ptr<const CanvasAtlasPixels>> atlases;
    // A file's bytes (the reader hands the same buffer while the file is
    // unchanged) and their digest; the entry keeps the buffer, so its address
    // is not reused while it is a key.
    std::map<const void *, std::pair<std::shared_ptr<const std::vector<std::byte>>, std::string>> digests;
    std::size_t fields_generated = 0;
    std::size_t atlases_encoded = 0;
};

Memo &memo() {
    static Memo instance;
    return instance;
}

std::string bytesDigest(const std::shared_ptr<const std::vector<std::byte>> &bytes) {
    auto &shared = memo();
    {
        const std::lock_guard lock{shared.mutex};
        if (const auto found = shared.digests.find(bytes.get()); found != shared.digests.end())
            return found->second.second;
    }
    const auto *begin = reinterpret_cast<const unsigned char *>(bytes->data());
    auto digest = picosha2::hash256_hex_string(begin, begin + bytes->size());
    const std::lock_guard lock{shared.mutex};
    if (shared.digests.size() >= memo_digest_limit) shared.digests.clear();
    shared.digests.emplace(bytes.get(), std::pair{bytes, digest});
    return digest;
}

// --- one glyph's field ---------------------------------------------------------

std::uint8_t quantize(float distance) {
    const auto value = std::round((distance / (2.0 * canvas_glyph_range) + 0.5) * 255.0);
    return static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0));
}

std::vector<std::uint8_t> glyphField(const FontGlyph &glyph, int units_per_em, std::uint32_t cell) {
    const auto placement = canvasGlyphPlacement(glyph, units_per_em, cell);
    Affine2 transform;
    transform.xx = placement.scale;
    transform.yy = -placement.scale;
    transform.x0 = placement.x0;
    transform.y0 = placement.y0;
    const auto size = static_cast<int>(cell);
    const auto field = multiChannelDistanceField(glyph.outline, size, size, transform,
                                                 static_cast<float>(canvas_glyph_range));
    std::vector<std::uint8_t> bytes(field.size());
    std::transform(field.begin(), field.end(), bytes.begin(), quantize);
    return bytes;
}

void appendPng(void *context, void *data, int size) {
    auto &out = *static_cast<std::vector<std::byte> *>(context);
    const auto *bytes = static_cast<const std::byte *>(data);
    out.insert(out.end(), bytes, bytes + size);
}

} // namespace

std::u32string decodeCanvasUtf8(std::string_view text, const std::string &what) {
    std::u32string result;
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;
        char32_t codepoint = 0;
        if (lead < 0x80) length = 1, codepoint = lead;
        else if ((lead & 0xE0) == 0xC0) length = 2, codepoint = lead & 0x1F;
        else if ((lead & 0xF0) == 0xE0) length = 3, codepoint = lead & 0x0F;
        else if ((lead & 0xF8) == 0xF0) length = 4, codepoint = lead & 0x07;
        else throw std::invalid_argument(what + " is not UTF-8");
        if (at + length > text.size()) throw std::invalid_argument(what + " is not UTF-8");
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[at + k]);
            if ((next & 0xC0) != 0x80) throw std::invalid_argument(what + " is not UTF-8");
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        static constexpr char32_t shortest[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (codepoint < shortest[length] || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint < 0xE000))
            throw std::invalid_argument(what + " is not UTF-8");
        result.push_back(codepoint);
        at += length;
    }
    return result;
}

std::vector<CanvasPlacedGlyph> layoutCanvasText(const FontFile &font, std::u32string_view text,
                                                const CanvasTextStyle &style, const std::string &node) {
    const double em = font.unitsPerEm();
    const double tracking = style.tracking / 1000.0;
    const bool boxed = style.box_width > 0.0;
    const double box = boxed ? style.box_width / style.size : 0.0;
    std::vector<CanvasPlacedGlyph> placed;
    std::size_t line = 0;
    // One line: pen positions from 0, and the width to the end of its last
    // glyph that is not a space.
    const auto setLine = [&](const std::vector<std::pair<char32_t, int>> &glyphs, std::size_t first, std::size_t last) {
        std::vector<double> pen;
        double x = 0.0, width = 0.0;
        for (std::size_t k = first; k < last; ++k) {
            if (k > first) x += font.kerning(glyphs[k - 1].second, glyphs[k].second) / em + tracking;
            pen.push_back(x);
            x += font.advance(glyphs[k].second) / em;
            if (!isSpace(glyphs[k].first)) width = x;
        }
        double shift = 0.0;
        if (style.align == CanvasTextAlign::center) shift = boxed ? 0.5 * (box - width) : -0.5 * width;
        else if (style.align == CanvasTextAlign::right) shift = boxed ? box - width : -width;
        for (std::size_t k = first; k < last; ++k)
            placed.push_back({glyphs[k].first, glyphs[k].second, pen[k - first] + shift,
                              static_cast<double>(line) * style.line_height});
        ++line;
    };
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find(U'\n', start);
        if (end == std::u32string_view::npos) end = text.size();
        std::vector<std::pair<char32_t, int>> glyphs;
        for (std::size_t k = start; k < end; ++k) {
            if (text[k] == U'\r') continue;
            const int glyph = font.glyphIndex(text[k]);
            if (glyph == 0)
                throw GraphRegistryError("canvas.glyph_missing@1",
                                         {{"node", node}, {"codepoint", static_cast<std::uint32_t>(text[k])}});
            glyphs.emplace_back(text[k], glyph);
        }
        if (!boxed) {
            setLine(glyphs, 0, glyphs.size());
        } else {
            // Greedy: as many glyphs as fit the box, cut at the last space
            // that fits, else before the glyph that does not fit.
            std::size_t first = 0;
            do {
                std::size_t next = first;
                std::size_t space = glyphs.size();
                double x = 0.0;
                bool cut_at_space = false;
                for (; next < glyphs.size(); ++next) {
                    if (next > first) x += font.kerning(glyphs[next - 1].second, glyphs[next].second) / em + tracking;
                    const auto right = x + font.advance(glyphs[next].second) / em;
                    if (right > box && next > first) {
                        cut_at_space = isSpace(glyphs[next].first);
                        break;
                    }
                    if (isSpace(glyphs[next].first) && next > first) space = next;
                    x = right;
                }
                if (next == glyphs.size()) {
                    setLine(glyphs, first, next);
                    first = next;
                } else if (cut_at_space) {
                    setLine(glyphs, first, next);
                    first = next + 1;
                } else if (space < glyphs.size()) {
                    setLine(glyphs, first, space);
                    first = space + 1;
                } else {
                    setLine(glyphs, first, next);
                    first = next;
                }
            } while (first < glyphs.size());
        }
        start = end + 1;
    }
    return placed;
}

const FontGlyph &CanvasFontLibrary::Face::glyph(int glyph_index) {
    auto found = glyphs.find(glyph_index);
    if (found == glyphs.end()) found = glyphs.emplace(glyph_index, file->glyph(glyph_index)).first;
    return found->second;
}

CanvasFontLibrary::CanvasFontLibrary(CanvasAssetReader reader) : reader_{std::move(reader)} {}

std::size_t CanvasFontLibrary::open(const std::string &reference, int face, const std::string &node) {
    for (std::size_t index = 0; index < faces_.size(); ++index)
        if (faces_[index]->reference == reference && faces_[index]->index == face) return index;
    if (!reader_)
        throw GraphRegistryError("canvas.font_missing@1",
                                 {{"node", node}, {"font", reference}, {"reason", "this lowering reads no files"}});
    auto bytes = reader_(reference);
    if (bytes == nullptr)
        throw GraphRegistryError("canvas.font_missing@1",
                                 {{"node", node}, {"font", reference}, {"reason", "no such file in the project"}});
    auto entry = std::make_unique<Face>();
    entry->reference = reference;
    entry->index = face;
    entry->bytes = std::move(bytes);
    try {
        entry->file = std::make_unique<FontFile>(
            std::span{reinterpret_cast<const std::uint8_t *>(entry->bytes->data()), entry->bytes->size()}, face);
    } catch (const std::invalid_argument &error) {
        throw GraphRegistryError("canvas.font_unreadable@1",
                                 {{"node", node}, {"font", reference}, {"face", face}, {"reason", error.what()}});
    }
    entry->digest = bytesDigest(entry->bytes);
    faces_.push_back(std::move(entry));
    return faces_.size() - 1;
}

CanvasGlyphPlacement canvasGlyphPlacement(const FontGlyph &glyph, int units_per_em, std::uint32_t cell) {
    const auto room = static_cast<double>(cell) - 2.0 * canvas_glyph_range;
    const double width = glyph.bounds[2] - glyph.bounds[0];
    const double height = glyph.bounds[3] - glyph.bounds[1];
    CanvasGlyphPlacement placement;
    placement.scale = room / std::max({static_cast<double>(units_per_em), width, height});
    placement.x0 = canvas_glyph_range + 0.5 * (room - width * placement.scale) - glyph.bounds[0] * placement.scale;
    placement.y0 = canvas_glyph_range + 0.5 * (room - height * placement.scale) + glyph.bounds[3] * placement.scale;
    return placement;
}

CanvasBakedAtlas bakeCanvasAtlas(CanvasFontLibrary::Face &face, const std::vector<int> &glyphs, std::uint32_t cell) {
    if (glyphs.empty()) throw std::invalid_argument("a canvas atlas needs a glyph");
    CanvasBakedAtlas baked;
    const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(glyphs.size()))));
    for (std::size_t k = 0; k < glyphs.size(); ++k)
        baked.cells[glyphs[k]] = {static_cast<std::uint32_t>(k % columns), static_cast<std::uint32_t>(k / columns)};
    auto &shared = memo();
    AtlasKey atlas_key{face.digest, face.index, cell, glyphs};
    {
        const std::lock_guard lock{shared.mutex};
        if (const auto found = shared.atlases.find(atlas_key); found != shared.atlases.end()) {
            baked.pixels = found->second;
            return baked;
        }
    }
    // Each glyph's field: the memo's, or made now (in parallel).
    const auto units_per_em = face.file->unitsPerEm();
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> fields(glyphs.size());
    std::vector<std::size_t> missing;
    {
        const std::lock_guard lock{shared.mutex};
        for (std::size_t k = 0; k < glyphs.size(); ++k) {
            const auto found = shared.fields.find({face.digest, face.index, glyphs[k], cell, canvas_glyph_range});
            if (found != shared.fields.end()) fields[k] = found->second;
            else missing.push_back(k);
        }
    }
    if (!missing.empty()) {
        // The outlines are read here: the font file is not for concurrent use.
        std::vector<const FontGlyph *> outlines;
        for (const auto k : missing) outlines.push_back(&face.glyph(glyphs[k]));
        std::atomic<std::size_t> next{0};
        std::exception_ptr failure;
        std::mutex failure_mutex;
        const auto work = [&] {
            for (std::size_t at; (at = next.fetch_add(1)) < missing.size();) {
                try {
                    fields[missing[at]] =
                        std::make_shared<const std::vector<std::uint8_t>>(glyphField(*outlines[at], units_per_em, cell));
                } catch (...) {
                    const std::lock_guard lock{failure_mutex};
                    if (!failure) failure = std::current_exception();
                }
            }
        };
        const auto threads = std::min<std::size_t>(std::max(1u, std::thread::hardware_concurrency()), missing.size());
        std::vector<std::thread> pool;
        for (std::size_t k = 1; k < threads; ++k) pool.emplace_back(work);
        work();
        for (auto &thread : pool) thread.join();
        if (failure) std::rethrow_exception(failure);
        const std::lock_guard lock{shared.mutex};
        if (shared.fields.size() + missing.size() > memo_field_limit) shared.fields.clear();
        for (const auto k : missing)
            shared.fields[{face.digest, face.index, glyphs[k], cell, canvas_glyph_range}] = fields[k];
        shared.fields_generated += missing.size();
    }
    // The grid; a cell no glyph takes is "far outside" (255).
    auto pixels = std::make_shared<CanvasAtlasPixels>();
    const auto rows = static_cast<std::uint32_t>((glyphs.size() + columns - 1) / columns);
    pixels->width = columns * cell;
    pixels->height = rows * cell;
    pixels->rgba.assign(std::size_t{pixels->width} * pixels->height * 4, 255);
    for (std::size_t k = 0; k < glyphs.size(); ++k) {
        const auto [column, row] = baked.cells.at(glyphs[k]);
        for (std::uint32_t y = 0; y < cell; ++y)
            std::copy_n(fields[k]->data() + std::size_t{y} * cell * 4, std::size_t{cell} * 4,
                        pixels->rgba.data() + ((std::size_t{row} * cell + y) * pixels->width + std::size_t{column} * cell) * 4);
    }
    stbi_write_png_to_func(appendPng, &pixels->png, static_cast<int>(pixels->width), static_cast<int>(pixels->height),
                           4, pixels->rgba.data(), static_cast<int>(pixels->width * 4));
    if (pixels->png.empty()) throw std::runtime_error("cannot encode the canvas glyph atlas");
    baked.pixels = pixels;
    const std::lock_guard lock{shared.mutex};
    if (shared.atlases.size() >= memo_atlas_limit) shared.atlases.clear();
    shared.atlases.emplace(std::move(atlas_key), std::move(pixels));
    ++shared.atlases_encoded;
    return baked;
}

CanvasTextMemoStats canvasTextMemoStats() {
    auto &shared = memo();
    const std::lock_guard lock{shared.mutex};
    return {shared.fields.size(), shared.atlases.size(), shared.fields_generated, shared.atlases_encoded};
}

void clearCanvasTextMemo() {
    auto &shared = memo();
    const std::lock_guard lock{shared.mutex};
    shared.fields.clear();
    shared.atlases.clear();
    shared.digests.clear();
}

} // namespace Pelican
