#pragma once

#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <vector>

namespace PelicanStudio {

struct CommandPaletteRow {
    QString token;
    QString title;
    QString keys;
    QStringList matches;
    bool enabled = false;
    bool checkable = false;
    bool checked = false;
};

enum class CommandMatchTier { Prefix = 0, WordStart = 1, Subsequence = 2 };

inline std::optional<CommandMatchTier> commandMatchTier(const QString &query,
                                                        const QString &title) {
    const QString folded_query = query.trimmed().toCaseFolded();
    const QString folded_title = title.toCaseFolded();
    if (folded_title.startsWith(folded_query)) return CommandMatchTier::Prefix;

    qsizetype from = 0;
    while (from <= folded_title.size() - folded_query.size()) {
        const qsizetype at = folded_title.indexOf(folded_query, from);
        if (at < 0) break;
        if (at == 0 || !folded_title.at(at - 1).isLetterOrNumber())
            return CommandMatchTier::WordStart;
        from = at + 1;
    }

    qsizetype at = 0;
    for (const QChar character : folded_query) {
        at = folded_title.indexOf(character, at);
        if (at < 0) return std::nullopt;
        ++at;
    }
    return CommandMatchTier::Subsequence;
}

inline std::optional<CommandMatchTier> commandRowMatchTier(
    const QString &query, const CommandPaletteRow &row) {
    std::optional<CommandMatchTier> best;
    const QStringList matches = row.matches.isEmpty()
                                    ? QStringList{row.title}
                                    : row.matches;
    for (const QString &match : matches) {
        const auto tier = commandMatchTier(query, match);
        if (!tier) continue;
        if (!best || static_cast<int>(*tier) < static_cast<int>(*best)) best = tier;
    }
    return best;
}

inline std::vector<CommandPaletteRow> filterCommandRows(
    const QString &query, const std::vector<CommandPaletteRow> &table) {
    std::vector<std::pair<CommandMatchTier, CommandPaletteRow>> matches;
    for (const CommandPaletteRow &row : table) {
        const auto tier = commandRowMatchTier(query, row);
        if (tier) matches.emplace_back(*tier, row);
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto &left, const auto &right) {
        return static_cast<int>(left.first) < static_cast<int>(right.first);
    });
    std::vector<CommandPaletteRow> result;
    result.reserve(matches.size());
    for (auto &match : matches) result.push_back(std::move(match.second));
    return result;
}

inline int firstEnabledRow(const std::vector<CommandPaletteRow> &rows) {
    for (std::size_t index = 0; index < rows.size(); ++index)
        if (rows[index].enabled) return static_cast<int>(index);
    return -1;
}

inline int nextEnabledRow(const std::vector<CommandPaletteRow> &rows, int current,
                          int step) {
    for (int index = current + step; index >= 0 && index < static_cast<int>(rows.size());
         index += step) {
        if (rows[static_cast<std::size_t>(index)].enabled) return index;
    }
    return current;
}

} // namespace PelicanStudio
