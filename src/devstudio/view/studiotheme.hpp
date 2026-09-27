#pragma once

#include <QColor>
#include <QFont>
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
    // GL-1: a node the canvas draws for what no graph node made (a
    // feature, a pass of the rendering section) -- read only, so its frame
    // is this colour, dashed, around a body a step dimmer than a node's.
    QColor derived_border;
    QColor derived_body;
};

// What every Studio panel sets its text in. One table, so two panels cannot
// drift into two sizes, and so "make the text smaller" is a data change.
// `size` is the panel's own text and `size_small` its secondary rows; both
// are pixels, which is what the platform default (9 pt at 96 dpi) already is.
struct StudioFonts {
    QString family;
    // The face that carries Japanese when `family` does not.
    QString fallback;
    double size = 12.0;
    double size_small = 11.0;
};

// PM-3a (design_play_mode 12.2): what the viewport's frame and the band above
// it turn while the game runs (PM-3b: playing, bluish) or is paused (PM-3a:
// yellowish). Edit keeps the window's own colours.
struct StudioPlayColors {
    QColor playing_frame;
    QColor playing_band;
    QColor paused_frame;
    QColor paused_band;
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
    StudioFonts font;
    // The heading band of a common-graph node, by the operation's domain.
    std::map<QString, QColor> domain_tint;
    // The heading band of a Frame Plan node, by where the resource came from.
    std::map<QString, QColor> source_tint;
    StudioPlayColors play;

    // A domain or a source the table never named is not a reason to draw
    // black: both fall through to the entry the table keeps for that case.
    [[nodiscard]] QColor domainTint(const QString &domain) const;
    [[nodiscard]] QColor sourceTint(const QString &source) const;
};

// The theme every drawing site reads. Never null: a malformed table leaves
// the built-in dark values, which is exactly the control.
const StudioTheme &currentStudioTheme();

// The two fonts a panel is allowed to use, and the colour its secondary text
// is drawn in. Panels read these instead of naming a face or a size, so the
// table above is the only place a size exists.
QFont studioPanelFont();
QFont studioSmallFont();
QColor studioDimTextColor();
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
