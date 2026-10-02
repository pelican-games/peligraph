#include "automationserver.hpp"
#include "automationcommands.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QWidget>

#include <iostream>

namespace PelicanStudio {
namespace {

QByteArray encodeReply(const QJsonValue &id, const AutomationReply &reply) {
    QJsonObject envelope{{QStringLiteral("id"), id},
                         {QStringLiteral("ok"), reply.ok}};
    if (reply.ok)
        envelope.insert(QStringLiteral("result"), reply.result);
    else
        envelope.insert(QStringLiteral("error"),
                        QJsonObject{{QStringLiteral("code"), reply.code},
                                    {QStringLiteral("message"), reply.message}});
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n';
}

void writeLine(QLocalSocket *socket, const QByteArray &line) {
    if (socket == nullptr) return;
    socket->write(line);
    socket->flush();
}

} // namespace

AutomationServer::AutomationServer(AutomationHost host, QObject *parent)
    : QObject{parent}, host_{std::move(host)} {}

AutomationServer::~AutomationServer() = default;

bool AutomationServer::listen(const QString &name, QString *error) {
    if (name.trimmed().isEmpty()) {
        if (error != nullptr) *error = tr("--automation needs a socket name");
        return false;
    }
    server_ = new QLocalServer(this);
    // A Studio that crashed last run can leave the name behind; the name is
    // the test's handle, so taking it over is what the driver expects.
    QLocalServer::removeServer(name);
    if (!server_->listen(name)) {
        if (error != nullptr) *error = server_->errorString();
        delete server_;
        server_ = nullptr;
        return false;
    }
    endpoint_ = server_->fullServerName();
    connect(server_, &QLocalServer::newConnection, this,
            &AutomationServer::acceptConnection);
    // Synthetic events go straight to the widget they name, but focus is the
    // window manager's business: an inactive window has no focus widget, and a
    // scenario that types into an editor would type into nothing.
    if (host_.window != nullptr) {
        host_.window->raise();
        host_.window->activateWindow();
    }
    return true;
}

void AutomationServer::acceptConnection() {
    while (QLocalSocket *socket = server_->nextPendingConnection()) {
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { readFrom(socket); });
        connect(socket, &QLocalSocket::disconnected, socket,
                &QLocalSocket::deleteLater);
    }
}

void AutomationServer::readFrom(QLocalSocket *socket) {
    while (socket->canReadLine()) {
        QByteArray line = socket->readLine();
        while (line.endsWith('\n') || line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        handleLine(socket, line);
    }
}

void AutomationServer::handleLine(QLocalSocket *socket, const QByteArray &line) {
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
    if (!document.isObject()) {
        writeLine(socket,
                  encodeReply(QJsonValue{},
                              AutomationReply::failure(
                                  QStringLiteral("automation.bad_request@1"),
                                  parse_error.errorString())));
        return;
    }
    const QJsonObject request = document.object();
    const QJsonValue id = request.value(QStringLiteral("id"));
    if (running_command_) {
        writeLine(socket,
                  encodeReply(id, AutomationReply::failure(
                                      QStringLiteral("automation.busy@1"),
                                      tr("a command is already running; the "
                                         "automation channel is sequential"))));
        return;
    }
    if (request.value(QStringLiteral("cmd")).toString() ==
        QLatin1String("quit")) {
        // Answer before the loop that is answering goes away.
        writeLine(socket, encodeReply(id, AutomationReply::success(QJsonObject{
                                              {QStringLiteral("quitting"), true}})));
        socket->waitForBytesWritten(2000);
        QTimer::singleShot(0, QCoreApplication::instance(), [before = host_.before_quit] {
            if (before) before([] { QCoreApplication::quit(); });
            else QCoreApplication::quit();
        });
        return;
    }

    running_command_ = true;
    AutomationReply reply;
    try {
        reply = runAutomationCommand(host_, request);
    } catch (const std::exception &failure) {
        reply = AutomationReply::failure(
            QStringLiteral("automation.command_threw@1"),
            QString::fromUtf8(failure.what()));
    }
    running_command_ = false;
    writeLine(socket, encodeReply(id, reply));
}

} // namespace PelicanStudio
