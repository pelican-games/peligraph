#include "studiotheme.hpp"

#include "battery/embed.hpp"

#include <QApplication>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString qtext(const std::string &value) {
    return QString::fromStdString(value);
}

constexpr auto ThemeSettingsKey = "studio/theme";

// The values the built-in dark theme keeps when the table says nothing. They
// are the colours the canvases shipped with, so a Studio whose resource fails
// to parse draws exactly what it drew before there was a table.
StudioCanvasColors builtInCanvas() {
    StudioCanvasColors canvas;
    canvas.background = QColor{QStringLiteral("#20262d")};
    canvas.grid = QColor{QStringLiteral("#2a313a")};
    canvas.edge = QColor{QStringLiteral("#8fa3b4")};
    canvas.edge_selected = QColor{QStringLiteral("#f2c14e")};
    canvas.node_body = QColor{QStringLiteral("#272e36")};
    canvas.node_border = QColor{QStringLiteral("#11161b")};
    canvas.heading_text = QColor{QStringLiteral("#f4f7fa")};
    canvas.dim_text = QColor{QStringLiteral("#cfdae4")};
    canvas.pill_bg = QColor{QStringLiteral("#3a444f")};
    return canvas;
}

StudioTheme builtInTheme() {
    StudioTheme theme;
    theme.name = QStringLiteral("dark");
    theme.label = QStringLiteral("Dark");
    theme.style = QStringLiteral("Fusion");
    theme.canvas = builtInCanvas();
    theme.play.playing_frame = QColor{QStringLiteral("#3d8fe0")};
    theme.play.playing_band = QColor{QStringLiteral("#1f3f60")};
    theme.play.paused_frame = QColor{QStringLiteral("#e0ac00")};
    theme.play.paused_band = QColor{QStringLiteral("#5e4b10")};
    theme.font.family = QStringLiteral("Segoe UI");
    theme.font.fallback = QStringLiteral("Yu Gothic UI");
    theme.font.size = 12.0;
    theme.font.size_small = 11.0;
    theme.domain_tint = {
        {QStringLiteral("values"), QColor{QStringLiteral("#3d6b4d")}},
        {QStringLiteral("rendering"), QColor{QStringLiteral("#3f5d86")}},
        {QStringLiteral("animation"), QColor{QStringLiteral("#7a5a34")}},
        {QStringLiteral("instance"), QColor{QStringLiteral("#7653a6")}},
        {QStringLiteral("input"), QColor{QStringLiteral("#2f6f7a")}},
        {QStringLiteral("structural"), QColor{QStringLiteral("#4a5560")}},
    };
    theme.source_tint = {
        {QStringLiteral("project"), QColor{QStringLiteral("#3978a8")}},
        {QStringLiteral("feature"), QColor{QStringLiteral("#c47b25")}},
        {QStringLiteral("engine"), QColor{QStringLiteral("#66717e")}},
        {QStringLiteral("unknown"), QColor{QStringLiteral("#806f68")}},
    };
    return theme;
}

// A colour the table spells in a way Qt cannot parse says nothing about
// colour; it must not black out the thing it names.
void readColor(const Json &entry, const char *key, QColor *out) {
    const auto found = entry.find(key);
    if (found == entry.end() || !found->is_string()) return;
    const QColor colour{qtext(found->get<std::string>())};
    if (colour.isValid()) *out = colour;
}

void readTints(const Json &entry, std::map<QString, QColor> *out) {
    if (!entry.is_object()) return;
    for (const auto &[key, value] : entry.items()) {
        if (!value.is_string()) continue;
        const QColor colour{qtext(value.get<std::string>())};
        if (colour.isValid()) (*out)[qtext(key)] = colour;
    }
}

// A size the table spells as something other than a positive number says
// nothing about size, and must not shrink the panel to nothing.
void readFonts(const Json &entry, StudioFonts *out) {
    if (!entry.is_object()) return;
    const auto string = [&entry](const char *key, QString *value) {
        const auto found = entry.find(key);
        if (found != entry.end() && found->is_string())
            *value = qtext(found->get<std::string>());
    };
    const auto size = [&entry](const char *key, double *value) {
        const auto found = entry.find(key);
        if (found == entry.end() || !found->is_number()) return;
        const double read = found->get<double>();
        if (read > 0.0) *value = read;
    };
    string("family", &out->family);
    string("fallback", &out->fallback);
    size("size", &out->size);
    size("size_small", &out->size_small);
}

void readPalette(const Json &entry, StudioTheme *theme) {
    if (!entry.is_object()) return;
    QPalette palette;
    const auto group = [&](const char *key, QPalette::ColorRole role) {
        QColor colour;
        readColor(entry, key, &colour);
        if (!colour.isValid()) return;
        palette.setColor(QPalette::All, role, colour);
    };
    group("window", QPalette::Window);
    group("window_text", QPalette::WindowText);
    group("base", QPalette::Base);
    group("alternate_base", QPalette::AlternateBase);
    group("text", QPalette::Text);
    group("button", QPalette::Button);
    group("button_text", QPalette::ButtonText);
    group("bright_text", QPalette::BrightText);
    group("highlight", QPalette::Highlight);
    group("highlighted_text", QPalette::HighlightedText);
    group("tool_tip_base", QPalette::ToolTipBase);
    group("tool_tip_text", QPalette::ToolTipText);
    group("placeholder_text", QPalette::PlaceholderText);
    group("link", QPalette::Link);
    QColor disabled;
    readColor(entry, "disabled_text", &disabled);
    if (disabled.isValid()) {
        for (const QPalette::ColorRole role :
             {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, disabled);
    }
    theme->palette = palette;
    theme->palette_declared = true;
}

StudioTheme themeFromJson(const QString &name, const Json &entry) {
    StudioTheme theme = builtInTheme();
    theme.name = name;
    theme.label = name;
    if (!entry.is_object()) return theme;
    if (const auto label = entry.find("label");
        label != entry.end() && label->is_string())
        theme.label = qtext(label->get<std::string>());
    if (const auto style = entry.find("style");
        style != entry.end() && style->is_string())
        theme.style = qtext(style->get<std::string>());
    readPalette(entry.value("palette", Json::object()), &theme);
    const Json canvas = entry.value("canvas", Json::object());
    if (canvas.is_object()) {
        readColor(canvas, "background", &theme.canvas.background);
        readColor(canvas, "grid", &theme.canvas.grid);
        readColor(canvas, "edge", &theme.canvas.edge);
        readColor(canvas, "edge_selected", &theme.canvas.edge_selected);
        readColor(canvas, "node_body", &theme.canvas.node_body);
        readColor(canvas, "node_border", &theme.canvas.node_border);
        readColor(canvas, "heading_text", &theme.canvas.heading_text);
        readColor(canvas, "dim_text", &theme.canvas.dim_text);
        readColor(canvas, "pill_bg", &theme.canvas.pill_bg);
    }
    readFonts(entry.value("font", Json::object()), &theme.font);
    readTints(entry.value("domain_tint", Json::object()), &theme.domain_tint);
    readTints(entry.value("source_tint", Json::object()), &theme.source_tint);
    if (const Json play = entry.value("play", Json::object()); play.is_object()) {
        readColor(play, "playing_frame", &theme.play.playing_frame);
        readColor(play, "playing_band", &theme.play.playing_band);
        readColor(play, "paused_frame", &theme.play.paused_frame);
        readColor(play, "paused_band", &theme.play.paused_band);
    }
    return theme;
}

struct ThemeTable {
    std::vector<StudioTheme> themes;
    QString default_name;
    QString current_name;
    std::size_t current_index = 0;
    bool loaded = false;
};

ThemeTable &table() {
    static ThemeTable state;
    return state;
}

struct Listener {
    int token = 0;
    std::function<void()> callback;
};

std::vector<Listener> &listeners() {
    static std::vector<Listener> registered;
    return registered;
}

void notifyListeners() {
    // A listener repopulates a scene, which must not be able to reach back in
    // and change the list while it is being walked.
    const std::vector<Listener> snapshot = listeners();
    for (const Listener &listener : snapshot)
        if (listener.callback) listener.callback();
}

void loadIfNeeded() {
    if (!table().loaded) resetStudioThemeTable();
}

const StudioTheme &fallbackTheme() {
    static const StudioTheme built_in = builtInTheme();
    return built_in;
}

} // namespace

QColor StudioTheme::domainTint(const QString &domain) const {
    const auto found = domain_tint.find(domain);
    if (found != domain_tint.end()) return found->second;
    const auto neutral = domain_tint.find(QStringLiteral("structural"));
    if (neutral != domain_tint.end()) return neutral->second;
    return canvas.node_body;
}

QColor StudioTheme::sourceTint(const QString &source) const {
    const auto found = source_tint.find(source);
    if (found != source_tint.end()) return found->second;
    const auto neutral = source_tint.find(QStringLiteral("unknown"));
    if (neutral != source_tint.end()) return neutral->second;
    return canvas.node_body;
}

void setStudioThemeTable(std::string_view json_text) {
    ThemeTable &state = table();
    const QString wanted =
        state.loaded ? state.current_name : QString{};
    state.loaded = true;
    state.themes.clear();
    state.default_name.clear();

    const Json parsed = Json::parse(json_text, nullptr, false);
    if (parsed.is_object()) {
        const auto themes = parsed.find("themes");
        if (themes != parsed.end() && themes->is_object())
            for (const auto &[name, entry] : themes->items())
                state.themes.push_back(themeFromJson(qtext(name), entry));
        if (const auto fallback = parsed.find("default");
            fallback != parsed.end() && fallback->is_string())
            state.default_name = qtext(fallback->get<std::string>());
    }
    // A table that names no theme still has to answer every drawing site, so
    // the built-in dark theme stands in. That is the control's ground truth.
    if (state.themes.empty()) state.themes.push_back(builtInTheme());
    const auto named = [&state](const QString &name) {
        return std::find_if(state.themes.begin(), state.themes.end(),
                            [&name](const StudioTheme &theme) {
                                return theme.name == name;
                            });
    };
    if (state.default_name.isEmpty() || named(state.default_name) == state.themes.end())
        state.default_name = state.themes.front().name;
    // Keep the theme the user is in when the table is replaced under them;
    // fall back to the table's default when it no longer exists.
    auto chosen = named(wanted);
    if (wanted.isEmpty() || chosen == state.themes.end())
        chosen = named(state.default_name);
    state.current_index =
        static_cast<std::size_t>(std::distance(state.themes.begin(), chosen));
    state.current_name = chosen->name;
    // Replacing the table under a running Studio changes the colours of the
    // theme it is already in, so the scenes have to be told. The very first
    // load is not a change: it happens inside the first colour lookup, and a
    // scene must not be rebuilt in the middle of drawing itself.
    if (!wanted.isEmpty()) notifyListeners();
}

void resetStudioThemeTable() {
    static const std::string shipped = b::embed<"studio_themes.json">().str();
    setStudioThemeTable(shipped);
}

const StudioTheme &currentStudioTheme() {
    loadIfNeeded();
    const ThemeTable &state = table();
    if (state.current_index >= state.themes.size()) return fallbackTheme();
    return state.themes[state.current_index];
}

namespace {

QFont fontOfSize(double pixels) {
    const StudioTheme &theme = currentStudioTheme();
    QFont font;
    QStringList families;
    if (!theme.font.family.isEmpty()) families.push_back(theme.font.family);
    if (!theme.font.fallback.isEmpty()) families.push_back(theme.font.fallback);
    if (!families.isEmpty()) font.setFamilies(families);
    if (pixels > 0.0) font.setPixelSize(static_cast<int>(std::lround(pixels)));
    return font;
}

} // namespace

QFont studioPanelFont() { return fontOfSize(currentStudioTheme().font.size); }

QFont studioSmallFont() {
    return fontOfSize(currentStudioTheme().font.size_small);
}

QColor studioDimTextColor() { return currentStudioTheme().canvas.dim_text; }

QString currentStudioThemeName() {
    loadIfNeeded();
    return table().current_name;
}

QStringList studioThemeNames() {
    loadIfNeeded();
    QStringList names;
    for (const StudioTheme &theme : table().themes) names.push_back(theme.name);
    return names;
}

QString defaultStudioThemeName() {
    loadIfNeeded();
    return table().default_name;
}

bool setCurrentStudioTheme(const QString &name) {
    loadIfNeeded();
    ThemeTable &state = table();
    for (std::size_t index = 0; index < state.themes.size(); ++index) {
        if (state.themes[index].name != name) continue;
        if (state.current_index == index && state.current_name == name)
            return true;
        state.current_index = index;
        state.current_name = name;
        notifyListeners();
        return true;
    }
    return false;
}

int addStudioThemeListener(std::function<void()> listener) {
    static int next_token = 0;
    ++next_token;
    listeners().push_back(Listener{next_token, std::move(listener)});
    return next_token;
}

void removeStudioThemeListener(int token) {
    std::vector<Listener> &registered = listeners();
    registered.erase(std::remove_if(registered.begin(), registered.end(),
                                    [token](const Listener &entry) {
                                        return entry.token == token;
                                    }),
                     registered.end());
}

void applyStudioThemeToApplication() {
    auto *application = qobject_cast<QApplication *>(QApplication::instance());
    if (application == nullptr) return;
    const StudioTheme &theme = currentStudioTheme();
    // The style the application started in, so a theme that names none can
    // hand the window back to the platform rather than to Fusion.
    static const QString original_style =
        application->style() != nullptr
            ? QString::fromLatin1(application->style()->metaObject()->className())
            : QString{};
    static const QPalette original_palette = application->palette();

    const QString wanted =
        theme.style.isEmpty() ? QString{} : theme.style;
    if (!wanted.isEmpty()) {
        if (QStyle *style = QStyleFactory::create(wanted))
            application->setStyle(style);
    } else if (!original_style.isEmpty()) {
        // QStyle class names are "QWindowsVistaStyle" for the "windowsvista"
        // key; the factory answers to the key, so ask it by the names it has.
        for (const QString &key : QStyleFactory::keys()) {
            if (!original_style.contains(key, Qt::CaseInsensitive)) continue;
            if (QStyle *style = QStyleFactory::create(key))
                application->setStyle(style);
            break;
        }
    }
    application->setPalette(theme.palette_declared ? theme.palette
                                                   : original_palette);
}

QString storedStudioThemeName() {
    QSettings settings;
    const QString stored =
        settings.value(QString::fromLatin1(ThemeSettingsKey)).toString();
    return stored;
}

void storeStudioThemeName(const QString &name) {
    QSettings settings;
    settings.setValue(QString::fromLatin1(ThemeSettingsKey), name);
    settings.sync();
}

} // namespace PelicanStudio
