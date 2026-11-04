#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

class QSettings;

namespace PelicanStudio {

// The per-user OSC listener choices.  They are deliberately not part of a
// project: a project cannot switch a user's listener on.
struct StudioOscSettings {
    bool enabled = false;
    int port = 9000;
    QString interface_address = QStringLiteral("127.0.0.1");
    QString allow;

    friend bool operator==(const StudioOscSettings &, const StudioOscSettings &) = default;
};

// The three engine options, in the order the engine's parser expects them.
// The caller has already checked that the settings are valid.
QStringList studioOscPlayerArguments(const StudioOscSettings &settings);

// Empty when the settings can be passed to the engine.  Otherwise the one
// sentence the Live Inputs dock shows.
QString validateStudioOscSettings(const StudioOscSettings &settings);

// True for 127.0.0.0/8.  The dock uses this only for its warning.
bool studioOscInterfaceIsLoopback(const QString &address);

// True when the configured player arguments already carry any OSC option.  In
// that case none of the settings are passed: the environment wins as a whole.
bool studioOscOptionsFromEnvironment(const QStringList &arguments);
bool studioOscOptionsFromEnvironment(const std::vector<std::string> &arguments);

// Whether this Studio build can start an OSC listener (`PELICAN_WITH_OSC`).
bool studioBuildHasOsc();

// Reads and writes the `osc/...` keys of one explicitly chosen settings file.
// MainWindow owns that QSettings, so the dock and the viewport agree.
class StudioOscSettingsStore {
public:
    explicit StudioOscSettingsStore(QSettings &settings);

    StudioOscSettings load() const;

    // Saves the settings and checks QSettings::status() after sync().  On
    // failure `error` is the sentence for the dock and returns false.
    bool save(const StudioOscSettings &settings, QString *error);

private:
    QSettings &settings_;
};

} // namespace PelicanStudio
