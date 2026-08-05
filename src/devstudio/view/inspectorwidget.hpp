#pragma once

#include "../model/project.hpp"

#include <QWidget>

#include <memory>
#include <optional>

class QEvent;
class QObject;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

class InspectorWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit InspectorWidget(EmbeddedViewport *viewport,
                             QWidget *parent = nullptr);
    ~InspectorWidget() override;

    void setSelection(std::optional<OutlinerObjectKey> selection,
                      const QString &display_text);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace PelicanStudio
