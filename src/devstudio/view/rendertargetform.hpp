#pragma once

#include "frameplanedit.hpp"

#include <QDialog>
#include <QString>

#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;

namespace PelicanStudio {

// F-1. The form behind "New render target..." and behind a double-click on a
// target's node. It is not modal: a modal dialog is a nested event loop, which
// neither the automation channel nor the rest of the panel can reach into, and
// nothing about a render target needs the panel to stop while it is typed.
//
// The form only collects values. What they become -- the declaration, the RPC,
// or the sentence that says why there is none -- is decided by the pure
// functions in frameplanedit, so a test can read the payload without it.
class RenderTargetForm final : public QDialog {
  public:
    enum class Mode { create, edit, read_only };

    // Returns the refusal to show inside the form, or an empty string when the
    // values were taken (the form then closes).
    using Submit = std::function<QString(const RenderTargetFormValues &values)>;

    explicit RenderTargetForm(QWidget *parent = nullptr);

    // `formats` and `usages` are the parser's own vocabulary, as the engine
    // reported it. `origin` is the one line that says where a read-only target
    // was declared.
    void present(Mode mode, const RenderTargetFormValues &values,
                const std::vector<std::string> &formats,
                const std::vector<std::string> &usages,
                const QString &origin = {});

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    // The name the form was opened on: the target a replace changes.
    [[nodiscard]] const std::string &editing() const noexcept {
        return editing_;
    }
    // The form's values, or an error sentence when a field cannot be read.
    [[nodiscard]] bool readValues(RenderTargetFormValues &values,
                                  QString &error) const;

    void setSubmit(Submit submit) { submit_ = std::move(submit); }

  private:
    void accept() override;
    void updateEnabled();

    Mode mode_ = Mode::create;
    std::string editing_;
    Submit submit_;
    QLineEdit *name_ = nullptr;
    QComboBox *kind_ = nullptr;
    QComboBox *format_ = nullptr;
    QComboBox *size_mode_ = nullptr;
    QDoubleSpinBox *extent_scale_ = nullptr;
    QSpinBox *width_ = nullptr;
    QSpinBox *height_ = nullptr;
    QWidget *usage_box_ = nullptr;
    QGridLayout *usage_layout_ = nullptr;
    std::vector<QCheckBox *> usage_checks_;
    QCheckBox *history_ = nullptr;
    QLineEdit *clear_color_ = nullptr;
    QLabel *origin_ = nullptr;
    QLabel *error_ = nullptr;
    QPushButton *ok_ = nullptr;
    QPushButton *cancel_ = nullptr;
};

} // namespace PelicanStudio
