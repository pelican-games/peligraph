#pragma once

#include <QWidget>

#include <memory>

namespace PelicanStudio {

class EmbeddedViewport;

class FramePlanWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit FramePlanWidget(EmbeddedViewport *viewport,
                             QWidget *parent = nullptr);
    ~FramePlanWidget() override;
};

} // namespace PelicanStudio
