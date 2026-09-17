#pragma once

#include "../toollayoutpreset.hpp"
#include "frameplaneditcapability.hpp"

#include <QWidget>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>

class QByteArray;
class QShowEvent;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

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
    void openTasksForHitGroup(const QString &group);
    void setNodeOrderAction(std::function<void(const QString &, const QStringList &, quint64)> action);
    void setTaskConnectionAction(std::function<void(const QString &, const QStringList &)> action);
    void setPresetPassConnectionAction(std::function<void(const QString &, const QString &, const QStringList &)> action);
    void setTaskEditAction(std::function<void(const QString &)> action);
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
    void setProjectRoot(const std::filesystem::path &project_root);
    void setShaderSourceOpenAction(ShaderSourceOpenAction action);

    ToolLayoutSnapshot toolLayoutSnapshot() const;

  protected:
    // N-8: the two authoring reads the canvas needs are asked for when this
    // panel comes to the front, not while it is behind another tab.
    void showEvent(QShowEvent *event) override;

  public:
    bool restoreToolLayout(const ToolLayoutSnapshot &snapshot);
    void applyDefaultToolLayout();
};

} // namespace PelicanStudio
