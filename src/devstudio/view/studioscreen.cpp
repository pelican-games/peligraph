#include "studioscreen.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <algorithm>
#include <iostream>

namespace PelicanStudio {

std::optional<int> studioScreenIndex(const QString &requested,
                                     const QStringList &screen_names) {
    const QString value = requested.trimmed();
    if (value.isEmpty()) return std::nullopt;
    bool number = false;
    const int index = value.toInt(&number);
    if (number && index >= 0 && index < screen_names.size()) return index;
    const auto found = std::ranges::find(screen_names, value);
    if (found != screen_names.end())
        return static_cast<int>(std::distance(screen_names.begin(), found));
    return std::nullopt;
}

QRect studioScreenPlacement(const QRect &frame, const QRect &available) {
    return {available.topLeft(),
            QSize{std::min(frame.width(), available.width()),
                  std::min(frame.height(), available.height())}};
}

void placeOnRequestedScreen(QWidget &window) {
    const QString requested = qEnvironmentVariable("PELICAN_STUDIO_SCREEN");
    if (requested.isEmpty()) return;
    const QList<QScreen *> screens = QGuiApplication::screens();
    QStringList names;
    for (const QScreen *screen : screens) names << screen->name();
    const auto index = studioScreenIndex(requested, names);
    if (!index) {
        std::cerr << "PELICAN_STUDIO_SCREEN names no screen: "
                  << requested.toStdString()
                  << "; screens: " << names.join(QStringLiteral(", ")).toStdString()
                  << std::endl;
        return;
    }
    // The native window exists before it is shown so the frame (title bar
    // and borders) is known: the frame, not only the client area, has to fit.
    (void)window.winId();
    // A maximized window is placed by its normal geometry, which is where
    // Windows maximizes it.
    const Qt::WindowStates state = window.windowState();
    const Qt::WindowStates enlarged = state & (Qt::WindowMaximized | Qt::WindowFullScreen);
    if (enlarged) window.setWindowState(state & ~enlarged);
    const QRect frame = window.frameGeometry();
    const QRect client = window.geometry();
    const QRect placed =
        studioScreenPlacement(frame, screens[*index]->availableGeometry());
    if (placed.size() != frame.size())
        window.resize(client.width() - (frame.width() - placed.width()),
                      client.height() - (frame.height() - placed.height()));
    window.move(placed.topLeft());
    if (enlarged) window.setWindowState(window.windowState() | enlarged);
}

} // namespace PelicanStudio
