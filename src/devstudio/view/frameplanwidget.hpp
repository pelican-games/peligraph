#pragma once

#include "../toollayoutpreset.hpp"

#include <QWidget>
#include <QStringList>

#include <filesystem>
#include <functional>
#include <memory>

class QByteArray;
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
    void setProjectRoot(const std::filesystem::path &project_root);
    void setShaderSourceOpenAction(ShaderSourceOpenAction action);

    ToolLayoutSnapshot toolLayoutSnapshot() const;
    bool restoreToolLayout(const ToolLayoutSnapshot &snapshot);
    void applyDefaultToolLayout();
};

} // namespace PelicanStudio
