#include "../src/devstudio/view/commandpalettematch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

using PelicanStudio::CommandPaletteRow;
using PelicanStudio::CommandMatchTier;

CommandPaletteRow row(const char *title, bool enabled = true) {
    return CommandPaletteRow{.title = QString::fromLatin1(title), .enabled = enabled};
}

std::vector<CommandPaletteRow> sampleTable() {
    return {row("Alpha scene"), row("Scene beta"), row("Ascend"), row("Second"),
            row("Zoo")};
}

std::vector<std::string> titles(const std::vector<CommandPaletteRow> &rows) {
    std::vector<std::string> result;
    for (const auto &row : rows) result.push_back(row.title.toStdString());
    return result;
}

} // namespace

TEST_CASE("KM-3 matching ranks prefixes, word starts and subsequences", "[km3]") {
    const auto table = sampleTable();
    const std::vector<std::string> expected{"Scene beta", "Alpha scene", "Ascend", "Second"};
    CHECK(titles(PelicanStudio::filterCommandRows(QStringLiteral("sc"), table)) == expected);
    CHECK(titles(PelicanStudio::filterCommandRows(QStringLiteral("SC"), table)) == expected);
    CHECK(PelicanStudio::filterCommandRows(QStringLiteral("cs"), table).empty());

    for (const QString query : {QString{}, QStringLiteral("  \t")}) {
        CHECK(titles(PelicanStudio::filterCommandRows(query, table)) == titles(table));
    }

    const auto two = std::vector<CommandPaletteRow>{row("Scene one"), row("Scene two")};
    CHECK(titles(PelicanStudio::filterCommandRows(QStringLiteral("scene"), two)) ==
          titles(two));
}

TEST_CASE("KM-3 word starts and disabled ranks follow the command text", "[km3]") {
    const QString title = QStringLiteral("Play / Stop (outside text fields)");
    CHECK(PelicanStudio::commandMatchTier(QStringLiteral("play"), title) ==
          CommandMatchTier::Prefix);
    for (const QString query : {QStringLiteral("text"), QStringLiteral("stop"),
                                QStringLiteral("outside")}) {
        CHECK(PelicanStudio::commandMatchTier(query, title) == CommandMatchTier::WordStart);
    }

    auto table = sampleTable();
    table[0].enabled = false;
    table[2].enabled = false;
    const auto filtered = PelicanStudio::filterCommandRows(QStringLiteral("sc"), table);
    REQUIRE(filtered.size() == 4);
    CHECK(filtered[1].title == QStringLiteral("Alpha scene"));
    CHECK_FALSE(filtered[1].enabled);
    CHECK(filtered[2].title == QStringLiteral("Ascend"));
    CHECK_FALSE(filtered[2].enabled);
}

TEST_CASE("KM-3 enabled-row navigation skips and stops without wrapping", "[km3]") {
    auto rows = std::vector<CommandPaletteRow>{row("Zero", false), row("One"),
                                               row("Two", false), row("Three"), row("Four")};
    CHECK(PelicanStudio::firstEnabledRow(rows) == 1);
    CHECK(PelicanStudio::nextEnabledRow(rows, 1, 1) == 3);
    CHECK(PelicanStudio::nextEnabledRow(rows, 3, 1) == 4);
    CHECK(PelicanStudio::nextEnabledRow(rows, 4, 1) == 4);
    CHECK(PelicanStudio::nextEnabledRow(rows, 4, -1) == 3);
    CHECK(PelicanStudio::nextEnabledRow(rows, 3, -1) == 1);
    CHECK(PelicanStudio::nextEnabledRow(rows, 1, -1) == 1);

    for (auto &row : rows) row.enabled = false;
    CHECK(PelicanStudio::firstEnabledRow(rows) == -1);
    CHECK(PelicanStudio::nextEnabledRow(rows, 2, 1) == 2);
    CHECK(PelicanStudio::nextEnabledRow(rows, 2, -1) == 2);
}

TEST_CASE("KM-3 every match string contributes the best tier", "[km3]") {
    auto command = row("Save Scene (stop Play first)");
    command.matches = {command.title, QStringLiteral("File › Save Scene")};
    const auto paths = std::vector<CommandPaletteRow>{command, row(" unrelated ")};
    const auto filtered = PelicanStudio::filterCommandRows(QStringLiteral("file save"), paths);
    REQUIRE(filtered.size() == 1);
    CHECK(filtered.front().title == command.title);
}
