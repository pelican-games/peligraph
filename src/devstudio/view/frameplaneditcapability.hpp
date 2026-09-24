#pragma once

#include <nlohmann/json.hpp>

#include <QtGlobal>

#include <functional>

class QByteArray;
class QJsonObject;
class QObject;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

// N-8. What the Frame Plan canvas is allowed to do to the document. It is a
// closed list on purpose: the canvas cannot name an RPC method, and every entry
// here is an authoring call that already existed before the canvas could edit.
struct FramePlanEditCapability {
    using ReadyHandler =
        std::function<void(bool available, const QString &reason)>;
    using ResultHandler =
        std::function<void(qint64 request_id, const QByteArray &result_json)>;
    using FailureHandler =
        std::function<void(qint64 request_id, const QString &message)>;

    std::function<void(QObject *context, ReadyHandler handler)> ready;
    std::function<qint64(QString *error)> requestAuthoringContext;
    std::function<qint64(QString *error)> requestTaskAuthoring;
    std::function<qint64(const QJsonObject &params, QString *error)>
        editPresetPass;
    std::function<qint64(const QJsonObject &params, QString *error)>
        removeAuthoredPass;
    std::function<qint64(const QJsonObject &params, QString *error)>
        editComputeTask;
    std::function<qint64(const QJsonObject &params, QString *error)>
        removeComputeTask;
    // F-1: add / replace / remove render targets in the editor's fragment.
    std::function<qint64(const QJsonObject &params, QString *error)>
        editRenderTargets;
    // G-1: "Add material pass..." adds its pass through the same call the
    // Pass Editor adds a fullscreen one with.
    std::function<qint64(const QJsonObject &params, QString *error)>
        addAuthoredPass;
    std::function<qint64(const QString &ticket, QString *error)>
        requestEditResult;
    std::function<void(QObject *context, ResultHandler handler)> result;
    std::function<void(QObject *context, FailureHandler handler)> failure;
};

FramePlanEditCapability framePlanEditCapability(EmbeddedViewport &viewport);

// Where the Frame Plan canvas keeps the places the user dragged things to. The
// document member is `layout.frame_plan`, and the write goes out through the
// common graph canvas's own debounce -- there is one `set_layout` for the whole
// root layout, not one per panel, so both halves have to travel together.
struct FramePlanLayoutCapability {
    // The `frame_plan` member of the root layout, or an empty object.
    std::function<nlohmann::json()> read;
    // Replace it and schedule the one write. False means the document cannot
    // hold a placement at all, and `reason` says why in one line.
    std::function<bool(const nlohmann::json &frame_plan, QString *reason)> write;
};

} // namespace PelicanStudio
