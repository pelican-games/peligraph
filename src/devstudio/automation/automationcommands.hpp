#pragma once

#include <QJsonValue>
#include <QString>
#include <QStringList>

class QJsonObject;

namespace PelicanStudio {

struct AutomationHost;

// The body of one reply. Exactly one side is meaningful: `result` when ok,
// `code` + `message` when not. The codes are the contract the driver reads, so
// they carry the project's `name@version` shape.
struct AutomationReply {
    bool ok = true;
    QJsonValue result;
    QString code;
    QString message;

    static AutomationReply success(QJsonValue result = QJsonValue{}) {
        return AutomationReply{true, std::move(result), {}, {}};
    }
    static AutomationReply failure(QString code, QString message) {
        return AutomationReply{false, QJsonValue{}, std::move(code),
                               std::move(message)};
    }
};

// Run one command against the live window. Called on the GUI thread, from the
// socket's readyRead: `rpc` and the `wait` family spin a nested event loop, so
// the window keeps painting while they block.
AutomationReply runAutomationCommand(const AutomationHost &host,
                                     const QJsonObject &request);

// Every command name this surface answers, for `list`-less discovery and for
// the error message an unknown command gets.
QStringList automationCommandNames();

} // namespace PelicanStudio
