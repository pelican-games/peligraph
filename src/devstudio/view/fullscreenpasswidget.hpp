#pragma once

#include "frameplanreadcapability.hpp"
#include "renderpassauthoringcapability.hpp"

#include <QWidget>
#include <QStringList>

#include <filesystem>
#include <memory>
#include <functional>

class QByteArray;
class QString;

namespace Pelican {
struct PassShapePolicy;
}

namespace PelicanStudio {

class RenderConfigDigest;

class FullscreenPassWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit FullscreenPassWidget(FramePlanReadCapability frame_plan,
                                  QWidget *parent = nullptr);
    FullscreenPassWidget(
        FramePlanReadCapability frame_plan,
        RenderPassAuthoringCapability authoring,
        QWidget *parent = nullptr);
    FullscreenPassWidget(
        FramePlanReadCapability frame_plan,
        const Pelican::PassShapePolicy &shape_policy,
        QWidget *parent = nullptr);
    FullscreenPassWidget(
        FramePlanReadCapability frame_plan,
        RenderPassAuthoringCapability authoring,
        const Pelican::PassShapePolicy &shape_policy,
        QWidget *parent = nullptr);
    ~FullscreenPassWidget() override;

    // Deterministic projection-test boundaries. Production authoring context
    // arrives only through RenderPassAuthoringCapability.
    void setCommittedAction(std::function<void()> action);
    // RSX, decision 1. A commit that answers restart_required hands the
    // restart to this action (the main window's requestEngineRestart) and
    // the status row says "Restarting the engine to apply this change...".
    void setEngineRestartAction(std::function<void(const QString &reason)> action);
    void editPresetPass(const QString &graph, const QString &pass_name);
    // N-8. "Add fullscreen pass..." on the Frame Plan canvas: the same form,
    // opened for a new pass, with the insert position already standing after
    // the node the user right-clicked. Nothing else is filled in -- the name
    // and the shaders are the author's.
    void insertAuthoredPassAfter(const QString &graph, const QString &after_pass);
    void editPresetPassConnection(const QString &graph, const QString &pass_name, const QStringList &outputs);
    // F-3. "New..." beside the fragment stem writes <root>/shaders/<name>.frag
    // and opens it with this action (the desktop's own by default).
    void setProjectRoot(const std::filesystem::path &project_root);
    // VI-3b. Fix sockets beside the fragment, shown for a pass whose shader
    // has a variadic group: the Frame Plan rewrites the shader.
    void setFixSocketsAction(
        std::function<void(const QString &graph, const QString &pass)> action);
    // LP-4e. "Decompose into loop..." under a material pass's contract: the
    // Frame Plan's entry, on the pass being edited.
    void setDecomposeAction(
        std::function<void(const QString &graph, const QString &pass)> action);
    void setShaderSourceOpenAction(
        std::function<bool(const std::filesystem::path &)> action);
    // F-5. A shader reload the engine refused: its log goes under the form
    // while the Pass Editor is open. Empty text takes down a log that a
    // reload put there (a reload that committed).
    void showShaderReloadLog(const QString &message);
    // T-1. The colour output's list ends with "New target: <pass>_color...";
    // picking it calls this action with that name (the main window opens the
    // Frame Plan's New render target form on it). When the target it names
    // is made, renderTargetCreated brings the frame plan in again and the
    // target becomes the draft's next colour output.
    void setNewRenderTargetAction(std::function<void(const QString &name)> action);
    void renderTargetCreated(const QString &name);
    void receiveResult(const QByteArray &result_json);
    void receiveAuthoringConfig(const QByteArray &config_json);
    // Q-13. The rendering document's digest, shared with the other panels
    // that edit it. Another panel's commit reads the context again and keeps
    // the form as the author left it; an edit refused for a moved document is
    // built again from that context once, and the second refusal offers
    // Reload.
    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest);
    // RU-1. An Undo or Redo of a render edit put documents back: the context
    // is read again and the pass being edited is loaded from it -- the form
    // shows what the document holds now, not the draft.
    void documentRestored();
    // GL-5e, decision 7. New pass on a root that is a graph and selects no
    // preset (Blank's): `action` is handed render.fullscreen, as the Frame
    // Plan's Add fullscreen pass... hands it, and the Graph tab places the
    // pass. False -- or no action -- keeps the form's new authored pass.
    void setGraphPlaceAction(std::function<bool(const QString &op)> action);
};

} // namespace PelicanStudio
