#pragma once

#include "../model/materialsmodel.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace PelicanStudio {

// The list's rows carry the document's path and, on a material's row, its
// name.
inline constexpr int MaterialPathRole = Qt::UserRole + 1;
inline constexpr int MaterialNameRole = Qt::UserRole + 2;

// MT-1s. The Materials dock (beside Assets, View > Materials): the project's
// material documents and their materials above (list_materials; a document
// that is missing or does not load is a grey row with the reason), and the
// selected material's editor below -- its surface, tags, routing, then one
// row per param and texture slot of the surface's declaration
// (get_surface_schema). New material… makes one on a surface the engine
// lists (list_surfaces), and Assign to selected object binds the material to
// every primitive of the selected object's model. MaterialsDockSession holds
// what the engine said; this is how it is shown.
class MaterialsDock final : public QWidget {
  public:
    struct SelectedObject {
        std::string model;
        std::string name;
    };
    struct Hooks {
        // Sends one editor request; the id its reply comes back under, or 0
        // with `error` saying why.
        std::function<qint64(const QString &method, const QJsonObject &params, QString *error)> send;
        // An edit's restart_required: the window's engine restart.
        std::function<void(const QString &reason)> restart;
        // The object selected in the Outliner, when it shows a model.
        std::function<std::optional<SelectedObject>()> selected_object;
        // The list of materials changed (a material made, edited outside).
        std::function<void()> materials_changed;
        // A bind_material of this dock committed.
        std::function<void(const std::string &model)> bound;
    };

    explicit MaterialsDock(Hooks hooks, QWidget *parent = nullptr);
    ~MaterialsDock() override;

    void open(const std::filesystem::path &project_root);
    void engineAvailable();
    void engineUnavailable();
    // The Outliner's selection moved: Assign to selected object follows it.
    void selectedObjectChanged();

    // The replies to what this dock sent. False for an id that is not its own.
    bool receiveResult(qint64 request_id, const QByteArray &result_json);
    bool receiveRefusal(qint64 request_id, int code, const QString &message, const QJsonValue &data);
    bool receiveFailure(qint64 request_id, const QString &message);

    [[nodiscard]] MaterialsDockSession &session() noexcept;
    [[nodiscard]] QTreeWidget *list() const noexcept;
    [[nodiscard]] QAction *undoAction() const noexcept;
    [[nodiscard]] QLabel *noticeLine() const noexcept;

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
