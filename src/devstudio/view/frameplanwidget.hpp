#pragma once

#include "../model/frameplanmodel.hpp"
#include "../toollayoutpreset.hpp"
#include "frameplaneditcapability.hpp"
#include "shaderdiagnostics.hpp"

#include <QWidget>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

class QAction;
class QByteArray;
class QHideEvent;
class QShowEvent;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;
class RenderConfigDigest;
struct ShaderReloadOutcome;

class FramePlanWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    using ShaderSourceOpenAction =
        std::function<bool(const std::filesystem::path &)>;

    explicit FramePlanWidget(EmbeddedViewport *viewport,
                             QWidget *parent = nullptr);
    ~FramePlanWidget() override;

    // Shared ingestion boundary for the production RPC callback and
    // deterministic widget tests.  Successful input always flows through the
    // same populate() path.
    void receiveResult(const QByteArray &result_json);
    void receiveGpuTimingResult(const QByteArray &result_json);

    // source_open_ref remains a portable logical reference on the wire. The
    // widget materializes it with the same pelican_project resolver only when
    // the user presses Open.
    void refresh();
    // Q-18. Another panel's edit committed and can have changed the plan (the
    // Graph tab's, other than a placement): read it again now when in view --
    // after a read already in flight, which may predate the commit -- or
    // when next shown.
    void planChangedByCommit();
    // Q-18. What asks the engine for the plan (get_frame_plan). The embedded
    // viewport's RPC unless named here; a test names one to count the reads,
    // and hands the replies in through the viewport's inspectorRpcSucceeded.
    void setPlanRequest(std::function<qint64(QString *error)> request);
    void openTasksForHitGroup(const QString &group);
    void setNodeOrderAction(std::function<void(const QString &, const QStringList &, quint64)> action);
    void setTaskConnectionAction(std::function<void(const QString &, const QStringList &)> action);
    void setPresetPassConnectionAction(std::function<void(const QString &, const QString &, const QStringList &)> action);
    void setTaskEditAction(std::function<void(const QString &)> action);
    // Q-12. "Open in Graph" on a task a render.compute node made: the scope
    // ("root" or a definition), the node id written there and the instance
    // path (MG-3b's graph_node). Answers false when the Graph tab could not
    // show that node, and the status line then says so.
    using GraphNodeOpenAction = std::function<bool(
        const QString &scope, const QString &node, const QString &instance)>;
    void setGraphNodeOpenAction(GraphNodeOpenAction action);
    // LP-4c, decision 10. "Open in Graph" on a pass or task a graph.loop
    // expanded: the loop, the element and the body node its loop_origin names.
    using GraphLoopNodeOpenAction = std::function<bool(
        const QString &loop, qint64 element, const QString &node)>;
    void setGraphLoopNodeOpenAction(GraphLoopNodeOpenAction action);
    // GL-1, decision 6. "Open in Graph" on any other pass: the Graph tab
    // selects the node the pass is drawn under (a graph node, its feature's
    // folded node, or its own). False when the Graph tab could not.
    using GraphPassOpenAction = std::function<bool(const QString &pass)>;
    void setGraphPassOpenAction(GraphPassOpenAction action);
    // GL-5c, decisions 1 and 2. On a root that is a graph and selects no
    // preset (the context's config_kind "direct", as Blank's), Add material
    // pass... / Add fullscreen pass... / Add compute task... hand `action`
    // the palette's op (render.material_pass, render.fullscreen,
    // render.compute): the Graph tab places it as its palette does. False
    // when the Graph tab cannot (its root is not a graph): the Frame Plan's
    // own form opens, as it does on a root that selects a preset.
    using GraphPlaceAction = std::function<bool(const QString &op)>;
    void setGraphPlaceAction(GraphPlaceAction action);
    // GL-5c, decision 3. Whether the Graph tab edits a pass of the plan (a
    // graph node's, or the rendering section's in its detail column): Edit
    // pass and a double-click then open it there, through Open in Graph,
    // instead of the Pass Editor. Unset: the Pass Editor, as before.
    void setGraphEditsPassQuery(std::function<bool(const QString &pass)> query);
    // GL-2b, decision 3. The Graph tab's Layers row names a plan node: the
    // Logical graph comes forward with it picked (its group opened).
    void showPlanNode(const QString &node);
    // UX-11, decision 4: the Materials tab in front with the rows whose
    // target pass is `pass` picked (the Graph's Materials drawn).
    void showMaterialRoutes(const QString &pass);
    // LP-4d2, decision 5: the Graph tab's Show in Frame Plan. The plan is read
    // again (the loop was just edited), and once it is here the loop's group
    // is opened with its copies picked. The status line says so when the
    // plan has no copies of that loop.
    void showLoopGroup(const QString &loop);
    // LP-4e. "Decompose into loop..." on a material pass -- the Frame Plan's
    // entry and the Pass Editor's button both come here: the confirmation,
    // then decompose_material_pass. Once it commits, `action` hears the loop
    // it made, which the Graph tab then shows.
    void decomposeMaterialPass(const QString &graph, const QString &pass);
    void setLoopDecomposedAction(std::function<void(const QString &loop)> action);
    // LP-4f2. "Replace with loop version..." on a feature's group: `offered`
    // says whether the feature (its reference) has one -- nullopt: no entry;
    // empty: offered; else the reason it cannot now -- and `replace` takes it
    // in. Both are the Features tab's.
    void setLoopVersionActions(
        std::function<std::optional<QString>(const QString &feature)> offered,
        std::function<void(const QString &feature)> replace);
    void setPresetPassEditAction(std::function<void(const QString &, const QString &)> action);
    // N-8. What the canvas is allowed to change, and where its placement is
    // kept. Without these the canvas is exactly the read-only projection it was
    // before, and every editing gesture says so in one line.
    void setEditCapability(FramePlanEditCapability capability);
    void setLayoutCapability(FramePlanLayoutCapability capability);
    // "Add fullscreen pass..." opens the Pass Editor; "Add compute task..."
    // opens the task dialog. Both are the existing forms, opened after a node.
    void setPassInsertAction(
        std::function<void(const QString &graph, const QString &after)> action);
    void setTaskInsertAction(std::function<void()> action);
    // The bands, the notices and the placement all want a fresh authoring
    // context; this is the one place that asks for it.
    void refreshAuthoringContext();
    // Q-13. The rendering document's current digest, shared with every panel
    // that edits it: this panel says what it read and committed, and reads its
    // context again when another panel's commit moved the document.
    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest);
    // RU-1. The render edit history's Undo / Redo (the main window owns the
    // history): Undo and Redo buttons on the toolbar, and the actions' keys
    // while this panel holds the focus. showRenderHistoryLine puts the
    // history's one line on the status row.
    void setRenderHistoryActions(QAction *undo, QAction *redo);
    void showRenderHistoryLine(const QString &line);
    void setProjectRoot(const std::filesystem::path &project_root);
    void setShaderSourceOpenAction(ShaderSourceOpenAction action);
    // F-3. A get_status result: its shader reload failures become marks on
    // the nodes whose shader they name. The widget reads get_status itself
    // only after a watched shader file changes; this is the same ingestion,
    // for tests and for a caller that already holds a reply.
    // A failure no drawn node reads is one line on the panel's status line.
    void receiveShaderReloadStatus(const QByteArray &status_json);
    // F-5. A write to a watched shader file asks the engine to recompile
    // (reload_shaders). What it refuses with is shown here as marks, and
    // handed to this action as the whole log -- empty text when a reload
    // committed and the log should go.
    void setShaderReloadLogAction(std::function<void(const QString &)> action);
    // G-4 / G-5. A commit that answers restart_required hands the restart to
    // this action (the window asks about unsaved scene edits first); without
    // one, the viewport restarts the engine at once. When the restart is
    // declined, engineRestartDeclined says why on the status row and reads
    // the plan the running engine now has.
    void setEngineRestartAction(std::function<void(const QString &reason)> action);
    void engineRestartDeclined(const QString &message);
    // T-1. New render target..., opened with a name already in it (the Pass
    // Editor's "New target: <pass>_color..."), and who hears that a target
    // this panel's form created has committed.
    void openNewRenderTarget(const QString &name);
    void setRenderTargetCreatedAction(std::function<void(const QString &name)> action);
    // T-1. What a node's band editor offers to complete to, by the band's
    // field id ("shader.fragment", "shader.vertex"). Nothing without it.
    void setInlineFieldChoices(std::function<QStringList(const QString &field)> choices);
    // The answer to a reload_shaders request, handed in the way the RPC
    // reply is (tests use it without an engine).
    void receiveShaderReloadOutcome(const ShaderReloadOutcome &outcome);
    // UX-6a, decision 2: this panel is the one sender of reload_shaders.
    // It also follows the shader files `sources` names (the Graph tab's
    // passes, by node), whether or not it has read a plan or is in front,
    // and calls `reloaded` after a reload committed or was refused for a
    // pass -- the declarations, and so the sockets, may have moved.
    void setExtraShaderSources(NodeShaderSources sources);
    void setShadersReloadedAction(std::function<void()> reloaded);
    // RL-1s, decision 1: the render documents the window watches go into
    // the same watcher as the shaders (one watcher, not two); a write to
    // one calls `changed` with its path and never asks for reload_shaders.
    void setWatchedRenderDocuments(std::vector<std::filesystem::path> files,
                                   std::function<void(const QString &path)> changed);
    // HW-2s, decision 7: a get_status any panel read. Whether the engine's
    // own watch is open (project.json's hot_reload) decides how a
    // reload_shaders that found nothing new is read.
    void observeEngineStatus(const nlohmann::json &status);
    // UX-6a, decision 4: the Physical plan's Remove on an image the planner
    // keeps out of aliasing because it has TRANSFER_SRC. The action takes
    // the flag off the image (the Graph tab's document): the resource, and
    // where a loop's copy came from; false when it could not.
    using DropTransferSourceAction = std::function<bool(
        const QString &resource, const std::optional<FramePlanLoopOrigin> &origin)>;
    void setDropTransferSourceAction(DropTransferSourceAction action);
    // UX-6b, decisions 1 and 3: beside the status line, the frame's GPU time
    // and the plan's size, and what the last edit moved (the window's
    // PlanPerfHub reads them). The labels stay, empty or not.
    void setPerfLines(const QString &summary, const QString &delta);
    // VI-3b. Fix sockets on one pass, as the band's Fix button does: the
    // variadic group of its shader becomes one fixed declaration per
    // connected input (engine://passthrough is copied into the project
    // first). The Pass Editor's Fix sockets comes here.
    void fixSockets(const QString &graph, const QString &pass);
    // LP-4b. The view axes (view_axes) of the node that runs `task` in the
    // current frame plan, for the Task Editor's View field. Empty when the
    // plan holds no such task or its family has no axes.
    [[nodiscard]] std::vector<FramePlanViewAxis> taskViewAxes(
        const QString &task) const;

    ToolLayoutSnapshot toolLayoutSnapshot() const;

  protected:
    // N-8: the two authoring reads the canvas needs are asked for when this
    // panel comes to the front, not while it is behind another tab.

  public:
    bool restoreToolLayout(const ToolLayoutSnapshot &snapshot);
    void applyDefaultToolLayout();

  protected:
    // GPU timing is a number on this panel. A panel nobody can see is not
    // worth asking the engine's render thread about once a second.
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
};

} // namespace PelicanStudio
