#pragma once

#include <QWidget>

#include <memory>

class QByteArray;

namespace PelicanStudio {

class EmbeddedViewport;

class FramePlanWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit FramePlanWidget(EmbeddedViewport *viewport,
                             QWidget *parent = nullptr);
    ~FramePlanWidget() override;

    // Shared ingestion boundary for the production RPC callback and
    // deterministic widget tests.  Successful input always flows through the
    // same populate() path.
    void receiveResult(const QByteArray &result_json);
};

} // namespace PelicanStudio
