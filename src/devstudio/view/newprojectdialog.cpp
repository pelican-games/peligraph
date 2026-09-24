#include "newprojectdialog.hpp"

#include "projecttemplate.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace PelicanStudio {

namespace {

std::filesystem::path toFilesystemPath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

QString toDisplayPath(const std::filesystem::path &path) {
#ifdef _WIN32
    return QDir::toNativeSeparators(QString::fromStdWString(path.wstring()));
#else
    return QString::fromStdString(path.string());
#endif
}

QString templateLabel(Pelican::ProjectTemplate kind) {
    switch (kind) {
    case Pelican::ProjectTemplate::Standard:
        return QObject::tr("Standard 3D scene");
    case Pelican::ProjectTemplate::Blank:
        return QObject::tr("Blank pipeline");
    }
    return {};
}

} // namespace

NewProjectDialog::NewProjectDialog(
    QWidget *parent, std::function<void(const QString &)> created)
    : QDialog(parent), created_(std::move(created)) {
    setObjectName(QStringLiteral("pelican.newProjectDialog"));
    setWindowTitle(tr("New Project"));

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    layout->addLayout(form);

    auto *location_row = new QHBoxLayout;
    location_ = new QLineEdit(this);
    location_->setObjectName(QStringLiteral("pelican.newProject.location"));
    location_->setText(QDir::toNativeSeparators(QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation)));
    auto *browse = new QPushButton(tr("Browse..."), this);
    browse->setObjectName(QStringLiteral("pelican.newProject.browse"));
    location_row->addWidget(location_, 1);
    location_row->addWidget(browse);
    form->addRow(tr("Folder"), location_row);

    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("pelican.newProject.name"));
    name_->setText(QStringLiteral("NewProject"));
    form->addRow(tr("Name"), name_);

    template_ = new QComboBox(this);
    template_->setObjectName(QStringLiteral("pelican.newProject.template"));
    for (const Pelican::ProjectTemplate kind : Pelican::projectTemplates()) {
        template_->addItem(
            templateLabel(kind),
            QString::fromUtf8(Pelican::projectTemplateName(kind).data(),
                              static_cast<qsizetype>(
                                  Pelican::projectTemplateName(kind).size())));
    }
    form->addRow(tr("Template"), template_);

    path_ = new QLabel(this);
    path_->setObjectName(QStringLiteral("pelican.newProject.path"));
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Creates"), path_);

    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("pelican.newProject.notice"));
    notice_->setWordWrap(true);
    layout->addWidget(notice_);

    auto *buttons = new QDialogButtonBox(this);
    create_ = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    create_->setObjectName(QStringLiteral("pelican.newProject.create"));
    create_->setDefault(true);
    QPushButton *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    cancel->setObjectName(QStringLiteral("pelican.newProject.cancel"));
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, [this] { this->browse(); });
    connect(location_, &QLineEdit::textChanged, this,
            [this] { refreshPath(); });
    connect(name_, &QLineEdit::textChanged, this, [this] { refreshPath(); });
    // Create is not wired to accept(): the dialog closes only once the
    // template is on disk, so a refusal leaves it open with the reason.
    connect(create_, &QPushButton::clicked, this, [this] { create(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    refreshPath();
}

QString NewProjectDialog::projectPath() const {
    const QString location = location_->text().trimmed();
    const QString name = name_->text().trimmed();
    if (location.isEmpty() || name.isEmpty()) return {};
    return QDir::toNativeSeparators(QDir(location).filePath(name));
}

void NewProjectDialog::browse() {
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Folder for the New Project"), location_->text(),
        QFileDialog::ShowDirsOnly);
    if (!directory.isEmpty())
        location_->setText(QDir::toNativeSeparators(directory));
}

void NewProjectDialog::refreshPath() {
    path_->setText(projectPath());
    notice_->clear();
}

void NewProjectDialog::create() {
    const QString name = name_->text().trimmed();
    if (location_->text().trimmed().isEmpty()) {
        notice_->setText(tr("Choose the folder to create the project in."));
        return;
    }
    if (name.isEmpty()) {
        notice_->setText(tr("Enter a name for the project."));
        return;
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        name == QLatin1String(".") || name == QLatin1String("..")) {
        notice_->setText(
            tr("The name is one folder: it cannot contain / or \\."));
        return;
    }
    const auto kind = Pelican::projectTemplateFromName(
        template_->currentData().toString().toStdString());
    if (!kind) {
        notice_->setText(tr("Choose a template."));
        return;
    }
    QString root;
    try {
        const auto result = Pelican::initializeProjectTemplate(
            toFilesystemPath(projectPath()), *kind);
        root = toDisplayPath(result.project_root);
    } catch (const std::exception &error) {
        // initializeProjectTemplate's own words: it names the directory and
        // says what is wrong with it ("... is not empty: <path>").
        notice_->setText(QString::fromUtf8(error.what()));
        return;
    }
    // Held before accept(): a dialog that deletes itself on close must not be
    // the owner of the call that opens the project.
    const auto created = created_;
    accept();
    if (created) created(root);
}

} // namespace PelicanStudio
