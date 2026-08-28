#pragma once

#include "../toollayoutpreset.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>

class QByteArray;

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
    void setProjectRoot(const std::filesystem::path &project_root);
    void setShaderSourceOpenAction(ShaderSourceOpenAction action);

    ToolLayoutSnapshot toolLayoutSnapshot() const;
    bool restoreToolLayout(const ToolLayoutSnapshot &snapshot);
    void applyDefaultToolLayout();
};

} // namespace PelicanStudio
