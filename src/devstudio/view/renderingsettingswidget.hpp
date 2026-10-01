#pragma once

#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QAction;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;
class RenderConfigDigest;

// RSX, decision 3. The render tools' Rendering settings tab: the keys of the
// root's `rendering` section that no other panel writes, each as a field
// (EL-3's edit_rendering_keys). draw_sort, render_strategy, resolver_version
// and multisampling are chosen from a list; shader_defines is a table of
// names and values (UX-4a's ShaderDefinesForm); snapshots, graph_transforms
// and xr are JSON for now. A change sends one edit_rendering_keys with the
// key in `set` (each value whole), or in `remove` when the field goes back to
// the engine's default.
//
// The values are read from the root itself (get_render_documents, the
// document whose kind is "root"). A root that selects a preset takes neither
// multisampling (pipeline.settings.msaa holds it) nor resolver_version: those
// fields are switched off with the engine's reason as their tooltip, and a
// field the engine refuses as rendering_key_not_allowed_here is switched off
// the same way.
//
// The commit is read the way every panel reads one (rendereditcompletion):
// restart_required hands the restart to the main window. The edits are the
// render edit history's (Undo / Redo).
class RenderingSettingsWidget final : public QWidget {
    Q_OBJECT

    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit RenderingSettingsWidget(EmbeddedViewport *viewport, QWidget *parent = nullptr);
    ~RenderingSettingsWidget() override;

    // The keys edit_rendering_keys writes, in the order the panel shows them.
    [[nodiscard]] static const std::vector<std::string> &editableKeys();

    // Q-13. The rendering document's digest, shared with the other panels
    // that edit it: another panel's commit (or an Undo) reads the root again.
    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest);
    // RU-1. The render edit history's Undo / Redo keys work in this panel.
    void setRenderHistoryActions(const QList<QAction *> &actions);
    // RSX, decision 1. A commit that answers restart_required hands the
    // restart to this action; engineRestartDeclined says why it did not
    // happen and reads the root from the engine that stays.
    void setEngineRestartAction(std::function<void(const QString &reason)> action);
    void engineRestartDeclined(const QString &message);
    // Reads the root again (Refresh).
    void refresh();
};

} // namespace PelicanStudio
