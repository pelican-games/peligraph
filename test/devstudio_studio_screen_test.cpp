#include "studioscreen.hpp"

#include <catch2/catch_test_macros.hpp>

using PelicanStudio::studioScreenIndex;
using PelicanStudio::studioScreenPlacement;

// OT-1, decision 1: PELICAN_STUDIO_SCREEN is an index into the screen list or
// a screen's own name; anything else names no screen. The window keeps its
// size at that screen's top-left, shrunk only when it does not fit.
TEST_CASE("OT-1 PELICAN_STUDIO_SCREEN picks a screen by index or by name and keeps "
          "the window's size at its top-left",
          "[studio][studio-screen]") {
    const QStringList screens{QStringLiteral("\\\\.\\DISPLAY1"),
                              QStringLiteral("\\\\.\\DISPLAY5")};
    SECTION("an index counted from 0") {
        CHECK(studioScreenIndex(QStringLiteral("0"), screens) == 0);
        CHECK(studioScreenIndex(QStringLiteral("1"), screens) == 1);
        CHECK(studioScreenIndex(QStringLiteral(" 1 "), screens) == 1);
    }
    SECTION("a screen's name") {
        CHECK(studioScreenIndex(QStringLiteral("\\\\.\\DISPLAY5"), screens) == 1);
        CHECK(studioScreenIndex(QStringLiteral("\\\\.\\DISPLAY1"), screens) == 0);
    }
    SECTION("neither names no screen") {
        CHECK_FALSE(studioScreenIndex(QStringLiteral("2"), screens).has_value());
        CHECK_FALSE(studioScreenIndex(QStringLiteral("-1"), screens).has_value());
        CHECK_FALSE(studioScreenIndex(QStringLiteral("DISPLAY5"), screens).has_value());
        CHECK_FALSE(studioScreenIndex(QString{}, screens).has_value());
        CHECK_FALSE(studioScreenIndex(QStringLiteral("0"), QStringList{}).has_value());
    }
    const QRect available{2560, 0, 1920, 1040};
    SECTION("a frame that fits keeps its size") {
        CHECK(studioScreenPlacement(QRect{100, 200, 1296, 839}, available) ==
              QRect{2560, 0, 1296, 839});
    }
    SECTION("a frame larger than the area is shrunk to it") {
        CHECK(studioScreenPlacement(QRect{-8, -8, 2576, 1416}, available) ==
              QRect{2560, 0, 1920, 1040});
        CHECK(studioScreenPlacement(QRect{0, 0, 1000, 2000}, available) ==
              QRect{2560, 0, 1000, 1040});
    }
}
