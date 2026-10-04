#pragma once

#include <QWidget>

#include <functional>
#include <memory>

class QString;

namespace PelicanStudio {

class EmbeddedViewport;
class RenderConfigDigest;

// RTL-2s (a). The Ray layout section of the Rendering settings tab: the
// project's ray group layout (engine RTL-1a / RTL-1b) as tables -- Ray types,
// Hit groups, Miss shaders, Record fields and the engine record -- and its
// Hit group assignments (the bindings), each change one edit_batch with every
// ray tracing task that uses the names it changes (raylayoutmodel).
//
// A project without a ray layout shows one line and Enable hit groups, which
// writes the default layout and every task in its form in one batch; the
// engine runs it after a restart (RS-1's restart). A layout that cannot be
// read shows its error and Show the document (BS-1s's restore).
//
// The edits are the render edit history's (Undo / Redo), except one that
// rewrites procedural assets' records (asset data, which no restore takes
// back): the section's own Undo sends the records before it again.
class RayLayoutWidget final : public QWidget {
    Q_OBJECT

    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit RayLayoutWidget(EmbeddedViewport *viewport, QWidget *parent = nullptr);
    ~RayLayoutWidget() override;

    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest);
    // A commit that answers restart_required hands the restart here.
    void setEngineRestartAction(std::function<void(const QString &reason)> action);
    void engineRestartDeclined(const QString &message);
    // Decision 6: Show the document (the render document repair's dialog).
    void setShowDocumentAction(std::function<void(const QString &reference)> action);
    // Decision 5: the Task Editor's acceleration structure settings.
    void setAccelerationStructuresAction(std::function<void()> action);
    // Decision 4: brings the assignment row of the material `material`
    // (a document path, or a name) forward and opens its hit group list.
    void revealMaterial(const QString &material);
    void refresh();
};

} // namespace PelicanStudio
