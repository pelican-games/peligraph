#pragma once

#include <QRect>
#include <QString>
#include <QStringList>
#include <optional>

class QWidget;

namespace PelicanStudio {

// OT-1: PELICAN_STUDIO_SCREEN puts the main window on one screen at startup,
// so a run that drives Studio (the automation scenarios) can keep it off the
// screen a person is working on.

// The screen `requested` names among `screen_names` (QGuiApplication's order):
// an index counted from 0, or a screen's own name (QScreen::name(), such as
// "\\.\DISPLAY5"). An index is read first; nullopt when neither matches.
[[nodiscard]] std::optional<int> studioScreenIndex(
    const QString &requested, const QStringList &screen_names);

// Where a window whose frame is `frame` goes on a screen whose available area
// is `available`: its top-left corner, at the same size. A frame larger than
// the area is shrunk to it (never grown).
[[nodiscard]] QRect studioScreenPlacement(const QRect &frame,
                                          const QRect &available);

// Reads PELICAN_STUDIO_SCREEN and moves `window` (not yet shown, its saved
// geometry already restored) to that screen. Unset: nothing happens. A value
// that names no screen is one line on stderr, and the window stays where it
// was.
void placeOnRequestedScreen(QWidget &window);

} // namespace PelicanStudio
