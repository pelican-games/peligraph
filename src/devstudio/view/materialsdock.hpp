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
// FO-1s: a document's row carries its status ("loaded", "missing", "invalid").
inline constexpr int MaterialStatusRole = Qt::UserRole + 3;

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
        // FO-1s, decision 3: where a refused delete or rename found the
        // material -- an object by its name (the Outliner's row), a render
        // document (its text, at `find`).
        std::function<void(const QString &object)> reveal_object;
        std::function<void(const QString &document, const QString &find)> reveal_document;
        // MR-1s, decision 1: a document came back without a restart -- the
        // problem list is read now.
        std::function<void()> problems_changed;
        // RTL-3s, decision 8: Open the pass of a document whose pass does not
        // declare what its materials use (list_materials' unusable) -- the
        // Graph tab with the pass picked, where its Problems row goes.
        std::function<void(const std::string &pass)> open_pass;
    };

    explicit MaterialsDock(Hooks hooks, QWidget *parent = nullptr);
    ~MaterialsDock() override;

    void open(const std::filesystem::path &project_root);
    void engineAvailable();
    void engineUnavailable();
    // The Outliner's selection moved: Assign to selected object follows it.
    void selectedObjectChanged();
    // PB-1s, decision 3: the material `name` of `document` (with or without
    // project://) selected, its editor below; without a name, the document's
    // row. False when the list has neither.
    bool revealMaterial(const std::string &document, const std::string &name);
    // FO-1s, decisions 2 and 3: the bindings of `model` (every primitive's
    // material, bind_material) in the row pelican.materials.model, with
    // `line` above them (what is wrong with them, when something is).
    void revealModel(const std::string &model, const std::string &line);
    // MR-1s, decisions 2 and 3: the engine's problem list moved (PB-1s's
    // rows, through materialProblemsOf).
    void observeProblems(const MaterialProblems &problems);

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
