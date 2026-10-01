#pragma once

#include <QtGlobal>

#include <functional>

class QByteArray;
class QJsonObject;
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
// the document edit: the canvas cannot attach an instance or fork a preset
// from here. GL-1e adds one pass edit, and only one: edit_preset_pass on a
// pass of the rendering section, changing its `input` the way the Frame
// Plan's socket drop does. set_time and render_frame are the scrub, and they
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
    // GL-1: get_graph_templates, for which features have a decomposed
    // version a feature's folded node can show (read only).
    std::function<qint64(QString *error)> requestGraphTemplates;
    // UX-5b (EL-4b): get_engine_shaders, the engine's shaders and what each
    // declares -- asked only when a shader list is opened (it takes the
    // engine seconds), never on connecting.
    std::function<qint64(QString *error)> requestEngineShaders;
    // The scrub, as two explicit writes. Nothing else on this canvas moves the
    // engine's clock or asks it to draw.
    std::function<qint64(double time, QString *error)> requestSetTime;
    std::function<qint64(QString *error)> requestRenderFrame;
    // One gesture, one op list, one CAS. `ops_json` is the serialised array the
    // model built; the digest is the document it was built against. GL-1d:
    // `material_route_default` goes beside the ops when it is not empty (a
    // Forward opaque material pass added where no pass draws deferred
    // materials).
    std::function<qint64(const QString &base_source_digest,
                         const QByteArray &ops_json,
                         const QString &material_route_default, QString *error)>
        requestEditGraphDocument;
    std::function<qint64(const QString &ticket, QString *error)>
        requestEditResult;
    // GL-1e: a pass of the rendering section takes a line from the Graph.
    // The read is what the Frame Plan and the Pass Editor read the pass from;
    // the write is the edit_preset_pass they send (the whole params object
    // the shared builder made), answered with a ticket get_edit_result polls.
    // Either may be left empty: the pass then takes no lines here.
    std::function<qint64(QString *error)> requestRenderAuthoringContext;
    std::function<qint64(const QJsonObject &params, QString *error)>
        requestEditPresetPass;
    // GL-3a: the root's fold groups. get_fold_groups reads their state, the
    // root document (get_render_documents of `reference` alone) their
    // decisions, and fold_nodes writes one group (a ticket get_edit_result
    // polls). Any may be left empty: the canvas then folds nothing.
    std::function<qint64(QString *error)> requestFoldGroups;
    std::function<qint64(const QString &reference, QString *error)> requestRootDocument;
    std::function<qint64(const QJsonObject &params, QString *error)> requestFoldNodes;
    // GL-4a: verify_fold, a ticket get_edit_result polls (running for as
    // long as the engine's child runs draw).
    std::function<qint64(const QJsonObject &params, QString *error)> requestVerifyFold;
    std::function<void(QObject *context, ResultHandler handler)> result;
    std::function<void(QObject *context, FailureHandler handler)> failure;
};

// Production adapter. MainWindow is the composition root that converts its
// generic EmbeddedViewport into the restricted capability above.
GraphCanvasReadCapability graphCanvasReadCapability(EmbeddedViewport &viewport);

} // namespace PelicanStudio
