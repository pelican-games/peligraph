#include "graphcanvasreadcapability.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <utility>

namespace PelicanStudio {

GraphCanvasReadCapability graphCanvasReadCapability(EmbeddedViewport &viewport) {
    return GraphCanvasReadCapability{
        .ready = [&viewport](QObject *context,
                             GraphCanvasReadCapability::ReadyHandler handler) {
            handler(viewport.rpcReady(), {});
            QObject::connect(
                &viewport, &EmbeddedViewport::engineRpcBecameAvailable,
                context, [handler] { handler(true, {}); });
            QObject::connect(
                &viewport, &EmbeddedViewport::engineRpcBecameUnavailable,
                context,
                [handler](const QString &message) { handler(false, message); });
        },
        .requestGraphInstances = [&viewport](QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_graph_instances"), QJsonObject{}, error);
        },
        .requestGraphCatalog = [&viewport](QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_graph_catalog"), QJsonObject{}, error);
        },
        .requestGraphValues = [&viewport](const QString &instance,
                                          QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_graph_values"),
                QJsonObject{{QStringLiteral("instance"), instance}}, error);
        },
        .result = [&viewport](QObject *context,
                              GraphCanvasReadCapability::ResultHandler handler) {
            QObject::connect(
                &viewport, &EmbeddedViewport::inspectorRpcSucceeded, context,
                [handler = std::move(handler)](qint64 request_id,
                                               const QByteArray &result_json) {
                    handler(request_id, result_json);
                });
        },
        .failure = [&viewport](QObject *context,
                               GraphCanvasReadCapability::FailureHandler handler) {
            QObject::connect(
                &viewport, &EmbeddedViewport::inspectorRpcFailed, context,
                [handler = std::move(handler)](qint64 request_id,
                                               const QString &message) {
                    handler(request_id, message);
                });
        },
    };
}

} // namespace PelicanStudio
