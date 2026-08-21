#pragma once

#include "frameplanreadcapability.hpp"
#include "renderpassauthoringcapability.hpp"

#include <QWidget>

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
    void receiveResult(const QByteArray &result_json);
    void receiveAuthoringConfig(const QByteArray &config_json);
};

} // namespace PelicanStudio
