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
// simplemodelview shows a model: one combo of the project's materials and
// "Model default" (bind_material on every primitive of the model), the line
// under it saying that a model's material is the model's -- every object
// showing it changes -- and, for a model of more than one primitive, a combo
// per primitive (get_material_bindings). Its own Undo takes a bind back with
// the reply's previous.bindings.
class MaterialAssignWidget final : public QGroupBox {
  public:
    struct Hooks {
        std::function<qint64(const QString &method, const QJsonObject &params, QString *error)> send;
        std::function<void(const QString &reason)> restart;
    };

    explicit MaterialAssignWidget(Hooks hooks, QWidget *parent = nullptr);
    ~MaterialAssignWidget() override;

    void engineAvailable();
    void engineUnavailable();
    // The selected object's model (empty: none) and its name.
    void setSubject(const std::string &model, const std::string &object);
    void materialsChanged();
    void bindingsChanged();

    bool receiveResult(qint64 request_id, const QByteArray &result_json);
    bool receiveRefusal(qint64 request_id, int code, const QString &message, const QJsonValue &data);
    bool receiveFailure(qint64 request_id, const QString &message);

    [[nodiscard]] MaterialAssignSession &session() noexcept;
    [[nodiscard]] QComboBox *choice() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
