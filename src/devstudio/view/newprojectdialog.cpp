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
#include <QSettings>
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

constexpr const char *LastLocationKey = "new_project/last_location";

} // namespace

QString nextFreeProjectName(const QString &location) {
    const QDir directory{location};
    for (int number = 1;; ++number) {
        const QString candidate =
            QStringLiteral("project_%1").arg(number, 3, 10, QLatin1Char('0'));
        // Any entry of that name counts, even an empty folder: the person
        // made it for something.
        if (location.trimmed().isEmpty() || !directory.exists(candidate))
            return candidate;
    }
}

NewProjectDialog::NewProjectDialog(
    QWidget *parent, std::function<void(const QString &)> created,
    QString settings_directory)
    : QDialog(parent), created_(std::move(created)),
      settings_directory_(std::move(settings_directory)) {
    setObjectName(QStringLiteral("pelican.newProjectDialog"));
    setWindowTitle(tr("New Project"));

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    layout->addLayout(form);

    auto *location_row = new QHBoxLayout;
    location_ = new QLineEdit(this);
    location_->setObjectName(QStringLiteral("pelican.newProject.location"));
    QString start = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (const QString file = settingsFile(); !file.isEmpty()) {
        const QString last =
            QSettings{file, QSettings::IniFormat}.value(QLatin1String{LastLocationKey}).toString();
        if (!last.isEmpty() && QDir{last}.exists()) start = last;
    }
    location_->setText(QDir::toNativeSeparators(start));
    auto *browse = new QPushButton(tr("Browse..."), this);
    browse->setObjectName(QStringLiteral("pelican.newProject.browse"));
    location_row->addWidget(location_, 1);
    location_row->addWidget(browse);
    form->addRow(tr("Folder"), location_row);

    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("pelican.newProject.name"));
    proposeName();
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
    connect(location_, &QLineEdit::textChanged, this, [this] {
        proposeName();
        refreshPath();
    });
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

QString NewProjectDialog::settingsFile() const {
    if (settings_directory_.isEmpty()) return {};
    return QDir{settings_directory_}.filePath(QStringLiteral("new_project.ini"));
}

void NewProjectDialog::proposeName() {
    const QString current = name_->text().trimmed();
    if (!current.isEmpty() && current != proposed_name_) return;
    proposed_name_ = nextFreeProjectName(location_->text().trimmed());
    name_->setText(proposed_name_);
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
    if (const QString file = settingsFile(); !file.isEmpty()) {
        QDir{}.mkpath(settings_directory_);
        QSettings settings{file, QSettings::IniFormat};
        settings.setValue(QLatin1String{LastLocationKey},
                          QDir::fromNativeSeparators(location_->text().trimmed()));
        settings.sync();
    }
    // Held before accept(): a dialog that deletes itself on close must not be
    // the owner of the call that opens the project.
    const auto created = created_;
    accept();
    if (created) created(root);
}

} // namespace PelicanStudio
