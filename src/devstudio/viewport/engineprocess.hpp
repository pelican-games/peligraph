#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

namespace PelicanStudio {

class EngineProcessLifetime;

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
    qint64 requestRpc(const QString &method, const QJsonObject &params,
                      QString *error = nullptr);

    State state() const noexcept { return state_; }
    bool isRunning() const noexcept;
    qint64 processId() const noexcept;
    QString failure() const { return failure_; }

  signals:
    void processStarted(qint64 process_id);
    void processStopped(qint64 process_id, int exit_code, QProcess::ExitStatus exit_status);
    void processFailed(const QString &message);
    void outputReceived(const QString &output);
    void rpcResultReceived(qint64 request_id, const QJsonValue &result);
    void rpcErrorReceived(qint64 request_id, int code,
                          const QString &message, const QJsonValue &data);
    void rpcTransportFailed(qint64 request_id, const QString &message);

  private:
    std::unique_ptr<EngineProcessLifetime> process_lifetime_;
    QProcess process_;
    State state_ = State::stopped;
    qint64 last_process_id_ = 0;
    QString failure_;
    QByteArray standard_output_buffer_;
    QSet<qint64> pending_rpc_requests_;
    qint64 next_rpc_request_id_ = 1;

    void setFailure(const QString &message);
    void drainStandardOutput(bool flush_partial_line = false);
    void drainStandardError();
    bool routeRpcResponse(const QByteArray &line);
    void failPendingRpcRequests(const QString &message);
};

} // namespace PelicanStudio
