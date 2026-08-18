#include "frameplanreadcapability.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <utility>

namespace PelicanStudio {

FramePlanReadCapability framePlanReadCapability(
    EmbeddedViewport &viewport) {
    return FramePlanReadCapability{
        .ready = [&viewport](QObject *context,
                             FramePlanReadCapability::ReadyHandler handler) {
            handler(viewport.rpcReady(), {});
            QObject::connect(
                &viewport, &EmbeddedViewport::engineRpcBecameAvailable,
                context, [handler] { handler(true, {}); });
            QObject::connect(
                &viewport, &EmbeddedViewport::engineRpcBecameUnavailable,
                context,
                [handler](const QString &message) {
                    handler(false, message);
                });
        },
        .requestFramePlan = [&viewport](QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_frame_plan"), QJsonObject{}, error);
        },
        .result = [&viewport](
                      QObject *context,
                      FramePlanReadCapability::ResultHandler handler) {
            QObject::connect(
                &viewport, &EmbeddedViewport::inspectorRpcSucceeded,
                context,
                [handler = std::move(handler)](
                    qint64 request_id, const QByteArray &result_json) {
                    handler(request_id, result_json);
                });
        },
        .failure = [&viewport](
                       QObject *context,
                       FramePlanReadCapability::FailureHandler handler) {
            QObject::connect(
                &viewport, &EmbeddedViewport::inspectorRpcFailed, context,
                [handler = std::move(handler)](
                    qint64 request_id, const QString &message) {
                    handler(request_id, message);
                });
        },
    };
}

} // namespace PelicanStudio
