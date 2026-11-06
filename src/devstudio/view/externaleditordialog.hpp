#pragma once

#include "externaleditor.hpp"

#include <QDialog>

#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSettings;

namespace PelicanStudio {

class ExternalEditorDialog final : public QDialog {
    Q_OBJECT

  public:
    ExternalEditorDialog(QSettings &settings, EditorPlatform platform, EditorProbe probe,
                         QWidget *parent = nullptr,
                         std::shared_ptr<ExternalEditorDiscovery> discovery = {});

    void setProgramChooser(std::function<QString()> chooser);

  private:
    QSettings &settings_;
    EditorPlatform platform_;
    EditorProbe probe_;
    std::shared_ptr<ExternalEditorDiscovery> discovery_;
    QComboBox *choice_ = nullptr;
    QLabel *resolved_ = nullptr;
    QLineEdit *program_ = nullptr;
    QPushButton *browse_ = nullptr;
    QLineEdit *arguments_ = nullptr;
    QLabel *hint_ = nullptr;
    QLabel *error_ = nullptr;
    QPushButton *ok_ = nullptr;
    QPushButton *cancel_ = nullptr;
    std::function<QString()> program_chooser_;
    QString displayed_choice_;
    quint64 resolution_generation_ = 0;

    void choiceChanged();
    void validateCustom();
    void accept();
    void showError(const QString &message);
};

} // namespace PelicanStudio
