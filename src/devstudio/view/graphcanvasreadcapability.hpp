#pragma once

#include <QtGlobal>

#include <functional>

class QByteArray;
class QObject;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

// The asynchronous surface the common-graph canvas needs. It names the three
// reads and the one edit it is allowed to make and deliberately cannot express
// a generic RPC method, so nothing else can leave through this seam. The edit
// is edit_graph_document alone: the canvas cannot attach an instance, fork a
// preset or touch a pass from here.
struct GraphCanvasReadCapability {
    using ReadyHandler =
        std::function<void(bool available, const QString &reason)>;
    using ResultHandler =
        std::function<void(qint64 request_id, const QByteArray &result_json)>;
    using FailureHandler =
        std::function<void(qint64 request_id, const QString &message)>;

    std::function<void(QObject *context, ReadyHandler handler)> ready;
    std::function<qint64(QString *error)> requestGraphInstances;
    std::function<qint64(QString *error)> requestGraphCatalog;
    std::function<qint64(const QString &instance, QString *error)>
        requestGraphValues;
    // One gesture, one op list, one CAS. `ops_json` is the serialised array the
    // model built; the digest is the document it was built against.
    std::function<qint64(const QString &base_source_digest,
                         const QByteArray &ops_json, QString *error)>
        requestEditGraphDocument;
    std::function<qint64(const QString &ticket, QString *error)>
        requestEditResult;
    std::function<void(QObject *context, ResultHandler handler)> result;
    std::function<void(QObject *context, FailureHandler handler)> failure;
};

// Production adapter. MainWindow is the composition root that converts its
// generic EmbeddedViewport into the restricted capability above.
GraphCanvasReadCapability graphCanvasReadCapability(EmbeddedViewport &viewport);

} // namespace PelicanStudio
