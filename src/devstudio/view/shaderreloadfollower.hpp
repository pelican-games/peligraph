#pragma once

#include "shaderdiagnostics.hpp"

#include <QtGlobal>

#include <functional>
#include <memory>
#include <optional>

class QByteArray;
class QFileSystemWatcher;
class QObject;
class QString;

namespace PelicanStudio {

// F-3. Reads the engine's shader reload status when, and only when, there is
// a reason to think it moved: a shader file a drawn node reads was written.
// Studio has no idle clock that reads get_status (Q-2 stopped them), and this
// adds none -- the reads it makes are a bounded run after a file change, plus
// one when the set of files it watches changes.
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
    };

    // After a change the engine needs its watcher's debounce and a frame
    // boundary before its status can say anything, so the first read waits
    // and the next ones follow until the reading moves.
    static constexpr int FirstReadDelayMs = 300;
    static constexpr int FollowReadIntervalMs = 250;
    static constexpr int MaxFollowReads = 16;

    explicit ShaderReloadFollower(Hooks hooks);
    ~ShaderReloadFollower();
    ShaderReloadFollower(const ShaderReloadFollower &) = delete;
    ShaderReloadFollower &operator=(const ShaderReloadFollower &) = delete;

    // The files each node reads. A set that differs from the last one is
    // watched instead, and read once so marks start from what the engine
    // already knows.
    void setSources(NodeShaderSources sources);
    [[nodiscard]] const NodeShaderSources &sources() const noexcept;

    // The get_status reply for a request this follower made. False when the
    // id is someone else's.
    bool receive(qint64 request_id, const QByteArray &status_json);
    bool fail(qint64 request_id);

    // A get_status result handed in directly (tests, and any caller that
    // already has one). Marks are applied; no follow-up read is started.
    void consume(const QByteArray &status_json);

    // A watched file changed. Public so a test can say so without a disk.
    void sourceChanged();

    [[nodiscard]] qint64 pendingRequest() const noexcept;
    [[nodiscard]] int readsLeft() const noexcept;

  private:
    void scheduleRead(int delay_ms);
    void read();
    void handle(const QByteArray &status_json, bool following);
    void rewatch();

    Hooks hooks_;
    NodeShaderSources sources_;
    // Parent of the watcher and the context of every scheduled read, so none
    // of them outlives this object.
    std::unique_ptr<QObject> context_;
    QFileSystemWatcher *watcher_ = nullptr;
    qint64 pending_ = 0;
    bool pending_follows_ = false;
    bool read_scheduled_ = false;
    int reads_left_ = 0;
    std::optional<ShaderReloadReading> last_reading_;
    std::optional<ShaderReloadReading> baseline_;
};

} // namespace PelicanStudio
