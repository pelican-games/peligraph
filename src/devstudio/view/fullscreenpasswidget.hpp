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
    void setShaderSourceOpenAction(
        std::function<bool(const std::filesystem::path &)> action);
    // F-5. A shader reload the engine refused: its log goes under the form
    // while the Pass Editor is open. Empty text takes down a log that a
    // reload put there (a reload that committed).
    void showShaderReloadLog(const QString &message);
    void receiveResult(const QByteArray &result_json);
    void receiveAuthoringConfig(const QByteArray &config_json);
};

} // namespace PelicanStudio
