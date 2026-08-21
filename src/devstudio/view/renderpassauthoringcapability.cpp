#include "renderpassauthoringcapability.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <utility>

namespace PelicanStudio {

RenderPassAuthoringCapability renderPassAuthoringCapability(
    EmbeddedViewport &viewport) {
    return RenderPassAuthoringCapability{
        .ready = [&viewport](
                     QObject *context,
                     RenderPassAuthoringCapability::ReadyHandler handler) {
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
        .requestContext = [&viewport](QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_render_authoring_context"),
                QJsonObject{}, error);
        },
        .addAuthoredPass = [&viewport](const QJsonObject &params,
                                      QString *error) {
            return viewport.requestRpc(
                QStringLiteral("add_authored_pass"), params, error);
        },
        .removeAuthoredPass = [&viewport](const QJsonObject &params,
                                         QString *error) {
            return viewport.requestRpc(
                QStringLiteral("remove_authored_pass"), params, error);
        },
        .requestEditResult = [&viewport](const QString &ticket,
                                        QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_edit_result"),
                QJsonObject{{QStringLiteral("ticket"), ticket}}, error);
        },
        .result = [&viewport](
                      QObject *context,
                      RenderPassAuthoringCapability::ResultHandler handler) {
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
                       RenderPassAuthoringCapability::FailureHandler handler) {
            QObject::connect(
                &viewport, &EmbeddedViewport::inspectorRpcFailed, context,
                [handler = std::move(handler)](
                    qint64 request_id, const QString &message) {
                    handler(request_id, message);
                });
        },
    };
}

RenderPassAuthoringCapability unavailableRenderPassAuthoringCapability() {
    const auto unavailable = [](QString *error) -> qint64 {
        if (error != nullptr) {
            *error = QStringLiteral("render authoring RPC is unavailable");
        }
        return 0;
    };
    return RenderPassAuthoringCapability{
        .ready = [](QObject *,
                    RenderPassAuthoringCapability::ReadyHandler handler) {
            handler(false,
                    QStringLiteral("render authoring RPC is unavailable"));
        },
        .requestContext = unavailable,
        .addAuthoredPass =
            [unavailable](const QJsonObject &, QString *error) {
                return unavailable(error);
            },
        .removeAuthoredPass =
            [unavailable](const QJsonObject &, QString *error) {
                return unavailable(error);
            },
        .requestEditResult =
            [unavailable](const QString &, QString *error) {
                return unavailable(error);
            },
        .result = [](QObject *,
                     RenderPassAuthoringCapability::ResultHandler) {},
        .failure = [](QObject *,
                      RenderPassAuthoringCapability::FailureHandler) {},
    };
}

} // namespace PelicanStudio
