#pragma once

#include "../model/gizmomodel.hpp"
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
    std::optional<GizmoTransformBinding> gizmoTransformBinding() const;
    bool beginGizmoEdit(std::string_view field_key);
    bool previewGizmoEdit(std::string_view field_key,
                          nlohmann::json value);
    void finishGizmoEdit(std::string_view field_key, bool commit);
    bool saveScene();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace PelicanStudio
