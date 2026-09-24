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

ShaderReloadFollower::ShaderReloadFollower(Hooks hooks)
    : hooks_{std::move(hooks)}, context_{std::make_unique<QObject>()} {
    watcher_ = new QFileSystemWatcher(context_.get());
    QObject::connect(watcher_, &QFileSystemWatcher::fileChanged,
                     context_.get(),
                     [this](const QString &) { sourceChanged(); });
}

ShaderReloadFollower::~ShaderReloadFollower() = default;

const NodeShaderSources &ShaderReloadFollower::sources() const noexcept {
    return sources_;
}

qint64 ShaderReloadFollower::pendingRequest() const noexcept {
    return pending_;
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
    baseline_ = last_reading_;
    reads_left_ = MaxFollowReads;
    pending_follows_ = pending_follows_ || pending_ != 0;
    scheduleRead(FirstReadDelayMs);
}

void ShaderReloadFollower::scheduleRead(int delay_ms) {
    if (read_scheduled_) return;
    read_scheduled_ = true;
    QTimer::singleShot(delay_ms, context_.get(), [this] {
        read_scheduled_ = false;
        read();
    });
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
                                   const QByteArray &status_json) {
    if (request_id == 0 || request_id != pending_) return false;
    pending_ = 0;
    handle(status_json, std::exchange(pending_follows_, false));
    return true;
}

bool ShaderReloadFollower::fail(qint64 request_id) {
    if (request_id == 0 || request_id != pending_) return false;
    pending_ = 0;
    pending_follows_ = false;
    reads_left_ = 0;
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
