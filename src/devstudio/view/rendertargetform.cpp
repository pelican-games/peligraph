#include "rendertargetform.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

namespace PelicanStudio {
namespace {

// The kinds the form offers, in the order a person meets them, with the
// format_class each one writes.
constexpr std::array Kinds{
    std::pair{"Scene color", "scene"},
    std::pair{"Display", "display"},
    std::pair{"Data", "data"},
    std::pair{"Explicit format", "explicit"},
};

QString text(const std::string &value) {
    return QString::fromStdString(value);
}

} // namespace

RenderTargetForm::RenderTargetForm(QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("pelican.renderTargetForm"));
    setModal(false);
    setWindowTitle(tr("Render target"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("pelican.renderTargetForm.name"));
    name_->setPlaceholderText(tr("A name passes can read and write"));
    form->addRow(tr("Name"), name_);

    auto *kind_row = new QHBoxLayout;
    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("pelican.renderTargetForm.kind"));
    for (const auto &[label, value] : Kinds)
        kind_->addItem(tr(label), QString::fromLatin1(value));
    kind_->setToolTip(tr(
        "Scene color and Display take the format the engine chooses for the "
        "frame. Data and Explicit format use the format you pick."));
    format_ = new QComboBox(this);
    format_->setObjectName(QStringLiteral("pelican.renderTargetForm.format"));
    kind_row->addWidget(kind_, 1);
    kind_row->addWidget(format_, 1);
    form->addRow(tr("Kind"), kind_row);

    auto *size_row = new QHBoxLayout;
    size_mode_ = new QComboBox(this);
    size_mode_->setObjectName(
        QStringLiteral("pelican.renderTargetForm.sizeMode"));
    size_mode_->addItem(tr("Screen ratio"), QStringLiteral("ratio"));
    size_mode_->addItem(tr("Fixed size"), QStringLiteral("fixed"));
    extent_scale_ = new QDoubleSpinBox(this);
    extent_scale_->setObjectName(
        QStringLiteral("pelican.renderTargetForm.extentScale"));
    extent_scale_->setRange(0.01, 16.0);
    extent_scale_->setDecimals(3);
    extent_scale_->setSingleStep(0.25);
    extent_scale_->setToolTip(
        tr("extent_scale: 1 is the size of the screen, 0.5 is half of it"));
    width_ = new QSpinBox(this);
    width_->setObjectName(QStringLiteral("pelican.renderTargetForm.width"));
    width_->setRange(1, 16384);
    height_ = new QSpinBox(this);
    height_->setObjectName(QStringLiteral("pelican.renderTargetForm.height"));
    height_->setRange(1, 16384);
    size_row->addWidget(size_mode_);
    size_row->addWidget(extent_scale_);
    size_row->addWidget(width_);
    size_row->addWidget(new QLabel(QStringLiteral("x"), this));
    size_row->addWidget(height_);
    form->addRow(tr("Size"), size_row);

    usage_box_ = new QWidget(this);
    usage_box_->setObjectName(QStringLiteral("pelican.renderTargetForm.usage"));
    usage_layout_ = new QGridLayout(usage_box_);
    usage_layout_->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("Usage"), usage_box_);

    history_ = new QCheckBox(tr("Keep the previous frame (history)"), this);
    history_->setObjectName(QStringLiteral("pelican.renderTargetForm.history"));
    form->addRow(QString{}, history_);

    clear_color_ = new QLineEdit(this);
    clear_color_->setObjectName(
        QStringLiteral("pelican.renderTargetForm.clearColor"));
    clear_color_->setPlaceholderText(tr("r, g, b, a (optional)"));
    form->addRow(tr("Clear color"), clear_color_);
    layout->addLayout(form);

    origin_ = new QLabel(this);
    origin_->setObjectName(QStringLiteral("pelican.renderTargetForm.origin"));
    origin_->setWordWrap(true);
    origin_->hide();
    layout->addWidget(origin_);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("pelican.renderTargetForm.error"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #d94c3d;"));
    error_->hide();
    layout->addWidget(error_);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    ok_ = new QPushButton(tr("OK"), this);
    ok_->setObjectName(QStringLiteral("pelican.renderTargetForm.ok"));
    ok_->setDefault(true);
    cancel_ = new QPushButton(tr("Cancel"), this);
    cancel_->setObjectName(QStringLiteral("pelican.renderTargetForm.cancel"));
    buttons->addWidget(ok_);
    buttons->addWidget(cancel_);
    layout->addLayout(buttons);

    connect(ok_, &QPushButton::clicked, this, [this] { accept(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { reject(); });
    connect(kind_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateEnabled(); });
    connect(size_mode_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateEnabled(); });
}

void RenderTargetForm::present(Mode mode, const RenderTargetFormValues &values,
                               const std::vector<std::string> &formats,
                               const std::vector<std::string> &usages,
                               const QString &origin) {
    mode_ = mode;
    editing_ = mode == Mode::create ? std::string{} : values.name;
    setWindowTitle(mode == Mode::create ? tr("New render target")
                   : mode == Mode::edit
                       ? tr("Render target %1").arg(text(values.name))
                       : tr("Render target %1 (read-only)")
                             .arg(text(values.name)));
    name_->setText(text(values.name));
    const int kind = kind_->findData(text(values.kind));
    kind_->setCurrentIndex(kind < 0 ? 0 : kind);

    format_->clear();
    for (const auto &format : formats) format_->addItem(text(format));
    if (!values.format.empty() && format_->findText(text(values.format)) < 0)
        format_->addItem(text(values.format));
    format_->setCurrentIndex(
        std::max(0, format_->findText(text(values.format))));

    size_mode_->setCurrentIndex(values.fixed_size ? 1 : 0);
    extent_scale_->setValue(values.extent_scale);
    width_->setValue(static_cast<int>(values.width));
    height_->setValue(static_cast<int>(values.height));

    // The checkboxes are the parser's usage names, plus any the declaration
    // carries that the engine did not list -- so nothing it holds is dropped
    // silently by opening and saving it.
    for (QCheckBox *check : usage_checks_) delete check;
    usage_checks_.clear();
    std::vector<std::string> names = usages;
    for (const auto &usage : values.usage)
        if (std::ranges::find(names, usage) == names.end())
            names.push_back(usage);
    int index = 0;
    for (const auto &usage : names) {
        auto *check = new QCheckBox(text(usage), usage_box_);
        check->setObjectName(
            QStringLiteral("pelican.renderTargetForm.usage.%1").arg(text(usage)));
        check->setChecked(std::ranges::find(values.usage, usage) !=
                          values.usage.end());
        usage_layout_->addWidget(check, index / 3, index % 3);
        usage_checks_.push_back(check);
        ++index;
    }

    history_->setChecked(values.history);
    if (values.clear_color) {
        const auto &rgba = *values.clear_color;
        clear_color_->setText(QStringLiteral("%1, %2, %3, %4")
                                  .arg(rgba[0])
                                  .arg(rgba[1])
                                  .arg(rgba[2])
                                  .arg(rgba[3]));
    } else {
        clear_color_->clear();
    }
    origin_->setText(origin);
    origin_->setVisible(!origin.isEmpty());
    error_->clear();
    error_->hide();
    updateEnabled();
    show();
    raise();
    activateWindow();
    name_->setFocus();
}

bool RenderTargetForm::readValues(RenderTargetFormValues &values,
                                  QString &error) const {
    values = RenderTargetFormValues{};
    values.name = name_->text().trimmed().toStdString();
    values.kind = kind_->currentData().toString().toStdString();
    values.format = format_->currentText().toStdString();
    values.fixed_size = size_mode_->currentIndex() == 1;
    values.extent_scale = extent_scale_->value();
    values.width = static_cast<std::uint32_t>(width_->value());
    values.height = static_cast<std::uint32_t>(height_->value());
    values.usage.clear();
    for (const QCheckBox *check : usage_checks_)
        if (check->isChecked()) values.usage.push_back(check->text().toStdString());
    values.history = history_->isChecked();
    const QString color = clear_color_->text().trimmed();
    if (!color.isEmpty()) {
        const QStringList parts = color.split(QLatin1Char(','));
        std::array<double, 4> rgba{};
        bool valid = parts.size() == 4;
        for (int part = 0; valid && part < 4; ++part)
            rgba[static_cast<std::size_t>(part)] =
                parts[part].trimmed().toDouble(&valid);
        if (!valid) {
            error = tr("The clear color is four numbers: r, g, b, a.");
            return false;
        }
        values.clear_color = rgba;
    }
    return true;
}

void RenderTargetForm::accept() {
    if (mode_ == Mode::read_only || !submit_) {
        QDialog::accept();
        return;
    }
    RenderTargetFormValues values;
    QString error;
    if (readValues(values, error)) error = submit_(values);
    if (!error.isEmpty()) {
        error_->setText(error);
        error_->show();
        return;
    }
    QDialog::accept();
}

void RenderTargetForm::updateEnabled() {
    const bool writable = mode_ != Mode::read_only;
    const auto kind = kind_->currentData().toString();
    const bool fixed = size_mode_->currentIndex() == 1;
    name_->setReadOnly(!writable);
    kind_->setEnabled(writable);
    format_->setEnabled(writable && (kind == QStringLiteral("data") ||
                                     kind == QStringLiteral("explicit")));
    size_mode_->setEnabled(writable);
    extent_scale_->setEnabled(writable && !fixed);
    width_->setEnabled(writable && fixed);
    height_->setEnabled(writable && fixed);
    for (QCheckBox *check : usage_checks_) check->setEnabled(writable);
    history_->setEnabled(writable);
    clear_color_->setReadOnly(!writable);
    cancel_->setVisible(writable);
}

void presentNewRenderTarget(RenderTargetForm &form,
                            const FramePlanAuthoring &authoring,
                            const std::string &name,
                            const std::optional<RenderTargetFormValues> &kind) {
    RenderTargetFormValues values = kind.value_or(RenderTargetFormValues{});
    values.name = name.empty() ? proposedRenderTargetName(authoring) : name;
    form.present(RenderTargetForm::Mode::create, values,
                 authoring.render_target_formats,
                 authoring.render_target_usages);
}

bool presentRenderTargetEdit(RenderTargetForm &form, const FramePlanAuthoring &authoring,
                             const std::string &name) {
    const auto *row = authoring.findRenderTarget(name);
    if (row == nullptr) return false;
    if (row->editable) {
        form.present(RenderTargetForm::Mode::edit,
                     renderTargetFormValues(row->authored.empty() ? row->declaration
                                                                  : row->authored),
                     authoring.render_target_formats, authoring.render_target_usages);
        return true;
    }
    form.present(RenderTargetForm::Mode::read_only, renderTargetFormValues(row->declaration),
                 authoring.render_target_formats, authoring.render_target_usages,
                 QObject::tr("Read-only: %1.")
                     .arg(QString::fromStdString(framePlanRenderTargetOrigin(
                         row->source, name, authoring.preset_name))));
    return true;
}

} // namespace PelicanStudio
