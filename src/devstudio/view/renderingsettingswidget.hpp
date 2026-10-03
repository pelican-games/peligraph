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
// names and values (UX-4a's ShaderDefinesForm). UX-10: snapshots are rows (a
// name, where it is taken, x, and + while there is none: v1 takes one), xr's
// view_execution is a combo, and material_route_default (Auto /
// Forward) is sent as set_material_route_default. UX-10, decision 7:
// draw_sort's and render_strategy's entries are made from
// get_registered_providers (asked with each read; a game's provider is
// marked), the fixed ones kept for an engine without it. A change sends one
// edit_rendering_keys with the key in `set` (each value whole), or in
// `remove` when the field goes back to the engine's default.
//
// UX-15a: no key is typed as JSON any more. render_strategy's provider is
// picked and its required name filled with the provider's (kept when the
// root has one), beside a name field and its parameters as one JSON line;
// draw_sort's xr_view_policy (Logical view center / Per view) is a combo of
// its own; graph_transforms is a row each (provider, name, parameters, up /
// down, x, and + to add one) with the whole value read-only under Advanced;
// xr.multiview_auto is a check with its minimum_gain_percent a spin, and xr
// whole is JSON under Advanced with Apply (its profiles); the check, the
// spin and the JSON write the same key and each commit is read back into
// all of them. A root
// that selects a preset shows what the preset's config holds for
// shader_defines, snapshots, draw_sort and graph_transforms as grey rows
// above the root's own (read from get_render_documents' preset document,
// which the engine lists for a project preset only; nothing writes them
// here).
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
