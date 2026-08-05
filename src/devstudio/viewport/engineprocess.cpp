#include "engineprocess.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace PelicanStudio {
namespace {

constexpr int RpcResponseTimeoutMs = 5000;
constexpr qint64 MaximumExactJsonInteger = 9007199254740991LL;

void setError(QString *error, const QString &message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

class EngineProcessLifetime {
  public:
    EngineProcessLifetime() {
#ifdef Q_OS_WIN
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_ == nullptr) {
            setWindowsFailure(QStringLiteral("create the engine process job"), GetLastError());
            return;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            setWindowsFailure(QStringLiteral("configure the engine process job"), GetLastError());
            return;
        }

        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        if (attribute_bytes == 0) {
            setWindowsFailure(QStringLiteral("size the engine process job attribute"),
                              GetLastError());
            return;
        }

        attribute_storage_.resize(attribute_bytes);
        attribute_list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attribute_storage_.data());
        if (!InitializeProcThreadAttributeList(attribute_list_, 1, 0, &attribute_bytes)) {
            attribute_list_ = nullptr;
            setWindowsFailure(QStringLiteral("initialize the engine process job attribute"),
                              GetLastError());
            return;
        }
        if (!UpdateProcThreadAttribute(attribute_list_, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                       &job_, sizeof(job_), nullptr, nullptr)) {
            setWindowsFailure(QStringLiteral("bind the engine process job attribute"),
                              GetLastError());
        }
#endif
    }

    ~EngineProcessLifetime() {
#ifdef Q_OS_WIN
        if (attribute_list_ != nullptr) {
            DeleteProcThreadAttributeList(attribute_list_);
        }
        if (job_ != nullptr) {
            CloseHandle(job_);
        }
#endif
    }

    EngineProcessLifetime(const EngineProcessLifetime &) = delete;
    EngineProcessLifetime &operator=(const EngineProcessLifetime &) = delete;

    void configure(QProcess &process) {
#ifdef Q_OS_WIN
        if (!failure_.isEmpty()) {
            return;
        }

        process.setCreateProcessArgumentsModifier(
            [this](QProcess::CreateProcessArguments *arguments) {
                // Put the child in the kill-on-close job as part of CreateProcess.
                // Assigning it after QProcess::started would leave a crash race.
                startup_info_ = {};
                startup_info_.StartupInfo = *arguments->startupInfo;
                startup_info_.StartupInfo.cb = sizeof(startup_info_);
                startup_info_.lpAttributeList = attribute_list_;
                arguments->startupInfo = &startup_info_.StartupInfo;
                arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
            });
#else
        Q_UNUSED(process);
#endif
    }

    QString failure() const { return failure_; }

  private:
    QString failure_;

#ifdef Q_OS_WIN
    HANDLE job_ = nullptr;
    std::vector<std::byte> attribute_storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST attribute_list_ = nullptr;
    STARTUPINFOEXW startup_info_{};

    void setWindowsFailure(const QString &operation, DWORD error) {
        failure_ = QStringLiteral("Could not %1 (Windows error %2).").arg(operation).arg(error);
    }
#endif
};

EngineProcess::EngineProcess(QObject *parent)
    : QObject(parent), process_lifetime_(std::make_unique<EngineProcessLifetime>()) {
    // stdout carries both ordinary text and line-delimited JSON-RPC replies.
    // Keep stderr separate so a log line can never be mistaken for a reply;
    // non-RPC stdout is still forwarded through the same Studio log signal.
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_lifetime_->configure(process_);

    connect(&process_, &QProcess::started, this, [this]() {
        state_ = State::running;
        last_process_id_ = process_.processId();
        emit processStarted(last_process_id_);
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this,
            [this]() { drainStandardOutput(); });
    connect(&process_, &QProcess::readyReadStandardError, this,
            [this]() { drainStandardError(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setFailure(process_.errorString());
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                drainStandardOutput(true);
                drainStandardError();
                failPendingRpcRequests(
                    tr("pelican_player exited before returning an RPC response."));
                const qint64 finished_process_id = last_process_id_;
                state_ = State::stopped;
                emit processStopped(finished_process_id, exit_code, exit_status);
            });
}

EngineProcess::~EngineProcess() {
    if (!isRunning()) {
        return;
    }

    process_.terminate();
    if (!process_.waitForFinished(1500)) {
        process_.kill();
        process_.waitForFinished(1500);
    }
}

bool EngineProcess::start(const EngineProcessLaunch &launch, QString *error) {
    if (isRunning()) {
        const QString message = tr("The engine process is already running.");
        setError(error, message);
        return false;
    }
    if (!process_lifetime_->failure().isEmpty()) {
        const QString message = process_lifetime_->failure();
        setFailure(message);
        setError(error, message);
        return false;
    }
    if (launch.program.isEmpty()) {
        const QString message = tr("No pelican_player executable was configured.");
        setFailure(message);
        setError(error, message);
        return false;
    }

    const QFileInfo program_info{launch.program};
    if (!program_info.isAbsolute() || !program_info.isFile()) {
        const QString message = tr("pelican_player was not found at %1").arg(launch.program);
        setFailure(message);
        setError(error, message);
        return false;
    }

    failure_.clear();
    state_ = State::starting;
    last_process_id_ = 0;
    standard_output_buffer_.clear();
    process_.setProgram(program_info.absoluteFilePath());
    process_.setArguments(launch.arguments);
    process_.setWorkingDirectory(
        launch.working_directory.isEmpty() ? program_info.absolutePath() : launch.working_directory);
    process_.start(QIODevice::ReadWrite);
    return true;
}

void EngineProcess::terminate() {
    if (isRunning()) {
        process_.terminate();
    }
}

void EngineProcess::kill() {
    if (isRunning()) {
        process_.kill();
    }
}

bool EngineProcess::waitForStarted(int timeout_ms) {
    return process_.waitForStarted(timeout_ms);
}

bool EngineProcess::waitForFinished(int timeout_ms) {
    return process_.waitForFinished(timeout_ms);
}

qint64 EngineProcess::requestRpc(const QString &method,
                                 const QJsonObject &params,
                                 QString *error) {
    if (state_ != State::running || !isRunning()) {
        setError(error, tr("pelican_player is not ready for RPC requests."));
        return 0;
    }
    if (method.isEmpty()) {
        setError(error, tr("RPC method must not be empty."));
        return 0;
    }
    if (next_rpc_request_id_ <= 0 ||
        next_rpc_request_id_ > MaximumExactJsonInteger) {
        setError(error, tr("RPC request identifier space is exhausted."));
        return 0;
    }

    const qint64 request_id = next_rpc_request_id_++;
    const QJsonObject request{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), request_id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    };
    QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    bytes.push_back('\n');

    pending_rpc_requests_.insert(request_id);
    if (process_.write(bytes) != bytes.size()) {
        pending_rpc_requests_.remove(request_id);
        setError(error, tr("Could not write the RPC request to pelican_player."));
        return 0;
    }

    QTimer::singleShot(RpcResponseTimeoutMs, this, [this, request_id]() {
        if (!pending_rpc_requests_.remove(request_id)) {
            return;
        }
        emit rpcTransportFailed(
            request_id,
            tr("pelican_player did not return an RPC response within %1 seconds.")
                .arg(RpcResponseTimeoutMs / 1000));
    });
    return request_id;
}

bool EngineProcess::isRunning() const noexcept {
    return process_.state() != QProcess::NotRunning;
}

qint64 EngineProcess::processId() const noexcept {
    return process_.processId();
}

void EngineProcess::setFailure(const QString &message) {
    failure_ = message;
    state_ = State::failed;
    emit processFailed(message);
}

void EngineProcess::drainStandardOutput(bool flush_partial_line) {
    standard_output_buffer_ += process_.readAllStandardOutput();

    for (;;) {
        const qsizetype newline = standard_output_buffer_.indexOf('\n');
        if (newline < 0) {
            break;
        }
        const QByteArray output_line =
            standard_output_buffer_.first(newline + 1);
        standard_output_buffer_.remove(0, newline + 1);

        QByteArray rpc_line = output_line.first(output_line.size() - 1);
        if (rpc_line.endsWith('\r')) {
            rpc_line.chop(1);
        }
        if (!routeRpcResponse(rpc_line)) {
            emit outputReceived(QString::fromLocal8Bit(output_line));
        }
    }

    if (flush_partial_line && !standard_output_buffer_.isEmpty()) {
        const QByteArray output_line = std::move(standard_output_buffer_);
        standard_output_buffer_.clear();
        if (!routeRpcResponse(output_line)) {
            emit outputReceived(QString::fromLocal8Bit(output_line));
        }
    }
}

void EngineProcess::drainStandardError() {
    const QByteArray bytes = process_.readAllStandardError();
    if (!bytes.isEmpty()) {
        emit outputReceived(QString::fromLocal8Bit(bytes));
    }
}

bool EngineProcess::routeRpcResponse(const QByteArray &line) {
    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return false;
    }

    const QJsonObject response = document.object();
    const QJsonValue id_value = response.value(QStringLiteral("id"));
    if (!id_value.isDouble()) {
        return false;
    }
    const double id_number = id_value.toDouble();
    if (!std::isfinite(id_number) || std::trunc(id_number) != id_number ||
        id_number <= 0.0 ||
        id_number > static_cast<double>(MaximumExactJsonInteger)) {
        return false;
    }
    const qint64 request_id = static_cast<qint64>(id_number);
    if (!pending_rpc_requests_.remove(request_id)) {
        return false;
    }

    if (response.value(QStringLiteral("jsonrpc")).toString() !=
        QStringLiteral("2.0")) {
        emit rpcTransportFailed(
            request_id,
            tr("pelican_player returned an invalid JSON-RPC version."));
        return true;
    }

    const bool has_result = response.contains(QStringLiteral("result"));
    const bool has_error = response.contains(QStringLiteral("error"));
    if (has_result == has_error) {
        emit rpcTransportFailed(
            request_id,
            tr("pelican_player RPC response must contain exactly one of result or error."));
        return true;
    }
    if (has_result) {
        emit rpcResultReceived(request_id,
                               response.value(QStringLiteral("result")));
        return true;
    }

    const QJsonValue error_value = response.value(QStringLiteral("error"));
    if (!error_value.isObject()) {
        emit rpcTransportFailed(
            request_id,
            tr("pelican_player returned an invalid JSON-RPC error."));
        return true;
    }
    const QJsonObject rpc_error = error_value.toObject();
    const QJsonValue code_value = rpc_error.value(QStringLiteral("code"));
    const QJsonValue message_value = rpc_error.value(QStringLiteral("message"));
    const double code_number = code_value.toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!code_value.isDouble() || !std::isfinite(code_number) ||
        std::trunc(code_number) != code_number ||
        code_number < std::numeric_limits<int>::min() ||
        code_number > std::numeric_limits<int>::max() ||
        !message_value.isString()) {
        emit rpcTransportFailed(
            request_id,
            tr("pelican_player returned an invalid JSON-RPC error."));
        return true;
    }
    emit rpcErrorReceived(request_id, static_cast<int>(code_number),
                          message_value.toString(),
                          rpc_error.value(QStringLiteral("data")));
    return true;
}

void EngineProcess::failPendingRpcRequests(const QString &message) {
    const auto pending = pending_rpc_requests_.values();
    pending_rpc_requests_.clear();
    for (const qint64 request_id : pending) {
        emit rpcTransportFailed(request_id, message);
    }
}

} // namespace PelicanStudio
