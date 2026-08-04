#include "engineprocess.hpp"

#include <QFileInfo>

#include <cstddef>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace PelicanStudio {
namespace {

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
    // QProcess redirects the child's stderr into the standard-output channel
    // in this mode, so drainOutput() emits both streams through one Studio log
    // path.
    process_.setProcessChannelMode(QProcess::MergedChannels);
    process_lifetime_->configure(process_);

    connect(&process_, &QProcess::started, this, [this]() {
        state_ = State::running;
        last_process_id_ = process_.processId();
        emit processStarted(last_process_id_);
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this]() { drainOutput(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            setFailure(process_.errorString());
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus exit_status) {
                drainOutput();
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
    process_.setProgram(program_info.absoluteFilePath());
    process_.setArguments(launch.arguments);
    process_.setWorkingDirectory(
        launch.working_directory.isEmpty() ? program_info.absolutePath() : launch.working_directory);
    process_.start(QIODevice::ReadOnly);
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

void EngineProcess::drainOutput() {
    const QByteArray bytes = process_.readAllStandardOutput();
    if (!bytes.isEmpty()) {
        emit outputReceived(QString::fromLocal8Bit(bytes));
    }
}

} // namespace PelicanStudio
