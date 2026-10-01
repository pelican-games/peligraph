#pragma once

#include "shaderdiagnostics.hpp"

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <optional>

class QByteArray;
class QFileSystemWatcher;
class QObject;
class QTimer;

namespace PelicanStudio {

// F-5. What one reload_shaders request came back with.
struct ShaderReloadOutcome {
    enum class Kind {
        // The engine recompiled what moved and draws it now.
        committed,
        // Nothing the engine compiles had moved.
        unchanged,
        // VI-1b: the reload committed, but the engine refused it for one or
        // more passes whose inputs the new shader no longer matches; those
        // keep drawing with their previous shader. `refused_passes` names
        // them and `message` is the first one's reason. A failure, not a
        // commit: whether it worked is whether refused_passes is empty.
        refused,
        // A shader did not compile; `message` is the engine's log, the same
        // text reload status keeps as last_error.
        failed,
        // A determinism gate (replay, strict assets) refuses the request;
        // `message` is the reason the engine gave.
        unavailable,
        // Anything else: the request could not be sent or answered.
        error,
    };
    Kind kind = Kind::error;
    QString message;
    QStringList changed;
    QStringList refused_passes;
};

// F-3 / F-5. Follows the shader files the drawn nodes read. A write to one
// of them asks the engine to recompile (reload_shaders, once per burst of
// writes, one request at a time), and then reads its shader reload status
// once so the marks follow what the engine now says.
//
// Without a `request_reload` hook it is F-3's follower: after a write it
// reads the status a bounded number of times, and nothing more.
//
// Studio has no idle clock that reads get_status (Q-2 stopped them), and this
// adds none -- every request it makes follows a file change, plus one read
// when the set of files it watches changes.
class ShaderReloadFollower {
  public:
    struct Hooks {
        // Sends get_status; the id the reply will carry, or 0.
        std::function<qint64(QString *error)> request_status;
        // A reading arrived. `after_change` is true for the reads a file
        // change started, false for the others.
        std::function<void(const ShaderReloadMarks &,
                           const ShaderReloadReading &, bool after_change)>
            apply;
        // F-5. Sends reload_shaders; the id the reply will carry, or 0.
        std::function<qint64(QString *error)> request_reload;
        // F-5. The answer to a reload_shaders request.
        std::function<void(const ShaderReloadOutcome &)> reloaded;
    };

    // After a change the engine needs its watcher's debounce and a frame
    // boundary before its status can say anything, so the first read waits
    // and the next ones follow until the reading moves.
    static constexpr int FirstReadDelayMs = 300;
    static constexpr int FollowReadIntervalMs = 250;
    static constexpr int MaxFollowReads = 16;
    // F-5. An editor's save is often several writes; they are one request.
    static constexpr int ReloadDebounceMs = 200;

    explicit ShaderReloadFollower(Hooks hooks);
    ~ShaderReloadFollower();
    ShaderReloadFollower(const ShaderReloadFollower &) = delete;
    ShaderReloadFollower &operator=(const ShaderReloadFollower &) = delete;

    // The files each node reads. A set that differs from the last one is
    // watched instead, and read once so marks start from what the engine
    // already knows.
    void setSources(NodeShaderSources sources);
    [[nodiscard]] const NodeShaderSources &sources() const noexcept;

    // RL-1s, decision 1: the render documents (the root and its fragments)
    // are watched by this same watcher, with a hook of their own -- a write
    // to one never asks for reload_shaders. Each call, and each change,
    // puts a file an editor replaced back into the watch.
    void setDocumentFiles(std::vector<std::filesystem::path> files,
                          std::function<void(const QString &path)> changed);
    // A watched render document changed. Public so a test can say so.
    void documentChanged(const QString &path);

    // The reply for a request this follower made. False when the id is
    // someone else's.
    bool receive(qint64 request_id, const QByteArray &result_json);
    // F-5. The engine refused a request this follower made, with the
    // error_code its JSON-RPC error carried. False when the id is someone
    // else's.
    bool reject(qint64 request_id, const QString &error_code,
                const QString &message);
    bool fail(qint64 request_id, const QString &message = {});

    // A get_status result handed in directly (tests, and any caller that
    // already has one). Marks are applied; no follow-up read is started.
    void consume(const QByteArray &status_json);

    // A watched file changed. Public so a test can say so without a disk.
    void sourceChanged();

    [[nodiscard]] qint64 pendingRequest() const noexcept;
    [[nodiscard]] qint64 pendingReload() const noexcept;
    [[nodiscard]] int readsLeft() const noexcept;

  private:
    void scheduleRead(int delay_ms);
    void read();
    void readOnce();
    void handle(const QByteArray &status_json, bool following);
    void rewatch();
    void sendReload();
    void finishReload(const ShaderReloadOutcome &outcome);

    Hooks hooks_;
    NodeShaderSources sources_;
    // Parent of the watcher and the context of every scheduled read, so none
    // of them outlives this object.
    std::unique_ptr<QObject> context_;
    QFileSystemWatcher *watcher_ = nullptr;
    QTimer *reload_debounce_ = nullptr;
    qint64 pending_ = 0;
    bool pending_follows_ = false;
    bool read_scheduled_ = false;
    // A read wanted while another was in flight: made when that one lands.
    bool read_again_ = false;
    int reads_left_ = 0;
    qint64 pending_reload_ = 0;
    // A write that landed while a reload was in flight: one more reload.
    bool reload_again_ = false;
    std::optional<ShaderReloadReading> last_reading_;
    std::optional<ShaderReloadReading> baseline_;
    // RL-1s: the render documents, as the watcher spells them.
    QStringList document_files_;
    std::function<void(const QString &path)> document_changed_;
};

} // namespace PelicanStudio
