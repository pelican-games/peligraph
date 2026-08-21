#pragma once

#include <QtGlobal>

#include <functional>

class QByteArray;
class QJsonObject;
class QObject;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

// Restricted asynchronous surface used by the authored-pass form.  The
// widget cannot choose arbitrary RPC methods and never receives a filesystem
// path; document ownership stays in the engine process.
struct RenderPassAuthoringCapability {
    using ReadyHandler =
        std::function<void(bool available, const QString &reason)>;
    using ResultHandler =
        std::function<void(qint64 request_id,
                           const QByteArray &result_json)>;
    using FailureHandler =
        std::function<void(qint64 request_id, const QString &message)>;

    std::function<void(QObject *context, ReadyHandler handler)> ready;
    std::function<qint64(QString *error)> requestContext;
    std::function<qint64(const QJsonObject &params, QString *error)>
        addAuthoredPass;
    std::function<qint64(const QJsonObject &params, QString *error)>
        removeAuthoredPass;
    std::function<qint64(const QString &ticket, QString *error)>
        requestEditResult;
    std::function<void(QObject *context, ResultHandler handler)> result;
    std::function<void(QObject *context, FailureHandler handler)> failure;
};

RenderPassAuthoringCapability renderPassAuthoringCapability(
    EmbeddedViewport &viewport);

// Keeps legacy deterministic projection tests useful without granting them a
// fake write path. Production always uses the adapter above.
RenderPassAuthoringCapability unavailableRenderPassAuthoringCapability();

} // namespace PelicanStudio
