#include "graphcanvasreadcapability.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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
        .requestGraphNodeValues = [&viewport](const QString &instance,
                                              QString *error) {
            // The root scope asks the question without the parameter: an
            // empty `instance` and an omitted one mean the same thing to the
            // engine, and leaving it out is what the RPC's own contract says.
            QJsonObject params;
            if (!instance.isEmpty())
                params.insert(QStringLiteral("instance"), instance);
            return viewport.requestRpc(
                QStringLiteral("get_graph_node_values"), params, error);
        },
        .requestStatus = [&viewport](QString *error) {
            return viewport.requestRpc(QStringLiteral("get_status"),
                                       QJsonObject{}, error);
        },
        .requestSetTime = [&viewport](double time, QString *error) {
            return viewport.requestRpc(
                QStringLiteral("set_time"),
                QJsonObject{{QStringLiteral("t"), time}}, error);
        },
        .requestRenderFrame = [&viewport](QString *error) {
            return viewport.requestRpc(QStringLiteral("render_frame"),
                                       QJsonObject{}, error);
        },
        .requestEditGraphDocument = [&viewport](const QString &digest,
                                                const QByteArray &ops_json,
                                                QString *error) {
            // The ops are built as JSON by the model, so they cross into Qt's
            // transport by being parsed back rather than rebuilt by hand: a
            // second spelling of the same op list is a second thing to get
            // wrong.
            QJsonParseError parsed{};
            const auto document = QJsonDocument::fromJson(ops_json, &parsed);
            if (parsed.error != QJsonParseError::NoError || !document.isArray()) {
                if (error != nullptr)
                    *error = QObject::tr("The graph edit could not be encoded: %1")
                                 .arg(parsed.errorString());
                return qint64{0};
            }
            return viewport.requestRpc(
                QStringLiteral("edit_graph_document"),
                QJsonObject{{QStringLiteral("base_source_digest"), digest},
                            {QStringLiteral("ops"), document.array()}},
                error);
        },
        .requestEditResult = [&viewport](const QString &ticket, QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_edit_result"),
                QJsonObject{{QStringLiteral("ticket"), ticket}}, error);
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
