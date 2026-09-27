#pragma once

#include "../model/playstatemodel.hpp"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

class QAction;
class QActionGroup;
class QCheckBox;
class QDialog;
class QLabel;
class QPushButton;
class QShortcut;
class QTimer;
class QToolBar;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace PelicanStudio {

// PM-3a / PM-3b (docs/design_play_mode.md 12.1 - 12.6). The play buttons on
// the band above the viewport -- ▶ Play / ■ Stop, ❚❚ Pause, ▶| Step, the
// View: Game | Editor choice, Play changes and the band's menu with Reload
// game logic --, their keys, the play line of the status bar, and the Play
// changes panel (the edits of the session and their Keep marks).
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
        // and the band, gates the Timeline, Save and the render panels.
        std::function<void()> changed;
        // A refusal, a failure or a Stop summary, as one line.
        std::function<void(const QString &line)> notice;
        // No scene edit or preview of Studio's is in flight: an action
        // play_busy turned back may be sent again.
        std::function<bool()> scene_idle;
    };

    // How long a follow-up read of a step waits after the previous reading.
    static constexpr int FollowReadIntervalMs = 40;
    // How often an action waiting for the scene to be idle looks again.
    static constexpr int RetryIntervalMs = 100;

    // The actions go on `band` (after a separator); the keys are shortcuts
    // of `window`, which also owns the Play changes panel; `status_parent`
    // owns the status line.
    PlayControls(QToolBar &band, QWidget &window, QWidget &status_parent,
                 Hooks hooks, QObject *parent = nullptr);
    ~PlayControls() override;

    void engineAvailable();
    // Stopped, restarting, switching projects: the next engine starts in
    // edit, and nothing this one said is shown for it.
    void engineUnavailable();

    // A get_status result, whoever asked.
    void observeStatus(const QByteArray &result_json);
    // The replies to what this sent. False for an id that is not its own.
    bool receiveResult(qint64 transport_id, const QByteArray &result_json);
    bool receiveRefusal(qint64 transport_id, int code, const QString &message,
                        const QJsonValue &data);
    bool receiveFailure(qint64 transport_id, const QString &message);

    // PM-3b. A scene edit Studio committed (its transaction, what it was).
    void noteCommittedEdit(const CommittedEdit &edit);
    // The Keep mark of one recorded edit (the Inspector's Keep link).
    void keep(const QString &transaction_id);
    // Shows the Play changes panel.
    void showChanges();
    // Stop with no edit kept (leaving the project, closing Studio). False
    // when it cannot be sent now.
    bool stopDiscarding();

    [[nodiscard]] QAction *playAction() const noexcept { return play_; }
    [[nodiscard]] QAction *pauseAction() const noexcept { return pause_; }
    [[nodiscard]] QAction *stepAction() const noexcept { return step_; }
    [[nodiscard]] QAction *viewGameAction() const noexcept { return view_game_; }
    [[nodiscard]] QAction *viewEditorAction() const noexcept { return view_editor_; }
    [[nodiscard]] QAction *changesAction() const noexcept { return changes_; }
    [[nodiscard]] QAction *reloadAction() const noexcept { return reload_; }
    [[nodiscard]] QPushButton *retryButton() const noexcept { return retry_; }
    [[nodiscard]] QLabel *statusLabel() const noexcept { return status_; }
    [[nodiscard]] QDialog *changesPanel() const noexcept { return panel_; }
    [[nodiscard]] const PlayStateModel &model() const noexcept { return model_; }

  private:
    Hooks hooks_;
    PlayStateModel model_;
    QAction *play_ = nullptr;
    QAction *pause_ = nullptr;
    QAction *step_ = nullptr;
    QAction *separator_ = nullptr;
    QAction *view_label_ = nullptr;
    QAction *view_game_ = nullptr;
    QAction *view_editor_ = nullptr;
    QActionGroup *view_group_ = nullptr;
    QAction *changes_ = nullptr;
    QAction *menu_button_ = nullptr;
    QAction *reload_ = nullptr;
    QShortcut *play_key_ = nullptr;
    QShortcut *pause_key_ = nullptr;
    QShortcut *step_key_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *retry_ = nullptr;
    QTimer *follow_timer_ = nullptr;
    QTimer *retry_timer_ = nullptr;
    // The Play changes panel.
    QDialog *panel_ = nullptr;
    QLabel *panel_summary_ = nullptr;
    QWidget *panel_rows_ = nullptr;
    QVBoxLayout *panel_rows_layout_ = nullptr;
    QLabel *panel_not_kept_ = nullptr;
    QPushButton *keep_all_ = nullptr;
    QPushButton *keep_none_ = nullptr;
    std::vector<QPointer<QCheckBox>> row_boxes_;
    std::vector<std::string> row_ids_;
    // transport id -> the model's request id.
    QHash<qint64, std::uint64_t> transport_;
    // A refusal's error, kept until the failure that follows it for the same
    // id finishes the request.
    QHash<qint64, QJsonObject> refused_;
    std::uint64_t shown_revision_ = 0;
    std::uint64_t shown_notice_serial_ = 0;
    int theme_listener_ = 0;

    void createPanel(QWidget &window);
    void press(bool (PlayStateModel::*action)());
    void dispatch();
    void synchronize();
    void synchronizePanel();
};

// True when the keyboard is in a place a person types text: a line edit, a
// text box, a spin box or an editable combo that is not read-only. The play
// keys do nothing there.
[[nodiscard]] bool textEntryHasFocus();

} // namespace PelicanStudio
