#include "engineprocess.hpp"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

// One reply line, walked once.
//
// `get_frame_plan` answers with 128-181 KB. Building a QJsonDocument out of
// that line only to hand the `result` back as bytes for the model to parse
// again cost three full passes over it on the UI thread. These helpers walk
// the line once and record where each top-level member's value lies, so the
// big member can travel as the bytes it already is and be parsed exactly
// once, by whoever actually needs its contents.

qsizetype skipJsonWhitespace(const QByteArray &text, qsizetype index) {
    while (index < text.size()) {
        const char character = text[index];
        if (character != ' ' && character != '\t' && character != '\n' &&
            character != '\r') {
            break;
        }
        ++index;
    }
    return index;
}

// One past the string literal beginning at `index`, or -1 if there is none.
qsizetype scanJsonString(const QByteArray &text, qsizetype index) {
    if (index >= text.size() || text[index] != '"') return -1;
    ++index;
    while (index < text.size()) {
        const char character = text[index];
        if (character == '\\') {
            index += 2;
            continue;
        }
        if (character == '"') return index + 1;
        ++index;
    }
    return -1;
}

// One past the value beginning at `index` (leading whitespace already
// skipped), or -1 if the text there is not a well-formed value. Containers are
// checked for balance and for matching closers, so a line that is not a JSON
// reply is still rejected here rather than passed on as if it were one.
qsizetype scanJsonValue(const QByteArray &text, qsizetype index) {
    if (index >= text.size()) return -1;
    const char first = text[index];
    if (first == '"') return scanJsonString(text, index);
    if (first == '{' || first == '[') {
        std::vector<char> closers;
        while (index < text.size()) {
            const char character = text[index];
            if (character == '"') {
                const qsizetype after = scanJsonString(text, index);
                if (after < 0) return -1;
                index = after;
                continue;
            }
            if (character == '{') {
                closers.push_back('}');
            } else if (character == '[') {
                closers.push_back(']');
            } else if (character == '}' || character == ']') {
                if (closers.empty() || closers.back() != character) return -1;
                closers.pop_back();
                if (closers.empty()) return index + 1;
            }
            ++index;
        }
        return -1;
    }
    // A number, true, false or null runs until the next structural character.
    const qsizetype begin = index;
    while (index < text.size()) {
        const char character = text[index];
        if (character == ',' || character == '}' || character == ']' ||
            character == ' ' || character == '\t' || character == '\n' ||
            character == '\r') {
            break;
        }
        ++index;
    }
    return index > begin ? index : -1;
}

// The top-level members of one JSON-RPC reply line, each as the bytes it
// already occupies in the line. `valid` is false for anything that is not one
// well-formed JSON object filling the whole line -- an engine log line, say.
struct JsonRpcEnvelope {
    bool valid = false;
    QByteArray id;
    QByteArray jsonrpc;
    QByteArray result;
    QByteArray error;
    bool has_result = false;
    bool has_error = false;
};

JsonRpcEnvelope scanJsonRpcEnvelope(const QByteArray &line) {
    JsonRpcEnvelope envelope;
    qsizetype index = skipJsonWhitespace(line, 0);
    if (index >= line.size() || line[index] != '{') return envelope;
    ++index;
    index = skipJsonWhitespace(line, index);
    if (index < line.size() && line[index] == '}') {
        envelope.valid = skipJsonWhitespace(line, index + 1) == line.size();
        return envelope;
    }
    for (;;) {
        index = skipJsonWhitespace(line, index);
        const qsizetype key_begin = index;
        const qsizetype key_end = scanJsonString(line, index);
        if (key_end < 0) return envelope;
        index = skipJsonWhitespace(line, key_end);
        if (index >= line.size() || line[index] != ':') return envelope;
        index = skipJsonWhitespace(line, index + 1);
        const qsizetype value_begin = index;
        const qsizetype value_end = scanJsonValue(line, index);
        if (value_end < 0) return envelope;
        const QByteArray key =
            line.sliced(key_begin + 1, key_end - key_begin - 2);
        const QByteArray value =
            line.sliced(value_begin, value_end - value_begin);
        if (key == "id") {
            envelope.id = value;
        } else if (key == "jsonrpc") {
            envelope.jsonrpc = value;
        } else if (key == "result") {
            envelope.result = value;
            envelope.has_result = true;
        } else if (key == "error") {
            envelope.error = value;
            envelope.has_error = true;
        }
        index = skipJsonWhitespace(line, value_end);
        if (index >= line.size()) return envelope;
        if (line[index] == ',') {
            ++index;
            continue;
        }
        if (line[index] != '}') return envelope;
        envelope.valid = skipJsonWhitespace(line, index + 1) == line.size();
        return envelope;
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
        const QString output = QString::fromLocal8Bit(bytes);
        emit standardErrorReceived(output);
        emit outputReceived(output);
    }
}

bool EngineProcess::routeRpcResponse(const QByteArray &line) {
    const JsonRpcEnvelope envelope = scanJsonRpcEnvelope(line);
    if (!envelope.valid || envelope.id.isEmpty()) {
        return false;
    }

    // An id has to be an exact positive integer. A quoted id is a string, not
    // a number, and is not this transport's.
    if (envelope.id.front() == '"') {
        return false;
    }
    bool id_parsed = false;
    const double id_number = envelope.id.toDouble(&id_parsed);
    if (!id_parsed || !std::isfinite(id_number) ||
        std::trunc(id_number) != id_number || id_number <= 0.0 ||
        id_number > static_cast<double>(MaximumExactJsonInteger)) {
        return false;
    }
    const qint64 request_id = static_cast<qint64>(id_number);
    if (!pending_rpc_requests_.remove(request_id)) {
        return false;
    }

    if (envelope.jsonrpc != "\"2.0\"") {
        emit rpcTransportFailed(
            request_id,
            tr("pelican_player returned an invalid JSON-RPC version."));
        return true;
    }

    if (envelope.has_result == envelope.has_error) {
        emit rpcTransportFailed(
            request_id,
            tr("pelican_player RPC response must contain exactly one of result or error."));
        return true;
    }
    if (envelope.has_result) {
        // The bytes the engine wrote, unchanged. Whoever needs the contents
        // parses them once; nothing here parses them at all.
        emit rpcResultReceived(request_id, envelope.result);
        return true;
    }

    // An error object is small, so the convenience of a document is free here.
    const QJsonDocument error_document = QJsonDocument::fromJson(
        "[" + envelope.error + "]");
    const QJsonValue error_value =
        error_document.isArray() ? error_document.array().at(0) : QJsonValue{};
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
