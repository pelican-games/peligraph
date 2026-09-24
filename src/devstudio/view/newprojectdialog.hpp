#pragma once

#include <QDialog>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace PelicanStudio {

// F-2: File > New Project... A folder, a name and a template; Create writes the
// template through Pelican::initializeProjectTemplate -- the function
// `pelican_cli project init` calls -- and hands the new root to `created`, which
// the main window points at its ordinary openProject. A place that already
// holds files is refused in the words initializeProjectTemplate uses, and the
// dialog stays open so the person can pick another.
//
// Every control carries an objectName (`pelican.newProject.*`) so a scenario can
// fill the dialog in over the automation channel. The dialog is shown with
// open(), not exec(): a nested event loop is something a command that owes a
// reply on the pipe cannot enter.
class NewProjectDialog final : public QDialog {
  public:
    NewProjectDialog(QWidget *parent,
                     std::function<void(const QString &project_root)> created);

    // The directory Create would write, as it stands now.
    [[nodiscard]] QString projectPath() const;

  private:
    std::function<void(const QString &)> created_;
    QLineEdit *location_ = nullptr;
    QLineEdit *name_ = nullptr;
    QComboBox *template_ = nullptr;
    QLabel *path_ = nullptr;
    QLabel *notice_ = nullptr;
    QPushButton *create_ = nullptr;

    void browse();
    void refreshPath();
    void create();
};

} // namespace PelicanStudio
