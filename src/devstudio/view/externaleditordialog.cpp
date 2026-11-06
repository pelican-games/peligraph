#include "externaleditordialog.hpp"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace PelicanStudio {
namespace {
void named(QWidget *widget, const QString &suffix) {
    widget->setObjectName(QStringLiteral("pelican.externalEditor.") + suffix);
}
}

ExternalEditorDialog::ExternalEditorDialog(
    QSettings &settings, EditorPlatform platform, EditorProbe probe, QWidget *parent,
    std::shared_ptr<ExternalEditorDiscovery> discovery)
    : QDialog{parent}, settings_{settings}, platform_{platform}, probe_{std::move(probe)},
      discovery_{discovery ? std::move(discovery)
                           : std::make_shared<ExternalEditorDiscovery>(probe_)} {
    setObjectName(QStringLiteral("pelican.externalEditor.dialog"));
    setWindowTitle(tr("External Editor"));
    setModal(false);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    choice_ = new QComboBox(this);
    named(choice_, QStringLiteral("choice"));
    choice_->addItem(tr("System default"), QStringLiteral("system"));
    for (const auto *preset : presetsFor(platform_)) choice_->addItem(preset->label, preset->id);
    choice_->addItem(tr("Custom"), QStringLiteral("custom"));
    form->addRow(tr("Editor"), choice_);

    resolved_ = new QLabel(this);
    named(resolved_, QStringLiteral("resolved"));
    resolved_->setWordWrap(true);
    form->addRow({}, resolved_);

    program_ = new QLineEdit(this);
    named(program_, QStringLiteral("program"));
    program_->setToolTip(tr("A Custom executable is user-trusted, not sandboxed. "
                            "Project files cannot select it."));
    browse_ = new QPushButton(tr("Browse..."), this);
    named(browse_, QStringLiteral("browse"));
    auto *program_row = new QHBoxLayout;
    program_row->addWidget(program_, 1);
    program_row->addWidget(browse_);
    form->addRow(tr("Program"), program_row);

    arguments_ = new QLineEdit(this);
    named(arguments_, QStringLiteral("arguments"));
    arguments_->setPlaceholderText(QStringLiteral("--goto {file}:{line}:{column}"));
    form->addRow(tr("Arguments"), arguments_);

    hint_ = new QLabel(tr("Placeholders: {file} {line} {column} {project}. "
                          "A missing line or column reads 1."), this);
    named(hint_, QStringLiteral("hint"));
    hint_->setWordWrap(true);
    layout->addLayout(form);
    layout->addWidget(hint_);

    error_ = new QLabel(this);
    named(error_, QStringLiteral("error"));
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    ok_ = new QPushButton(tr("OK"), this);
    named(ok_, QStringLiteral("ok"));
    cancel_ = new QPushButton(tr("Cancel"), this);
    named(cancel_, QStringLiteral("cancel"));
    buttons->addWidget(ok_);
    buttons->addWidget(cancel_);
    layout->addLayout(buttons);

    const auto stored = ExternalEditorStore{settings_}.load();
    int selected = choice_->findData(stored.choice);
    if (selected < 0) {
        choice_->addItem(tr("Unknown editor (%1)").arg(stored.choice), stored.choice);
        selected = choice_->count() - 1;
    }
    choice_->setCurrentIndex(selected);
    displayed_choice_ = stored.choice;
    program_->setText(stored.custom_program);
    arguments_->setText(stored.custom_arguments);

    connect(choice_, &QComboBox::currentIndexChanged, this, [this] { choiceChanged(); });
    connect(program_, &QLineEdit::textChanged, this, [this] { validateCustom(); });
    connect(arguments_, &QLineEdit::textChanged, this, [this] { validateCustom(); });
    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString chosen = program_chooser_
                                   ? program_chooser_()
                                   : QFileDialog::getOpenFileName(
                                         this, tr("Choose editor"), {},
                                         platform_ == EditorPlatform::windows
                                             ? tr("Programs (*.exe)") : QString{});
        if (!chosen.isEmpty()) program_->setText(chosen);
    });
    connect(ok_, &QPushButton::clicked, this, [this] { accept(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { reject(); });
    choiceChanged();
}

void ExternalEditorDialog::setProgramChooser(std::function<QString()> chooser) {
    program_chooser_ = std::move(chooser);
}

void ExternalEditorDialog::choiceChanged() {
    ++resolution_generation_;
    displayed_choice_ = choice_->currentData().toString();
    const bool custom = displayed_choice_ == QStringLiteral("custom");
    const auto *preset = externalEditorPreset(displayed_choice_);
    program_->setEnabled(custom);
    browse_->setEnabled(custom);
    arguments_->setEnabled(custom);
    ok_->setEnabled(custom || preset != nullptr || displayed_choice_ == QStringLiteral("system"));
    if (!custom && !preset && displayed_choice_ != QStringLiteral("system")) {
        resolved_->setText(tr("The saved editor choice %1 isn't known. Choose a valid editor before saving.")
                               .arg(displayed_choice_));
        showError(resolved_->text());
        return;
    }
    showError({});
    validateCustom();

    if (displayed_choice_ == QStringLiteral("system")) {
        resolved_->setText(tr("Uses the program this computer opens the file with."));
    } else if (custom) {
        resolved_->clear();
    } else if (preset) {
        QString suffix;
        if (preset->id == QStringLiteral("visual_studio"))
            suffix = tr(" Visual Studio opens the file, but this command-line integration "
                        "cannot reliably go to a line in a running instance.");
        resolved_->setText(tr("Finding %1...").arg(preset->label));
        const auto generation = resolution_generation_;
        auto finish = [this, generation, suffix](const QString &program) {
            if (generation != resolution_generation_) return;
            resolved_->setText(program.isEmpty()
                                   ? tr("Not found on this computer.") +
                                         (platform_ == EditorPlatform::other
                                              ? tr(" Install this editor's command-line launcher on PATH.")
                                              : QString{})
                                   : tr("Starts %1").arg(program) + suffix);
        };
        if (preset->id == QStringLiteral("visual_studio")) {
            discovery_->resolve(*preset, platform_, this, std::move(finish));
        } else {
            finish(resolvePresetProgram(*preset, platform_, probe_));
        }
    }
}

void ExternalEditorDialog::validateCustom() {
    if (displayed_choice_ != QStringLiteral("custom")) return;
    const QString problem = customEditorProblem(program_->text(), arguments_->text(), platform_);
    ok_->setEnabled(problem.isEmpty());
    showError(problem);
}

void ExternalEditorDialog::showError(const QString &message) {
    error_->setText(message);
    error_->setVisible(!message.isEmpty());
}

void ExternalEditorDialog::accept() {
    const QString selected = choice_->currentData().toString();
    if (selected != QStringLiteral("system") && selected != QStringLiteral("custom") &&
        externalEditorPreset(selected) == nullptr)
        return;
    if (selected == QStringLiteral("custom")) {
        const QString problem = customEditorProblem(program_->text(), arguments_->text(), platform_);
        if (!problem.isEmpty()) {
            showError(problem);
            return;
        }
    }
    ExternalEditorSettings settings;
    settings.choice = selected;
    settings.custom_program = program_->text();
    settings.custom_arguments = arguments_->text();
    QString error;
    if (!ExternalEditorStore{settings_}.save(settings, &error)) {
        showError(error);
        return;
    }
    QDialog::accept();
}

} // namespace PelicanStudio
