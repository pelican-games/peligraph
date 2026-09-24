#pragma once

// Q-6: how the Add component dialog shows a candidate this build cannot take.
// The rows come from componentAddRows (model); this is the part that puts
// them on Qt widgets, so a test can hold it without an engine behind it.

#include "../model/inspectormodel.hpp"

#include <Qt>

#include <optional>
#include <vector>

class QComboBox;
class QPlainTextEdit;
class QPushButton;

namespace PelicanStudio {

// Item data beside the key (Qt::UserRole, what currentData() answers).
inline constexpr int ComponentAddCandidateRole = Qt::UserRole + 1;
inline constexpr int ComponentAddAddableRole = Qt::UserRole + 2;
inline constexpr int ComponentAddBlockedRole = Qt::UserRole + 3;

// Replaces the list with the rows, in their order. A row that cannot be
// added is drawn in the disabled text colour but stays selectable, so its
// reason can be read.
void fillComponentAddTypes(QComboBox &types, const std::vector<ComponentAddRow> &rows);

// The row under the current index, as fillComponentAddTypes stored it.
[[nodiscard]] std::optional<ComponentAddRow> currentComponentAddRow(const QComboBox &types);

// Add and the declaration for the current row: both off when the row cannot
// be added, whatever `ready` says. Answers whether the row can be added.
bool applyComponentAddAvailability(const QComboBox &types, bool ready, QPushButton &add,
                                   QPlainTextEdit &declaration);

} // namespace PelicanStudio
