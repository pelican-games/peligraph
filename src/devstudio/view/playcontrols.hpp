#pragma once

#include "../model/playstatemodel.hpp"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

class QAction;
class QLabel;
class QShortcut;
class QTimer;
class QToolBar;
class QWidget;

namespace PelicanStudio {

// PM-3a (docs/design_play_mode.md 12.1, 12.2). The ❚❚ Pause and ▶| Step
// buttons on the band above the viewport, their keys, and the play line of
// the status bar. Resume is Pause released; Play and Stop come with PM-3b.
//
// The buttons exist only while the engine reports `play` in get_status: an
// engine from before PM-1a gets none. The state moves with every get_status
// reply the viewport receives (observeStatus) and with the answers to what
// these buttons send; nothing here reads on a clock while nothing changes.
class PlayControls final : public QObject {
  public:
    struct Hooks {
        // Sends one editor request; the id its reply comes back under, or 0
        // with `error` saying why.
        std::function<qint64(const QString &method, const QJsonObject &params,
                             QString *error)>
            send;
        // Anything shown may read differently: the owner re-tints the frame
        // and the band, and gates the Timeline.
        std::function<void()> changed;
        // A refusal or a failure, as one line.
        std::function<void(const QString &line)> notice;
    };

    // How long a follow-up read of a step waits after the previous reading.
    static constexpr int FollowReadIntervalMs = 40;

    // The two actions go on `band` (after a separator); the keys are
    // shortcuts of `window`; `status_parent` owns the status line.
    PlayControls(QToolBar &band, QWidget &window, QWidget &status_parent,
                 Hooks hooks, QObject *parent = nullptr);
    ~PlayControls() override;

    void engineAvailable();
    // Stopped, restarting, switching projects: the next engine starts playing,
    // and nothing this one said is shown for it.
    void engineUnavailable();

    // A get_status result, whoever asked.
    void observeStatus(const QByteArray &result_json);
    // The replies to what this sent. False for an id that is not its own.
    bool receiveResult(qint64 transport_id, const QByteArray &result_json);
    bool receiveRefusal(qint64 transport_id, int code, const QString &message,
                        const QJsonValue &data);
    bool receiveFailure(qint64 transport_id, const QString &message);

    [[nodiscard]] QAction *pauseAction() const noexcept { return pause_; }
    [[nodiscard]] QAction *stepAction() const noexcept { return step_; }
    [[nodiscard]] QLabel *statusLabel() const noexcept { return status_; }
    [[nodiscard]] const PlayStateModel &model() const noexcept { return model_; }

  private:
    Hooks hooks_;
    PlayStateModel model_;
    QAction *pause_ = nullptr;
    QAction *step_ = nullptr;
    QAction *separator_ = nullptr;
    QShortcut *pause_key_ = nullptr;
    QShortcut *step_key_ = nullptr;
    QLabel *status_ = nullptr;
    QTimer *follow_timer_ = nullptr;
    // transport id -> the model's request id.
    QHash<qint64, std::uint64_t> transport_;
    // A refusal's error, kept until the failure that follows it for the same
    // id finishes the request.
    QHash<qint64, QJsonObject> refused_;
    std::uint64_t shown_revision_ = 0;
    std::uint64_t shown_notice_serial_ = 0;
    int theme_listener_ = 0;

    void pressPause();
    void pressStep();
    void dispatch();
    void synchronize();
};

// True when the keyboard is in a place a person types text: a line edit, a
// text box, a spin box or an editable combo that is not read-only. The play
// keys do nothing there.
[[nodiscard]] bool textEntryHasFocus();

} // namespace PelicanStudio
