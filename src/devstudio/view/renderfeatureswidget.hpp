#pragma once

#include <QWidget>

#include <memory>

class QByteArray;

namespace PelicanStudio {

class EmbeddedViewport;

// RPC-only Studio surface for the engine-owned authored config features[].
// It deliberately has no pelican_core types or feature-name constants.
class RenderFeaturesWidget final : public QWidget {
    Q_OBJECT

    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit RenderFeaturesWidget(
        EmbeddedViewport *viewport,
        QWidget *parent = nullptr);
    ~RenderFeaturesWidget() override;

    // Applies the successful list_render_features payload. Production RPC
    // delivery and focused widget tests intentionally share this entry.
    void receiveCatalogResult(const QByteArray &result_json);
};

} // namespace PelicanStudio
