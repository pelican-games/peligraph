#pragma once

#include "frameplanreadcapability.hpp"

#include <QWidget>

#include <filesystem>
#include <memory>

class QByteArray;
class QString;

namespace Pelican {
struct PassShapePolicy;
}

namespace PelicanStudio {

class FullscreenPassWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit FullscreenPassWidget(FramePlanReadCapability frame_plan,
                                  QWidget *parent = nullptr);
    FullscreenPassWidget(
        FramePlanReadCapability frame_plan,
        const Pelican::PassShapePolicy &shape_policy,
        QWidget *parent = nullptr);
    ~FullscreenPassWidget() override;

    // These ingestion boundaries are shared by the production RPC/project
    // path and deterministic widget tests. They only replace in-memory form
    // context; neither function persists or applies the draft.
    void receiveResult(const QByteArray &result_json);
    void receiveAuthoringConfig(const QByteArray &config_json);
    void openProjectReadOnly(const std::filesystem::path &project_root);
};

} // namespace PelicanStudio
