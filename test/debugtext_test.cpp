#include "../src/core/container.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace Pelican {

namespace {

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

} // namespace

TEST_CASE("DebugText CPU API is a no-op before the feature registers a pass", "[debug-text]") {
    DebugText debug_text;

    debug_text.text(0, 0, "WP54");

    REQUIRE_FALSE(debug_text.isEnabledForTesting());
    REQUIRE(debug_text.queuedGlyphCountForTesting() == 0);
}

TEST_CASE("GameContext debugText is a no-op before the feature registers a pass", "[debug-text]") {
    ensureLogger();
    FastModuleContainer modules;
    GameContext context;

    context.debugText(-1024, -1024, "offscreen");

    auto &debug_text = GET_MODULE(DebugText);
    REQUIRE_FALSE(debug_text.isEnabledForTesting());
    REQUIRE(debug_text.queuedGlyphCountForTesting() == 0);
}

TEST_CASE("BitmapFont layout owns DebugText control, fallback, scale, and overflow rules",
          "[debug-text][bitmap-font][layout]") {
    const auto font = ui::BitmapFont::bundledDebugFont();
    std::string text{"A \r\n\t"};
    text.push_back('\x01');
    text.push_back('B');

    const auto glyphs = font.layout(10, 20, text, 2);
    REQUIRE(glyphs.size() == 3);

    const auto &a = font.glyph('A');
    REQUIRE(glyphs[0].code == static_cast<std::uint32_t>('A'));
    REQUIRE(glyphs[0].destination ==
            ui::RectI{10, 20, 10 + a.width * 2, 20 + a.height * 2});

    const auto fallback_x = 10 + static_cast<std::int32_t>(font.cellWidth() * 8);
    const auto second_line_y = 20 + static_cast<std::int32_t>(font.cellHeight() * 2);
    const auto &fallback = font.glyph(ui::BitmapFont::fallbackCode);
    REQUIRE(glyphs[1].code == ui::BitmapFont::fallbackCode);
    REQUIRE(glyphs[1].destination ==
            ui::RectI{fallback_x, second_line_y,
                      fallback_x + fallback.width * 2,
                      second_line_y + fallback.height * 2});

    const auto &b = font.glyph('B');
    const auto b_x = fallback_x + fallback.advance * 2;
    REQUIRE(glyphs[2].destination ==
            ui::RectI{b_x, second_line_y, b_x + b.width * 2,
                      second_line_y + b.height * 2});

    REQUIRE_THROWS_AS(font.layout(std::numeric_limits<std::int32_t>::max(), 0, "A", 64),
                      std::overflow_error);
}

} // namespace Pelican
