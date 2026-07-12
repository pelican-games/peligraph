#include "bitmapfont.hpp"

#include "../loader/engineresources.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican::ui {
namespace {

std::uint32_t uintField(const nlohmann::json &json, const char *name, std::string_view context) {
    if (!json.contains(name) || !json.at(name).is_number_unsigned()) {
        throw std::runtime_error(std::string{context} + " requires non-negative integer field: " + name);
    }
    const auto value = json.at(name).get<std::uint64_t>();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string{context} + " field exceeds uint32: " + name);
    }
    return static_cast<std::uint32_t>(value);
}

std::int32_t intField(const nlohmann::json &json, const char *name, std::string_view context) {
    if (!json.contains(name) || !json.at(name).is_number_integer()) {
        throw std::runtime_error(std::string{context} + " requires integer field: " + name);
    }
    const auto value = json.at(name).get<std::int64_t>();
    if (value < 0 || value > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error(std::string{context} + " field is outside non-negative int32: " + name);
    }
    return static_cast<std::int32_t>(value);
}

std::uint32_t normalized(unsigned char code) noexcept {
    return code < BitmapFont::firstCode || code > BitmapFont::lastCode
               ? BitmapFont::fallbackCode
               : static_cast<std::uint32_t>(code);
}

} // namespace

BitmapFont BitmapFont::fromJson(std::string_view json_text) {
    const auto json = nlohmann::json::parse(json_text);
    if (json.value("schema", std::string{}) != "pelican.debug_text_font" || json.value("version", 0) != 1) {
        throw std::runtime_error("bitmap font table schema is not supported");
    }
    BitmapFont result;
    result.atlas_width_ = uintField(json, "atlas_width", "bitmap font table");
    result.atlas_height_ = uintField(json, "atlas_height", "bitmap font table");
    result.cell_width_ = uintField(json, "cell_width", "bitmap font table");
    result.cell_height_ = uintField(json, "cell_height", "bitmap font table");
    if (uintField(json, "first_code", "bitmap font table") != firstCode ||
        uintField(json, "last_code", "bitmap font table") != lastCode ||
        !json.contains("glyphs") || !json.at("glyphs").is_array() ||
        json.at("glyphs").size() != result.glyphs_.size()) {
        throw std::runtime_error("bitmap font table must contain each ASCII glyph 32..126 exactly once");
    }
    std::array<bool, lastCode - firstCode + 1> seen{};
    for (const auto &entry : json.at("glyphs")) {
        const auto code = uintField(entry, "code", "bitmap glyph");
        if (code < firstCode || code > lastCode || seen[code - firstCode]) {
            throw std::runtime_error("bitmap font table contains an invalid or duplicate glyph code");
        }
        seen[code - firstCode] = true;
        auto &glyph = result.glyphs_[code - firstCode];
        glyph = {intField(entry, "x", "bitmap glyph"), intField(entry, "y", "bitmap glyph"),
                 intField(entry, "w", "bitmap glyph"), intField(entry, "h", "bitmap glyph"),
                 intField(entry, "advance", "bitmap glyph")};
        if (std::int64_t{glyph.x} + glyph.width > result.atlas_width_ ||
            std::int64_t{glyph.y} + glyph.height > result.atlas_height_) {
            throw std::runtime_error("bitmap glyph lies outside the atlas");
        }
    }
    return result;
}

BitmapFont BitmapFont::bundledDebugFont() {
    return fromJson(engineResourceOrThrow("debug_text_font.json"));
}

const BitmapGlyph &BitmapFont::glyph(std::uint32_t code) const noexcept {
    if (code < firstCode || code > lastCode) code = fallbackCode;
    return glyphs_[code - firstCode];
}

std::vector<PositionedGlyph> BitmapFont::layout(std::int32_t x, std::int32_t y,
                                                std::string_view text, std::uint32_t scale) const {
    scale = std::clamp(scale, 1u, 64u);
    std::vector<PositionedGlyph> result;
    std::int64_t cursor_x = x;
    std::int64_t cursor_y = y;
    const std::int64_t line_height = std::int64_t{cell_height_} * scale;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\r') continue;
        if (ch == '\n') {
            cursor_x = x;
            cursor_y += line_height;
            continue;
        }
        if (ch == '\t') {
            cursor_x += std::int64_t{cell_width_} * scale * 4;
            continue;
        }
        const auto code = normalized(ch);
        const auto &metric = glyph(code);
        if (code != static_cast<std::uint32_t>(' ')) {
            const auto right = cursor_x + std::int64_t{metric.width} * scale;
            const auto bottom = cursor_y + std::int64_t{metric.height} * scale;
            if (cursor_x < INT32_MIN || cursor_y < INT32_MIN || right > INT32_MAX || bottom > INT32_MAX) {
                throw std::overflow_error("bitmap text layout exceeds int32");
            }
            result.push_back({code,
                              {static_cast<std::int32_t>(cursor_x), static_cast<std::int32_t>(cursor_y),
                               static_cast<std::int32_t>(right), static_cast<std::int32_t>(bottom)},
                              {metric.x, metric.y, metric.x + metric.width, metric.y + metric.height}});
        }
        cursor_x += std::int64_t{metric.advance} * scale;
    }
    return result;
}

PointI BitmapFont::measure(std::string_view text, std::uint32_t scale) const noexcept {
    scale = std::clamp(scale, 1u, 64u);
    std::int64_t line = 0;
    std::int64_t width = 0;
    std::int64_t lines = 1;
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\r') continue;
        if (ch == '\n') {
            width = std::max(width, line);
            line = 0;
            ++lines;
        } else if (ch == '\t') {
            line += std::int64_t{cell_width_} * scale * 4;
        } else {
            line += std::int64_t{glyph(normalized(ch)).advance} * scale;
        }
    }
    width = std::max(width, line);
    const auto height = text.empty() ? std::int64_t{0} : lines * cell_height_ * scale;
    return {static_cast<std::int32_t>(std::min<std::int64_t>(width, INT32_MAX)),
            static_cast<std::int32_t>(std::min<std::int64_t>(height, INT32_MAX))};
}

} // namespace Pelican::ui
