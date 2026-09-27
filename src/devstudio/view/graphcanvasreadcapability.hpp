#pragma once

#include <QtGlobal>

#include <functional>

class QByteArray;
class QObject;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

// The asynchronous surface the common-graph canvas needs. It names the five
// reads and the three writes it is allowed to make and deliberately cannot
// express a generic RPC method, so nothing else can leave through this seam.
//
// The reads are the three the document needs (instances, catalog, instance
// values) plus the two AG-3b observes with: get_graph_node_values, which is
// the only report that reaches inside a definition body, and get_status, which
// carries the scene time the Timeline's axis is and the animation sinks its
// band is.
//
// The writes are three, and they are three on purpose. edit_graph_document is
// the document edit: the canvas cannot attach an instance, fork a preset or
// touch a pass from here. set_time and render_frame are the scrub, and they
// are named separately rather than folded into one "scrub" call so that what
// leaves the Studio when a person drags the Timeline is two RPCs a reader can
// count -- O-1's reading of going to a time, and nothing else.
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
    // AG-3a's node values. An empty instance is the root scope's question,
    // which the RPC spells by leaving the parameter out.
    std::function<qint64(const QString &instance, QString *error)>
        requestGraphNodeValues;
    std::function<qint64(QString *error)> requestStatus;
    // LP-4c: get_frame_plan, whose loops[] say how many elements each loop
    // expanded and which branch each took. The canvas reads nothing else of
    // the plan, and asks only when the document holds a loop.
    std::function<qint64(QString *error)> requestFramePlan;
    // LP-4e: one more page of a draw_loop's materials (get_frame_plan
    // {loop_elements: {node, offset, limit}}); loops[] is again all it reads.
    std::function<qint64(const QString &node, qint64 offset, qint64 limit, QString *error)>
        requestLoopElements;
    // The scrub, as two explicit writes. Nothing else on this canvas moves the
    // engine's clock or asks it to draw.
    std::function<qint64(double time, QString *error)> requestSetTime;
    std::function<qint64(QString *error)> requestRenderFrame;
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
