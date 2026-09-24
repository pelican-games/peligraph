#include "materialpassform.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace PelicanStudio {
namespace {

QString text(const std::string &value) {
    return QString::fromStdString(value);
}

} // namespace

MaterialPassForm::MaterialPassForm(QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("pelican.materialPassForm"));
    setModal(false);
    setWindowTitle(tr("Add material pass"));
    auto *layout = new QVBoxLayout(this);

    where_ = new QLabel(this);
    where_->setObjectName(QStringLiteral("pelican.materialPassForm.where"));
    where_->setWordWrap(true);
    layout->addWidget(where_);

    auto *form = new QFormLayout;
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("pelican.materialPassForm.name"));
    form->addRow(tr("Name"), name_);
    contract_ = new QComboBox(this);
    contract_->setObjectName(QStringLiteral("pelican.materialPassForm.contract"));
    for (const auto &choice : materialPassContracts())
        contract_->addItem(tr(choice.label.c_str()), text(choice.contract));
    contract_->setToolTip(tr(
        "Which materials the pass draws and what it writes: a forward pass "
        "writes lit color, a G-buffer pass writes five surface attributes for "
        "a lighting pass to read."));
    form->addRow(tr("Contract"), contract_);
    layout->addLayout(form);

    slot_box_ = new QWidget(this);
    slot_box_->setObjectName(QStringLiteral("pelican.materialPassForm.outputs"));
    slot_layout_ = new QGridLayout(slot_box_);
    slot_layout_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(slot_box_);

    create_missing_ = new QPushButton(tr("Create missing targets"), this);
    create_missing_->setObjectName(
        QStringLiteral("pelican.materialPassForm.createMissing"));
    create_missing_->setToolTip(tr(
        "Give every empty output a new render target, made with the format "
        "the contract requires."));
    layout->addWidget(create_missing_, 0, Qt::AlignLeft);

    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("pelican.materialPassForm.notice"));
    notice_->setWordWrap(true);
    layout->addWidget(notice_);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("pelican.materialPassForm.error"));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #d94c3d;"));
    error_->hide();
    layout->addWidget(error_);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    ok_ = new QPushButton(tr("Save"), this);
    ok_->setObjectName(QStringLiteral("pelican.materialPassForm.ok"));
    ok_->setDefault(true);
    cancel_ = new QPushButton(tr("Cancel"), this);
    cancel_->setObjectName(QStringLiteral("pelican.materialPassForm.cancel"));
    buttons->addWidget(ok_);
    buttons->addWidget(cancel_);
    layout->addLayout(buttons);

    connect(ok_, &QPushButton::clicked, this, [this] { accept(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { reject(); });
    connect(create_missing_, &QPushButton::clicked, this, [this] { fillMissing(); });
    connect(contract_, &QComboBox::currentIndexChanged, this, [this](int) {
        const auto contract = contract_->currentData().toString().toStdString();
        if (name_->text().toStdString() == proposed_name_) {
            proposed_name_ = materialPassDefaultName(authoring_, graph_, contract);
            name_->setText(text(proposed_name_));
        }
        layoutSlots();
    });
}

void MaterialPassForm::present(const FramePlanAuthoring &authoring,
                               const std::string &graph,
                               const std::string &insert, const QString &where) {
    authoring_ = authoring;
    graph_ = graph;
    insert_ = insert;
    hdr_ = materialPassHdr(authoring);
    declared_.clear();
    for (const auto &[name, row] : authoring.render_targets) {
        (void)row;
        declared_.push_back(name);
    }
    where_->setText(where);
    {
        const QSignalBlocker block{contract_};
        contract_->setCurrentIndex(0);
    }
    proposed_name_ = materialPassDefaultName(
        authoring_, graph_, contract_->currentData().toString().toStdString());
    name_->setText(text(proposed_name_));
    layoutSlots();
    error_->clear();
    error_->hide();
    show();
    raise();
    activateWindow();
    name_->setFocus();
}

void MaterialPassForm::layoutSlots() {
    for (QWidget *widget : slot_widgets_) delete widget;
    slot_widgets_.clear();
    slot_targets_.clear();
    slot_states_.clear();
    slots_ = materialPassSlots(contract_->currentData().toString().toStdString(), hdr_);
    int row = 0;
    for (const auto &slot : slots_) {
        auto *label = new QLabel(
            tr("%1 (%2)").arg(tr(slot.role.c_str()), text(slot.format)), slot_box_);
        auto *target = new QComboBox(slot_box_);
        target->setObjectName(
            QStringLiteral("pelican.materialPassForm.target.%1").arg(text(slot.id)));
        target->setEditable(true);
        target->setInsertPolicy(QComboBox::NoInsert);
        target->addItem(QString{});
        for (const auto &name : declared_) target->addItem(text(name));
        target->setCurrentIndex(0);
        target->setToolTip(tr("A declared render target, or a new name to make "
                              "one with %1")
                               .arg(text(slot.format)));
        auto *state = new QLabel(slot_box_);
        state->setObjectName(
            QStringLiteral("pelican.materialPassForm.state.%1").arg(text(slot.id)));
        slot_layout_->addWidget(label, row, 0);
        slot_layout_->addWidget(target, row, 1);
        slot_layout_->addWidget(state, row, 2);
        slot_widgets_.insert(slot_widgets_.end(), {label, target, state});
        slot_targets_.push_back(target);
        slot_states_.push_back(state);
        connect(target, &QComboBox::currentTextChanged, this,
                [this](const QString &) { updateSlotStates(); });
        ++row;
    }
    updateSlotStates();
}

void MaterialPassForm::updateSlotStates() {
    const auto current = values();
    const auto created = materialPassNewSlots(authoring_, current);
    for (std::size_t index = 0; index < slot_states_.size(); ++index) {
        const auto &name = current.targets[index];
        const bool is_new = std::ranges::find(created, index) != created.end();
        slot_states_[index]->setText(
            name.empty() ? tr("empty")
            : is_new     ? tr("new: made as %1").arg(text(slots_[index].format))
                         : tr("declared"));
    }
    notice_->setText(
        created.empty()
            ? tr("Save adds the pass.")
            : tr("Save first makes %n new render target(s) in one change, then "
                 "adds the pass in a second. If the engine refuses the pass, "
                 "the new targets stay declared; delete them from their nodes "
                 "if you do not want them.",
                 nullptr, static_cast<int>(created.size())));
}

void MaterialPassForm::fillMissing() {
    auto current = values();
    const auto filled =
        materialPassFillMissing(declared_, current.contract, current.targets);
    for (std::size_t index = 0; index < slot_targets_.size() && index < filled.size();
         ++index) {
        if (!current.targets[index].empty()) continue;
        slot_targets_[index]->setEditText(text(filled[index]));
    }
    updateSlotStates();
}

MaterialPassFormValues MaterialPassForm::values() const {
    MaterialPassFormValues values;
    values.graph = graph_;
    values.insert = insert_;
    values.name = name_->text().trimmed().toStdString();
    values.contract = contract_->currentData().toString().toStdString();
    for (const QComboBox *target : slot_targets_)
        values.targets.push_back(target->currentText().trimmed().toStdString());
    return values;
}

void MaterialPassForm::accept() {
    if (!submit_) {
        QDialog::accept();
        return;
    }
    const QString error = submit_(values());
    if (!error.isEmpty()) {
        error_->setText(error);
        error_->show();
        return;
    }
    QDialog::accept();
}

} // namespace PelicanStudio
