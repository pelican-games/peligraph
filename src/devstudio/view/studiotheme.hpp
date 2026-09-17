#pragma once

#include <QColor>
#include <QPalette>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <string_view>

namespace PelicanStudio {

// What every node canvas in Studio draws with. One table, so the Frame Plan
// and the common graph cannot drift apart, and so a theme is a data change
// rather than a sweep through two thousand lines of drawing code.
struct StudioCanvasColors {
    QColor background;
    QColor grid;
    QColor edge;
    QColor edge_selected;
    QColor node_body;
    QColor node_border;
    QColor heading_text;
    QColor dim_text;
    QColor pill_bg;
};

// One theme. `style` and `palette` are the window chrome; a theme that
// declares no palette keeps Qt's own, which is what the light theme wants.
struct StudioTheme {
    QString name;
    QString label;
    QString style;
    bool palette_declared = false;
    QPalette palette;
    StudioCanvasColors canvas;
    // The heading band of a common-graph node, by the operation's domain.
    std::map<QString, QColor> domain_tint;
    // The heading band of a Frame Plan node, by where the resource came from.
    std::map<QString, QColor> source_tint;

    // A domain or a source the table never named is not a reason to draw
    // black: both fall through to the entry the table keeps for that case.
    [[nodiscard]] QColor domainTint(const QString &domain) const;
    [[nodiscard]] QColor sourceTint(const QString &source) const;
};

// The theme every drawing site reads. Never null: a malformed table leaves
// the built-in dark values, which is exactly the control.
const StudioTheme &currentStudioTheme();
QString currentStudioThemeName();
QStringList studioThemeNames();
QString defaultStudioThemeName();

// Switch. Returns false when the table has no such theme, and then changes
// nothing. A real change notifies the listeners below.
bool setCurrentStudioTheme(const QString &name);

// Replacing the table is how a test runs its control.
void setStudioThemeTable(std::string_view json_text);
void resetStudioThemeTable();

// A scene repopulates when the theme changes: its items hold the colours they
// were built with, so redrawing is the only way they follow the table. Qt
// signals would need moc in every target that compiles a view source, and the
// scene tests compile them without it, so the notification is a plain list.
int addStudioThemeListener(std::function<void()> listener);
void removeStudioThemeListener(int token);

// Put the current theme's style and palette on the running application.
// Studio calls this; a scene test does not, so a test keeps Qt's own palette
// while still reading the table's canvas colours.
void applyStudioThemeToApplication();

// The stored preference. Panel layouts are already "a person's taste" rather
// than project data, and the theme is filed the same way.
QString storedStudioThemeName();
void storeStudioThemeName(const QString &name);

} // namespace PelicanStudio
