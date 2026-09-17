#include "frameplaneditcapability.hpp"

#include "../viewport/embeddedviewport.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <utility>

namespace PelicanStudio {

FramePlanEditCapability framePlanEditCapability(EmbeddedViewport &viewport) {
    const auto send = [&viewport](const char *method) {
        return [&viewport, method](const QJsonObject &params, QString *error) {
            return viewport.requestRpc(QString::fromLatin1(method), params,
                                       error);
        };
    };
    return FramePlanEditCapability{
        .ready = [&viewport](QObject *context,
                             FramePlanEditCapability::ReadyHandler handler) {
            handler(viewport.rpcReady(), {});
            QObject::connect(&viewport,
                             &EmbeddedViewport::engineRpcBecameAvailable,
                             context, [handler] { handler(true, {}); });
            QObject::connect(&viewport,
                             &EmbeddedViewport::engineRpcBecameUnavailable,
                             context, [handler](const QString &message) {
                                 handler(false, message);
                             });
        },
        .requestAuthoringContext = [&viewport](QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_render_authoring_context"), QJsonObject{},
                error);
        },
        .requestTaskAuthoring = [&viewport](QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_compute_task_authoring"), QJsonObject{},
                error);
        },
        .editPresetPass = send("edit_preset_pass"),
        .removeAuthoredPass = send("remove_authored_pass"),
        .editComputeTask = send("edit_compute_task"),
        .removeComputeTask = send("remove_compute_task"),
        .requestEditResult = [&viewport](const QString &ticket, QString *error) {
            return viewport.requestRpc(
                QStringLiteral("get_edit_result"),
                QJsonObject{{QStringLiteral("ticket"), ticket}}, error);
        },
        .result = [&viewport](QObject *context,
                              FramePlanEditCapability::ResultHandler handler) {
            QObject::connect(&viewport,
                             &EmbeddedViewport::inspectorRpcSucceeded, context,
                             [handler = std::move(handler)](
                                 qint64 request_id,
                                 const QByteArray &result_json) {
                                 handler(request_id, result_json);
                             });
        },
        .failure = [&viewport](QObject *context,
                               FramePlanEditCapability::FailureHandler handler) {
            QObject::connect(&viewport, &EmbeddedViewport::inspectorRpcFailed,
                             context,
                             [handler = std::move(handler)](
                                 qint64 request_id, const QString &message) {
                                 handler(request_id, message);
                             });
        },
    };
}

} // namespace PelicanStudio
