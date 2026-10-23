#pragma once

#include "../model/materialsmodel.hpp"

#include <QByteArray>
#include <QGroupBox>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>
#include <memory>
#include <string>

class QComboBox;
class QLabel;
class QToolButton;

namespace PelicanStudio {

// MT-1s, decision 5. The Inspector's Material rows for an object whose
// simplemodelview shows a model: one combo of the project's materials, the
// line under it saying what a choice applies to, and, for a model of more
// than one primitive, a combo per primitive.
// MT-2s. What a choice applies to is chosen first: This object (the default;
// set_object_material through the Inspector's editor session, so the scene's
// Undo takes it back) or Every <model> (bind_material, MT-1s; its own Undo
// takes a bind back with the reply's previous.bindings, to the model it was
// sent to). This object shows what get_object_materials says is drawn and
// where it comes from, and Use model default takes the object's own away.
class MaterialAssignWidget final : public QGroupBox {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<qint64(const QString &method, const QJsonObject &params, QString *error)> send;
        std::function<void(const QString &reason)> restart;
        // set_object_material without actor_id and base_scene_revision: the
        // Inspector's editor session adds both and sends it as its own edit.
        std::function<bool(const QJsonObject &params, const QString &description, QString *error)> edit_object;
    };

    explicit MaterialAssignWidget(Hooks hooks, QWidget *parent = nullptr);
    ~MaterialAssignWidget() override;

    void engineAvailable();
    void engineUnavailable();
    // The selected object (an empty model: none).
    void setSubject(const MaterialAssignSession::Subject &subject);
    // The Inspector's editor session has an edit in flight.
    void setSceneBusy(bool busy);
    void materialsChanged();
    void bindingsChanged();
    // The engine refused the set_object_material the editor session sent for
    // these rows: said here; answers the one sentence it reads as.
    QString objectEditRefused(const QString &error_code, const QString &message, const QJsonValue &data);

    bool receiveResult(qint64 request_id, const QByteArray &result_json);
    bool receiveRefusal(qint64 request_id, int code, const QString &message, const QJsonValue &data);
    bool receiveFailure(qint64 request_id, const QString &message);

    [[nodiscard]] MaterialAssignSession &session() noexcept;
    [[nodiscard]] QComboBox *choice() const noexcept;
    // Empty until the current rows are read and the combo carries a material reference.
    [[nodiscard]] QString assignableMaterial() const;

  signals:
    void rowsPresented();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
