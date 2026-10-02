#include "playcontrols.hpp"

#include "studiotheme.hpp"

#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

Json parsed(const QByteArray &bytes) {
    return Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
}

QJsonObject toQt(const Json &object) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(object.dump())).object();
}

Json fromQt(const QJsonObject &object) {
    return parsed(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

bool textEntryHasFocus() {
    QWidget *focus = QApplication::focusWidget();
    if (focus == nullptr) return false;
    if (const auto *line = qobject_cast<QLineEdit *>(focus)) return !line->isReadOnly();
    if (const auto *text = qobject_cast<QTextEdit *>(focus)) return !text->isReadOnly();
    if (const auto *plain = qobject_cast<QPlainTextEdit *>(focus)) return !plain->isReadOnly();
    if (const auto *spin = qobject_cast<QAbstractSpinBox *>(focus)) return !spin->isReadOnly();
    if (const auto *combo = qobject_cast<QComboBox *>(focus)) return combo->isEditable();
    return false;
}

PlayControls::PlayControls(QToolBar &band, QWidget &window, QWidget &status_parent, Hooks hooks,
                           QObject *parent)
    : QObject{parent}, hooks_{std::move(hooks)} {
    separator_ = band.addSeparator();
    separator_->setObjectName(QStringLiteral("pelican.play.separator"));
    play_ = band.addAction(tr("▶ Play"));
    play_->setObjectName(QStringLiteral("pelican.play.play"));
    pause_ = band.addAction(tr("❚❚ Pause"));
    pause_->setObjectName(QStringLiteral("pelican.play.pause"));
    pause_->setCheckable(true);
    step_ = band.addAction(tr("▶| Step"));
    step_->setObjectName(QStringLiteral("pelican.play.step"));

    // 12.5: which camera the frame is drawn from, while paused or in edit.
    auto *view_label = new QLabel(tr("View:"), &band);
    view_label->setObjectName(QStringLiteral("pelican.play.viewLabel"));
    view_label->setContentsMargins(8, 0, 2, 0);
    view_label_ = band.addWidget(view_label);
    view_group_ = new QActionGroup(&band);
    view_group_->setExclusive(true);
    view_game_ = band.addAction(tr("Game"));
    view_game_->setObjectName(QStringLiteral("pelican.play.viewGame"));
    view_game_->setCheckable(true);
    view_game_->setToolTip(tr("Look through the game's camera"));
    view_group_->addAction(view_game_);
    view_editor_ = band.addAction(tr("Editor"));
    view_editor_->setObjectName(QStringLiteral("pelican.play.viewEditor"));
    view_editor_->setCheckable(true);
    view_editor_->setToolTip(
        tr("Look around the stopped game with the editor camera; the game's camera stays "
           "where it is"));
    view_group_->addAction(view_editor_);

    changes_ = band.addAction(tr("Play changes"));
    changes_->setObjectName(QStringLiteral("pelican.play.changes"));
    changes_->setToolTip(
        tr("The scene edits made while the game runs. Stop puts them back, except the ones "
           "marked Keep."));

    // The band's menu (12.6): what works in every state.
    auto *menu_button = new QToolButton(&band);
    menu_button->setObjectName(QStringLiteral("pelican.play.menuButton"));
    menu_button->setText(QStringLiteral("⋯"));
    menu_button->setToolTip(tr("More play commands"));
    menu_button->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(menu_button);
    menu->setObjectName(QStringLiteral("pelican.play.menu"));
    reload_ = menu->addAction(tr("Reload game logic"));
    reload_->setObjectName(QStringLiteral("pelican.play.reloadGameLogic"));
    menu_button->setMenu(menu);
    menu_button_ = band.addWidget(menu_button);

    // The keys are shortcuts of their own, not of the actions: a click on a
    // button has to work wherever the keyboard is, and a key must not while a
    // person is typing (12.1).
    const auto key = [&window](const char *sequence, const char *name) {
        auto *shortcut = new QShortcut(QKeySequence{QString::fromLatin1(sequence)}, &window);
        shortcut->setObjectName(QString::fromLatin1(name));
        shortcut->setContext(Qt::WindowShortcut);
        return shortcut;
    };
    play_key_ = key("Ctrl+P", "pelican.play.playKey");
    pause_key_ = key("Ctrl+Shift+P", "pelican.play.pauseKey");
    step_key_ = key("Ctrl+Alt+P", "pelican.play.stepKey");
    status_ = new QLabel(&status_parent);
    status_->setObjectName(QStringLiteral("pelican.playStatus"));
    // 12.1: play_busy. The line says why; this sends the same thing again once
    // Studio's edit or preview has finished.
    retry_ = new QPushButton(tr("Send when done"), &status_parent);
    retry_->setObjectName(QStringLiteral("pelican.play.retry"));
    retry_->setToolTip(
        tr("Wait for the edit or preview in flight to finish, then press the button again"));
    retry_->hide();
    follow_timer_ = new QTimer(this);
    follow_timer_->setSingleShot(true);
    follow_timer_->setInterval(FollowReadIntervalMs);
    retry_timer_ = new QTimer(this);
    retry_timer_->setInterval(RetryIntervalMs);
    createPanel(window);

    connect(play_, &QAction::triggered, this, [this] { press(&PlayStateModel::playOrStop); });
    connect(pause_, &QAction::triggered, this, [this] { press(&PlayStateModel::togglePause); });
    connect(step_, &QAction::triggered, this, [this] { press(&PlayStateModel::step); });
    connect(view_game_, &QAction::triggered, this, [this] {
        if (model_.setViewCamera(ViewCamera::game)) dispatch();
        else synchronize();
    });
    connect(view_editor_, &QAction::triggered, this, [this] {
        if (model_.setViewCamera(ViewCamera::editor)) dispatch();
        else synchronize();
    });
    connect(changes_, &QAction::triggered, this, [this] { showChanges(); });
    connect(reload_, &QAction::triggered, this,
            [this] { press(&PlayStateModel::reloadGameLogic); });
    connect(retry_, &QPushButton::clicked, this, [this] {
        model_.armRetry();
        synchronize();
    });
    const auto keyed = [this](QShortcut *shortcut, QAction *action) {
        connect(shortcut, &QShortcut::activated, this, [action] {
            if (textEntryHasFocus() || !action->isVisible() || !action->isEnabled()) return;
            action->trigger();
        });
    };
    keyed(play_key_, play_);
    keyed(pause_key_, pause_);
    keyed(step_key_, step_);
    connect(follow_timer_, &QTimer::timeout, this, [this] {
        if (model_.follow()) dispatch();
    });
    connect(retry_timer_, &QTimer::timeout, this, [this] {
        if (!model_.retryArmed()) {
            retry_timer_->stop();
            return;
        }
        if (hooks_.scene_idle && !hooks_.scene_idle()) return;
        if (model_.retryWhenIdle()) dispatch();
        else synchronize();
    });
    theme_listener_ = addStudioThemeListener([this] {
        if (hooks_.changed) hooks_.changed();
    });
    synchronize();
}

PlayControls::~PlayControls() { removeStudioThemeListener(theme_listener_); }

void PlayControls::createPanel(QWidget &window) {
    // 12.3: a small panel beside the window, not a modal question -- the game
    // goes on while it is open, and more edits land in it.
    panel_ = new QDialog(&window, Qt::Tool);
    panel_->setObjectName(QStringLiteral("pelican.playChanges"));
    panel_->setWindowTitle(tr("Play changes"));
    panel_->setModal(false);
    auto *layout = new QVBoxLayout(panel_);
    panel_summary_ = new QLabel(panel_);
    panel_summary_->setObjectName(QStringLiteral("pelican.playChanges.summary"));
    panel_summary_->setWordWrap(true);
    layout->addWidget(panel_summary_);
    auto *scroll = new QScrollArea(panel_);
    scroll->setWidgetResizable(true);
    scroll->setMinimumSize(320, 120);
    panel_rows_ = new QWidget(scroll);
    panel_rows_->setObjectName(QStringLiteral("pelican.playChanges.rows"));
    panel_rows_layout_ = new QVBoxLayout(panel_rows_);
    panel_rows_layout_->addStretch(1);
    scroll->setWidget(panel_rows_);
    layout->addWidget(scroll, 1);
    auto *buttons = new QHBoxLayout;
    keep_all_ = new QPushButton(tr("Keep all"), panel_);
    keep_all_->setObjectName(QStringLiteral("pelican.playChanges.keepAll"));
    keep_none_ = new QPushButton(tr("Keep none"), panel_);
    keep_none_->setObjectName(QStringLiteral("pelican.playChanges.keepNone"));
    buttons->addWidget(keep_all_);
    buttons->addWidget(keep_none_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    panel_not_kept_ = new QLabel(panel_);
    panel_not_kept_->setObjectName(QStringLiteral("pelican.playChanges.notKept"));
    panel_not_kept_->setWordWrap(true);
    panel_not_kept_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    panel_not_kept_->hide();
    layout->addWidget(panel_not_kept_);
    connect(keep_all_, &QPushButton::clicked, this, [this] {
        model_.keepAll();
        synchronize();
    });
    connect(keep_none_, &QPushButton::clicked, this, [this] {
        model_.keepNone();
        synchronize();
    });
}

void PlayControls::showChanges() {
    synchronizePanel();
    panel_->show();
    panel_->raise();
}

void PlayControls::engineAvailable() {
    model_.connect();
    dispatch();
}

void PlayControls::engineUnavailable() {
    follow_timer_->stop();
    retry_timer_->stop();
    transport_.clear();
    answered_.clear();
    refused_.clear();
    model_.disconnect();
    synchronize();
}

void PlayControls::observeStatus(const QByteArray &result_json) {
    if (model_.observeStatus(parsed(result_json))) synchronize();
}

void PlayControls::observeStatus(qint64 transport_id, const Json &status) {
    if (model_.observeStatus(status)) synchronize();
    const auto found = transport_.find(transport_id);
    if (found == transport_.end()) return;
    const auto id = found.value();
    transport_.erase(found);
    answered_.insert(transport_id);
    model_.receiveResult(id, status);
    dispatch();
}

bool PlayControls::receiveResult(qint64 transport_id, const QByteArray &result_json) {
    if (answered_.remove(transport_id)) return true;
    const auto found = transport_.find(transport_id);
    if (found == transport_.end()) return false;
    const auto id = found.value();
    transport_.erase(found);
    model_.receiveResult(id, parsed(result_json));
    dispatch();
    return true;
}

bool PlayControls::receiveRefusal(qint64 transport_id, int code, const QString &message,
                                  const QJsonValue &data) {
    if (!transport_.contains(transport_id)) return false;
    // The play refusals name themselves as data.code; F-5's (and the game
    // logic reload's) as error_code.
    QJsonObject error = data.toObject();
    if (!error.contains(QStringLiteral("code")) && error.contains(QStringLiteral("error_code")))
        error.insert(QStringLiteral("code"), error.value(QStringLiteral("error_code")));
    error.insert(QStringLiteral("message"), message);
    error.insert(QStringLiteral("rpc_code"), code);
    refused_.insert(transport_id, error);
    return true;
}

bool PlayControls::receiveFailure(qint64 transport_id, const QString &message) {
    const auto found = transport_.find(transport_id);
    if (found == transport_.end()) return false;
    const auto id = found.value();
    transport_.erase(found);
    if (const auto refusal = refused_.find(transport_id); refusal != refused_.end()) {
        model_.receiveRefusal(id, fromQt(refusal.value()));
        refused_.erase(refusal);
    } else {
        model_.receiveFailure(id, message.toStdString());
    }
    dispatch();
    return true;
}

void PlayControls::noteCommittedEdit(const CommittedEdit &edit) {
    model_.noteCommittedEdit(edit);
    dispatch();
}

void PlayControls::keep(const QString &transaction_id) {
    model_.setKeep(transaction_id.toStdString(), true);
    synchronize();
}

bool PlayControls::stopDiscarding() {
    model_.keepNone();
    if (!model_.stop()) {
        synchronize();
        return false;
    }
    dispatch();
    return true;
}

void PlayControls::press(bool (PlayStateModel::*action)()) {
    // A checkable action has already flipped itself; the model says what the
    // button shows, and synchronize() puts it back when nothing was sent.
    if ((model_.*action)()) dispatch();
    else synchronize();
}

void PlayControls::dispatch() {
    for (auto &request : model_.takeRequests()) {
        QString error;
        const qint64 transport_id =
            hooks_.send ? hooks_.send(QString::fromStdString(request.method),
                                      toQt(request.params), &error)
                        : 0;
        if (transport_id == 0) {
            model_.receiveFailure(request.request_id,
                                  error.isEmpty() ? std::string{"The engine is not ready."}
                                                  : error.toStdString());
            continue;
        }
        transport_.insert(transport_id, request.request_id);
    }
    synchronize();
}

void PlayControls::synchronize() {
    const bool shown = model_.supported();
    const bool editing = model_.editing();
    separator_->setVisible(shown);
    play_->setVisible(shown);
    pause_->setVisible(shown);
    step_->setVisible(shown);
    changes_->setVisible(shown);
    menu_button_->setVisible(shown);

    play_->setEnabled(model_.canPlayOrStop());
    play_->setText(editing || !shown ? tr("▶ Play") : tr("■ Stop"));
    play_->setToolTip(
        editing ? tr("Start the game from the scene as it is (Ctrl+P). Stop puts the scene "
                     "back as it was.")
                : tr("Stop the game and put the scene back as it was when Play was pressed; "
                     "the edits marked Keep in Play changes stay (Ctrl+P)"));
    pause_->setEnabled(model_.canTogglePause());
    pause_->setChecked(model_.paused());
    pause_->setToolTip(model_.paused()
                           ? tr("Resume the game (Ctrl+Shift+P)")
                           : editing ? tr("Press Play first; the game is not running")
                                     : tr("Pause the game: its clock, its code and its sound "
                                          "stop (Ctrl+Shift+P)"));
    step_->setEnabled(model_.canStep());
    step_->setToolTip(model_.paused()
                          ? tr("Advance the paused game by one frame (Ctrl+Alt+P)")
                          : editing ? tr("Start the game paused and advance it by one frame "
                                         "(Ctrl+Alt+P)")
                                    : tr("Pause the game to step it one frame at a time "
                                         "(Ctrl+Alt+P)"));

    const bool view = shown && model_.viewChoosable();
    view_label_->setVisible(view);
    view_game_->setVisible(view);
    view_editor_->setVisible(view);
    view_game_->setEnabled(model_.canSetView());
    view_editor_->setEnabled(model_.canSetView());
    if (const auto &status = model_.status(); status && status->view_camera) {
        view_game_->setChecked(*status->view_camera == ViewCamera::game);
        view_editor_->setChecked(*status->view_camera == ViewCamera::editor);
    }

    reload_->setEnabled(model_.canReload());
    reload_->setToolTip(editing ? tr("Load the game logic DLL again; the scene stays in edit")
                                : tr("Load the game logic DLL again and make the game again "
                                     "from the scene as it is now, edits of this run "
                                     "included; the run goes on"));
    const std::size_t rows = model_.status() ? model_.status()->recorded_edits.size() : 0;
    changes_->setText(rows > 0 ? tr("Play changes (%1)").arg(rows) : tr("Play changes"));

    status_->setText(QString::fromStdString(model_.statusText()));
    status_->setVisible(shown);

    const bool retry = model_.busyAction() != PlayStateModel::Action::None;
    retry_->setVisible(retry);
    retry_->setEnabled(retry && !model_.retryArmed());
    retry_->setText(model_.retryArmed() ? tr("Waiting to send...") : tr("Send when done"));
    if (model_.retryArmed() && !retry_timer_->isActive()) retry_timer_->start();

    if (model_.followWanted() && !follow_timer_->isActive()) follow_timer_->start();

    synchronizePanel();

    if (model_.noticeSerial() != shown_notice_serial_) {
        shown_notice_serial_ = model_.noticeSerial();
        const auto &notice = model_.notice();
        if (!notice.empty() && hooks_.notice) hooks_.notice(QString::fromStdString(notice));
    }
    if (model_.revision() != shown_revision_) {
        shown_revision_ = model_.revision();
        if (hooks_.changed) hooks_.changed();
    }
}

void PlayControls::synchronizePanel() {
    const auto rows = model_.changeRows();
    std::vector<std::string> ids;
    ids.reserve(rows.size());
    for (const auto &row : rows) ids.push_back(row.transaction_id);
    if (ids != row_ids_) {
        for (auto &box : row_boxes_)
            if (box) box->deleteLater();
        row_boxes_.clear();
        row_ids_ = ids;
        int index = 0;
        for (const auto &row : rows) {
            auto *box = new QCheckBox(panel_rows_);
            // Named by position, as a person reads the list top down.
            box->setObjectName(QStringLiteral("pelican.playChanges.row.%1").arg(index++));
            const QString id = QString::fromStdString(row.transaction_id);
            connect(box, &QCheckBox::toggled, this, [this, id](bool checked) {
                if (model_.kept(id.toStdString()) == checked) return;
                model_.setKeep(id.toStdString(), checked);
                synchronize();
            });
            // As wide as its words: a check box answers a click on its box
            // and its text only, and a row as wide as the panel would take
            // clicks it does not answer.
            panel_rows_layout_->insertWidget(panel_rows_layout_->count() - 1, box, 0,
                                             Qt::AlignLeft);
            row_boxes_.emplace_back(box);
        }
    }
    for (std::size_t index = 0; index < rows.size() && index < row_boxes_.size(); ++index) {
        QCheckBox *box = row_boxes_[index];
        if (box == nullptr) continue;
        const auto &row = rows[index];
        const QSignalBlocker quiet{box};
        box->setText(QString::fromStdString(row.text));
        box->setChecked(row.keep);
        box->setToolTip(row.studio ? tr("Kept after Stop when checked; put back when not")
                                   : tr("An edit another client made (actor %1)")
                                         .arg(row.actor));
    }
    const bool session = model_.session();
    keep_all_->setEnabled(session && !rows.empty());
    keep_none_->setEnabled(session && !rows.empty());

    QString summary;
    if (session) {
        summary = rows.empty() ? tr("No scene edits in this run yet. Scene edits made now go "
                                    "back on Stop unless they are marked Keep.")
                               : tr("%n scene edit(s) in this run. Stop puts them back, "
                                    "except the ones marked Keep.",
                                    nullptr, static_cast<int>(rows.size()));
    } else if (const auto &stopped = model_.lastStop()) {
        summary = tr("Stopped: %1 reverted, %2 kept").arg(stopped->reverted).arg(stopped->kept);
        if (!stopped->not_kept.empty())
            summary += tr(", %1 could not be kept").arg(stopped->not_kept.size());
        summary += QLatin1Char('.');
    } else {
        summary = tr("The game is not running. Press Play; the scene edits made while it runs "
                     "are listed here.");
    }
    panel_summary_->setText(summary);

    QStringList not_kept;
    if (const auto &stopped = model_.lastStop(); stopped && !session)
        for (const auto &row : stopped->not_kept)
            not_kept << tr("Not kept: %1 — %2")
                            .arg(QString::fromStdString(row.text),
                                 QString::fromStdString(row.reason));
    panel_not_kept_->setText(not_kept.join(QLatin1Char('\n')));
    panel_not_kept_->setVisible(!not_kept.isEmpty());
}

} // namespace PelicanStudio
