#pragma once

#include <QtGlobal>

#include <functional>

class QByteArray;
class QObject;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

// The read-only, asynchronous surface needed by the fullscreen pass form.
// In particular, it deliberately cannot express a generic RPC method.
struct FramePlanReadCapability {
    using ReadyHandler =
        std::function<void(bool available, const QString &reason)>;
    using ResultHandler =
        std::function<void(qint64 request_id,
                           const QByteArray &result_json)>;
    using FailureHandler =
        std::function<void(qint64 request_id, const QString &message)>;

    std::function<void(QObject *context, ReadyHandler handler)> ready;
    std::function<qint64(QString *error)> requestFramePlan;
    std::function<void(QObject *context, ResultHandler handler)> result;
    std::function<void(QObject *context, FailureHandler handler)> failure;
};

// Production adapter. MainWindow is the composition root that converts its
// generic EmbeddedViewport into the restricted capability above.
FramePlanReadCapability framePlanReadCapability(
    EmbeddedViewport &viewport);

} // namespace PelicanStudio
