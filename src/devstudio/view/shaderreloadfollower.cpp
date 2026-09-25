#include "shaderreloadfollower.hpp"

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <set>
#include <utility>

namespace PelicanStudio {
namespace {

// The engine's reload_shaders refusals (reloadservice.hpp).
constexpr auto ShaderReloadFailedCode = "shader_reload_failed@1";
constexpr auto ShaderReloadUnavailableCode = "shader_reload_unavailable@1";

} // namespace

ShaderReloadFollower::ShaderReloadFollower(Hooks hooks)
    : hooks_{std::move(hooks)}, context_{std::make_unique<QObject>()} {
    watcher_ = new QFileSystemWatcher(context_.get());
    QObject::connect(watcher_, &QFileSystemWatcher::fileChanged,
                     context_.get(),
                     [this](const QString &) { sourceChanged(); });
    reload_debounce_ = new QTimer(context_.get());
    reload_debounce_->setSingleShot(true);
    reload_debounce_->setInterval(ReloadDebounceMs);
    QObject::connect(reload_debounce_, &QTimer::timeout, context_.get(),
                     [this] { sendReload(); });
}

ShaderReloadFollower::~ShaderReloadFollower() = default;

const NodeShaderSources &ShaderReloadFollower::sources() const noexcept {
    return sources_;
}

qint64 ShaderReloadFollower::pendingRequest() const noexcept {
    return pending_;
}

qint64 ShaderReloadFollower::pendingReload() const noexcept {
    return pending_reload_;
}

int ShaderReloadFollower::readsLeft() const noexcept { return reads_left_; }

void ShaderReloadFollower::setSources(NodeShaderSources sources) {
    if (sources == sources_) return;
    sources_ = std::move(sources);
    rewatch();
    // What the engine already knows about these files, once. Not a clock:
    // the next read waits for the next change.
    if (!sources_.empty()) scheduleRead(0);
}

void ShaderReloadFollower::rewatch() {
    std::set<QString> wanted;
    for (const auto &[node, paths] : sources_) {
        (void)node;
        for (const auto &path : paths)
            wanted.insert(QString::fromStdWString(path.wstring()));
    }
    const QStringList watched = watcher_->files();
    for (const QString &path : watched)
        if (!wanted.contains(path)) watcher_->removePath(path);
    for (const QString &path : wanted) {
        // An editor that saves by replacing the file takes the old one out
        // of the watch; the new one is put back here.
        if (!watched.contains(path) && QFileInfo::exists(path))
            watcher_->addPath(path);
    }
}

void ShaderReloadFollower::sourceChanged() {
    if (hooks_.request_reload) {
        // F-5: the engine is asked, once the writes stop for a moment.
        reload_debounce_->start();
        return;
    }
    baseline_ = last_reading_;
    reads_left_ = MaxFollowReads;
    pending_follows_ = pending_follows_ || pending_ != 0;
    scheduleRead(FirstReadDelayMs);
}

void ShaderReloadFollower::sendReload() {
    rewatch();
    if (pending_reload_ != 0) {
        reload_again_ = true;
        return;
    }
    QString error;
    pending_reload_ = hooks_.request_reload(&error);
    if (pending_reload_ == 0 && !error.isEmpty() && hooks_.reloaded)
        hooks_.reloaded(ShaderReloadOutcome{
            .kind = ShaderReloadOutcome::Kind::error, .message = error});
}

void ShaderReloadFollower::finishReload(const ShaderReloadOutcome &outcome) {
    pending_reload_ = 0;
    if (hooks_.reloaded) hooks_.reloaded(outcome);
    // The marks follow the engine's own status: a commit has cleared its
    // last error, a failure has written it.
    if (outcome.kind == ShaderReloadOutcome::Kind::committed ||
        outcome.kind == ShaderReloadOutcome::Kind::failed ||
        outcome.kind == ShaderReloadOutcome::Kind::refused)
        readOnce();
    if (std::exchange(reload_again_, false)) sendReload();
}

void ShaderReloadFollower::scheduleRead(int delay_ms) {
    if (read_scheduled_) return;
    read_scheduled_ = true;
    QTimer::singleShot(delay_ms, context_.get(), [this] {
        read_scheduled_ = false;
        read();
    });
}

void ShaderReloadFollower::readOnce() {
    if (pending_ != 0) {
        read_again_ = true;
        return;
    }
    scheduleRead(0);
}

void ShaderReloadFollower::read() {
    rewatch();
    if (pending_ != 0 || !hooks_.request_status) return;
    QString error;
    pending_ = hooks_.request_status(&error);
    pending_follows_ = reads_left_ > 0;
    if (pending_ == 0) reads_left_ = 0;
}

bool ShaderReloadFollower::receive(qint64 request_id,
                                   const QByteArray &result_json) {
    if (request_id == 0) return false;
    if (request_id == pending_reload_) {
        ShaderReloadOutcome outcome;
        try {
            const auto result = nlohmann::json::parse(
                result_json.constData(),
                result_json.constData() + result_json.size());
            const bool committed = result.value("committed", false);
            outcome.kind = committed ? ShaderReloadOutcome::Kind::committed
                                     : ShaderReloadOutcome::Kind::unchanged;
            if (const auto changed = result.find("changed");
                changed != result.end() && changed->is_array()) {
                for (const auto &reference : *changed)
                    if (reference.is_string())
                        outcome.changed.push_back(QString::fromStdString(
                            reference.get<std::string>()));
            }
            // VI-1b: a commit can still have been refused for some passes,
            // which keep their previous shader. Absent reads as none.
            if (const auto refused = result.find("refused_passes");
                refused != result.end() && refused->is_array()) {
                for (const auto &pass : *refused) {
                    if (!pass.is_object()) continue;
                    outcome.refused_passes.push_back(QString::fromStdString(
                        pass.value("pass", std::string{})));
                    if (outcome.message.isEmpty())
                        outcome.message = QString::fromStdString(
                            pass.value("reason", std::string{}));
                }
            }
            if (!outcome.refused_passes.isEmpty())
                outcome.kind = ShaderReloadOutcome::Kind::refused;
        } catch (const std::exception &error) {
            outcome.kind = ShaderReloadOutcome::Kind::error;
            outcome.message = QString::fromUtf8(error.what());
        }
        finishReload(outcome);
        return true;
    }
    if (request_id != pending_) return false;
    pending_ = 0;
    handle(result_json, std::exchange(pending_follows_, false));
    if (std::exchange(read_again_, false)) scheduleRead(0);
    return true;
}

bool ShaderReloadFollower::reject(qint64 request_id, const QString &error_code,
                                  const QString &message) {
    if (request_id == 0 || request_id != pending_reload_) return false;
    auto kind = ShaderReloadOutcome::Kind::error;
    if (error_code == QLatin1String(ShaderReloadFailedCode))
        kind = ShaderReloadOutcome::Kind::failed;
    else if (error_code == QLatin1String(ShaderReloadUnavailableCode))
        kind = ShaderReloadOutcome::Kind::unavailable;
    finishReload(ShaderReloadOutcome{.kind = kind, .message = message});
    return true;
}

bool ShaderReloadFollower::fail(qint64 request_id, const QString &message) {
    if (request_id == 0) return false;
    if (request_id == pending_reload_) {
        finishReload(ShaderReloadOutcome{
            .kind = ShaderReloadOutcome::Kind::error, .message = message});
        return true;
    }
    if (request_id != pending_) return false;
    pending_ = 0;
    pending_follows_ = false;
    reads_left_ = 0;
    if (std::exchange(read_again_, false)) scheduleRead(0);
    return true;
}

void ShaderReloadFollower::consume(const QByteArray &status_json) {
    handle(status_json, false);
}

void ShaderReloadFollower::handle(const QByteArray &status_json,
                                  bool following) {
    ShaderReloadReading reading;
    try {
        reading = readShaderReloadStatus(nlohmann::json::parse(
            status_json.constData(),
            status_json.constData() + status_json.size()));
    } catch (const std::exception &) {
        reads_left_ = 0;
        return;
    }
    last_reading_ = reading;
    if (hooks_.apply)
        hooks_.apply(shaderReloadMarks(reading, sources_), reading, following);
    if (!following) return;
    // The engine has answered the change when its reading moved; a shut gate
    // or an engine without the participant will never answer it.
    const bool settled = !reading.reported || !reading.enabled ||
                         (baseline_ && reading != *baseline_);
    if (settled || --reads_left_ <= 0) {
        reads_left_ = 0;
        return;
    }
    scheduleRead(FollowReadIntervalMs);
}

} // namespace PelicanStudio
