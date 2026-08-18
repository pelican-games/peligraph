#pragma once

#include <QWidget>
#include <QtGlobal>

#include <filesystem>
#include <functional>
#include <memory>

class QByteArray;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

struct FramePlanRefreshDriver {
    std::function<bool()> ready;
    std::function<qint64(QString *)> request;
};

class FullscreenPassWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit FullscreenPassWidget(EmbeddedViewport *viewport,
                                  QWidget *parent = nullptr);
    explicit FullscreenPassWidget(FramePlanRefreshDriver refresh_driver,
                                  QWidget *parent = nullptr);
    ~FullscreenPassWidget() override;

    // These ingestion boundaries are shared by the production RPC/project
    // path and deterministic widget tests. They only replace in-memory form
    // context; neither function persists or applies the draft.
    void receiveResult(const QByteArray &result_json);
    void receiveRefreshResult(qint64 request_id,
                              const QByteArray &result_json);
    void receiveRefreshFailure(qint64 request_id, const QString &message);
    void setRefreshAvailable(bool available, const QString &reason = {});
    void receiveAuthoringConfig(const QByteArray &config_json);
    void openProjectReadOnly(const std::filesystem::path &project_root);
};

} // namespace PelicanStudio
