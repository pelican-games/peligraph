#pragma once

#include "types.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Pelican::ui {

struct BitmapGlyph {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t advance = 0;
    auto operator<=>(const BitmapGlyph &) const = default;
};

struct PositionedGlyph {
    std::uint32_t code = 0;
    RectI destination{};
    RectI source{};
    auto operator<=>(const PositionedGlyph &) const = default;
};

class BitmapFont {
  public:
    static constexpr std::uint32_t firstCode = 32;
    static constexpr std::uint32_t lastCode = 126;
    static constexpr std::uint32_t fallbackCode = '?';

    static BitmapFont fromJson(std::string_view json);
    static BitmapFont bundledDebugFont();

    const BitmapGlyph &glyph(std::uint32_t code) const noexcept;
    std::vector<PositionedGlyph> layout(std::int32_t x, std::int32_t y,
                                        std::string_view text, std::uint32_t scale = 1) const;
    PointI measure(std::string_view text, std::uint32_t scale = 1) const noexcept;

    std::uint32_t atlasWidth() const noexcept { return atlas_width_; }
    std::uint32_t atlasHeight() const noexcept { return atlas_height_; }
    std::uint32_t cellWidth() const noexcept { return cell_width_; }
    std::uint32_t cellHeight() const noexcept { return cell_height_; }

  private:
    std::uint32_t atlas_width_ = 0;
    std::uint32_t atlas_height_ = 0;
    std::uint32_t cell_width_ = 0;
    std::uint32_t cell_height_ = 0;
    std::array<BitmapGlyph, lastCode - firstCode + 1> glyphs_{};
};

} // namespace Pelican::ui
