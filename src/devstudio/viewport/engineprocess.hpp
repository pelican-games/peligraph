#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace PelicanStudio {

struct EngineProcessLaunch {
    QString program;
    QStringList arguments;
    QString working_directory;
};

class EngineProcess final : public QObject {
    Q_OBJECT

  public:
    enum class State {
        stopped,
        starting,
        running,
        failed,
    };

    explicit EngineProcess(QObject *parent = nullptr);
    ~EngineProcess() override;

    bool start(const EngineProcessLaunch &launch, QString *error = nullptr);
    void terminate();
    void kill();
    bool waitForStarted(int timeout_ms);
    bool waitForFinished(int timeout_ms);

    State state() const noexcept { return state_; }
    bool isRunning() const noexcept;
    qint64 processId() const noexcept;
    QString failure() const { return failure_; }

  signals:
    void processStarted(qint64 process_id);
    void processStopped(qint64 process_id, int exit_code, QProcess::ExitStatus exit_status);
    void processFailed(const QString &message);
    void outputReceived(const QString &output);

  private:
    QProcess process_;
    State state_ = State::stopped;
    qint64 last_process_id_ = 0;
    QString failure_;

    void setFailure(const QString &message);
    void drainOutput();
};

} // namespace PelicanStudio
