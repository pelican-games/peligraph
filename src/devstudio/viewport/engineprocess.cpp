#include "engineprocess.hpp"

#include <QFileInfo>

namespace PelicanStudio {
namespace {

void setError(QString *error, const QString &message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

EngineProcess::EngineProcess(QObject *parent) : QObject(parent) {
    process_.setProcessChannelMode(QProcess::MergedChannels);

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
