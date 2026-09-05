#pragma once
#include <QDialog>
#include <functional>
#include <memory>
namespace PelicanStudio {
class EmbeddedViewport;
class ProceduralAssetDialog final : public QDialog {
  struct Impl;
  std::unique_ptr<Impl> impl_;

public:
  ProceduralAssetDialog(EmbeddedViewport &viewport, QWidget *parent,
                        std::function<void()> committed);
  ~ProceduralAssetDialog() override;
};
} // namespace PelicanStudio
