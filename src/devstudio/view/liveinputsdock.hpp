#pragma once

#include "panelshown.hpp"
#include "studiooscsettings.hpp"

#include <QWidget>

#include <QJsonObject>

#include <functional>
#include <memory>

class QByteArray;
class QHideEvent;
class QSettings;
class QShowEvent;

namespace PelicanStudio {

// OSC-3c: the user's live inputs.  The table and the listener line are read
// from the running engine; the settings are the user's own, not the project's,
// and applying them restarts the engine through MainWindow.
class LiveInputsDock final : public QWidget {
    Q_OBJECT

  public:
    struct Hooks {
        // Sends get_live_inputs; returns 0 when nothing could be sent.
        std::function<qint64(const QString &method, const QJsonObject &params,
                             QString *error)>
            send;
        // The id of the engine this Studio started, to drop a reply from the
        // previous one. Empty in tests that do not start engines.
        std::function<qint64()> engine_identity;
        std::function<void(const QString &reason)> restart;
        // Called after a successful save, before the restart, so the viewport
        // that is about to start the engine already has the new settings.
        std::function<void(const StudioOscSettings &settings)> applied;
        // PELICAN_STUDIO_PLAYER_ARGUMENTS, split, as the environment sees it.
        std::function<QStringList()> configured_arguments;
        // Tests inject the clock; production uses one QElapsedTimer.
        std::function<qint64()> clock_ms;
    };

    LiveInputsDock(Hooks hooks, QSettings &settings, QWidget *parent = nullptr);
    ~LiveInputsDock() override;

    // The request id is 0 / does not belong to this dock: returns false so the
    // caller can offer it to the next panel.  A reply for this dock whose
    // engine generation is stale is consumed and dropped.
    bool receiveResult(qint64 request_id, const QByteArray &result);
    bool receiveFailure(qint64 request_id, const QString &message);

    void engineAvailable();
    void engineUnavailable();
    void engineRestartDeclined(const QString &message);

    // Reads now, but only while this panel is actually shown.
    void readNow();

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
