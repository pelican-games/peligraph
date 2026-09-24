#pragma once

#include "materialpassedit.hpp"

#include <QDialog>
#include <QString>

#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

namespace PelicanStudio {

// G-1. The form behind "Add material pass...". Not modal, for the reason the
// render target form is not: a modal dialog is a nested event loop the
// automation channel cannot reach into.
//
// Choosing a contract lays out its outputs -- one row per color attachment,
// with what it holds and the format it must be, and one for depth. Each row
// takes a declared target or a new name; "Create missing targets" fills the
// empty rows with new names. The form only collects: what Save sends is
// decided by the pure functions in materialpassedit.
class MaterialPassForm final : public QDialog {
  public:
    // Returns the refusal to show inside the form, or an empty string when the
    // values were taken (the form then closes).
    using Submit = std::function<QString(const MaterialPassFormValues &values)>;

    explicit MaterialPassForm(QWidget *parent = nullptr);

    // `declared` are the render targets the context names; `hdr` picks the
    // formats a G-buffer's new targets are shown (and made) with. `where` is
    // the one line that says where the pass goes.
    void present(const FramePlanAuthoring &authoring,
                 const std::string &graph, const std::string &insert,
                 const QString &where);

    [[nodiscard]] MaterialPassFormValues values() const;
    void setSubmit(Submit submit) { submit_ = std::move(submit); }

  private:
    void accept() override;
    void layoutSlots();
    void updateSlotStates();
    void fillMissing();

    Submit submit_;
    std::string graph_;
    std::string insert_;
    std::vector<std::string> declared_;
    FramePlanAuthoring authoring_;
    bool hdr_ = true;
    // The name the form proposed, so a contract change replaces it only while
    // the user has not typed one of their own.
    std::string proposed_name_;
    std::vector<MaterialPassSlot> slots_;
    std::vector<QComboBox *> slot_targets_;
    std::vector<QLabel *> slot_states_;
    std::vector<QWidget *> slot_widgets_;
    QLabel *where_ = nullptr;
    QLineEdit *name_ = nullptr;
    QComboBox *contract_ = nullptr;
    QWidget *slot_box_ = nullptr;
    QGridLayout *slot_layout_ = nullptr;
    QPushButton *create_missing_ = nullptr;
    QLabel *notice_ = nullptr;
    QLabel *error_ = nullptr;
    QPushButton *ok_ = nullptr;
    QPushButton *cancel_ = nullptr;
};

} // namespace PelicanStudio
