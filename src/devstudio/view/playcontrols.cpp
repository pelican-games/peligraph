#include "playcontrols.hpp"

#include "studiotheme.hpp"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
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
    pause_ = band.addAction(tr("❚❚ Pause"));
    pause_->setObjectName(QStringLiteral("pelican.play.pause"));
    pause_->setCheckable(true);
    step_ = band.addAction(tr("▶| Step"));
    step_->setObjectName(QStringLiteral("pelican.play.step"));
    // The keys are shortcuts of their own, not of the actions: a click on a
    // button has to work wherever the keyboard is, and a key must not while a
    // person is typing (12.1).
    pause_key_ = new QShortcut(QKeySequence{QStringLiteral("Ctrl+Shift+P")}, &window);
    pause_key_->setObjectName(QStringLiteral("pelican.play.pauseKey"));
    pause_key_->setContext(Qt::WindowShortcut);
    step_key_ = new QShortcut(QKeySequence{QStringLiteral("Ctrl+Alt+P")}, &window);
    step_key_->setObjectName(QStringLiteral("pelican.play.stepKey"));
    step_key_->setContext(Qt::WindowShortcut);
    status_ = new QLabel(&status_parent);
    status_->setObjectName(QStringLiteral("pelican.playStatus"));
    follow_timer_ = new QTimer(this);
    follow_timer_->setSingleShot(true);
    follow_timer_->setInterval(FollowReadIntervalMs);

    connect(pause_, &QAction::triggered, this, [this] { pressPause(); });
    connect(step_, &QAction::triggered, this, [this] { pressStep(); });
    connect(pause_key_, &QShortcut::activated, this, [this] {
        if (textEntryHasFocus() || !pause_->isVisible() || !pause_->isEnabled()) return;
        pause_->trigger();
    });
    connect(step_key_, &QShortcut::activated, this, [this] {
        if (textEntryHasFocus() || !step_->isVisible() || !step_->isEnabled()) return;
        step_->trigger();
    });
    connect(follow_timer_, &QTimer::timeout, this, [this] {
        if (model_.follow()) dispatch();
    });
    theme_listener_ = addStudioThemeListener([this] {
        if (hooks_.changed) hooks_.changed();
    });
    synchronize();
}

PlayControls::~PlayControls() { removeStudioThemeListener(theme_listener_); }

void PlayControls::engineAvailable() {
    model_.connect();
    dispatch();
}

void PlayControls::engineUnavailable() {
    follow_timer_->stop();
    transport_.clear();
    refused_.clear();
    model_.disconnect();
    synchronize();
}

void PlayControls::observeStatus(const QByteArray &result_json) {
    if (model_.observeStatus(parsed(result_json))) synchronize();
}

bool PlayControls::receiveResult(qint64 transport_id, const QByteArray &result_json) {
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
    // The play refusals name themselves as data.code; F-5's as error_code.
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

void PlayControls::pressPause() {
    // A checkable action has already flipped itself; the model says what the
    // button shows, and synchronize() puts it back when nothing was sent.
    if (model_.togglePause()) dispatch();
    else synchronize();
}

void PlayControls::pressStep() {
    if (model_.step()) dispatch();
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
    separator_->setVisible(shown);
    pause_->setVisible(shown);
    step_->setVisible(shown);
    pause_->setEnabled(model_.canTogglePause());
    step_->setEnabled(model_.canStep());
    pause_->setChecked(model_.paused());
    pause_->setToolTip(model_.paused()
                           ? tr("Resume the game (Ctrl+Shift+P)")
                           : tr("Pause the game: its clock, its code and its sound stop "
                                "(Ctrl+Shift+P)"));
    step_->setToolTip(model_.paused() ? tr("Advance the paused game by one frame (Ctrl+Alt+P)")
                                      : tr("Pause the game to step it one frame at a time "
                                           "(Ctrl+Alt+P)"));
    status_->setText(QString::fromStdString(model_.statusText()));
    status_->setVisible(shown);

    if (model_.followWanted() && !follow_timer_->isActive()) follow_timer_->start();

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

} // namespace PelicanStudio
