#include "projectmigrationwidget.hpp"
#include "../guitrace.hpp"

#include "panelhelp.hpp"

#include <QBrush>
#include <QColor>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace PelicanStudio {
namespace {

constexpr auto PlanStaleCode = "project.migration_plan_stale@1";
constexpr auto RestartRequiredCode = "project.migration_restart_required@1";
// The colour every Studio notice that asks for attention is drawn in.
constexpr auto AttentionColor = "#d98c00";

struct Refusal {
    QString code;
    QJsonObject fields;
};

// "<code>: <fields JSON>", the form every migration refusal takes -- in the
// message of an RPC error, in the line the player writes to stderr, and in
// what the offline open throws. The data member's error_code, when there is
// one, is the same code.
Refusal readRefusal(const QString &error_code, const QString &message) {
    Refusal refusal{error_code, {}};
    const qsizetype separator = message.indexOf(QStringLiteral(": "));
    if (separator <= 0) return refusal;
    const QString head = message.left(separator);
    if (!head.contains(QLatin1Char('@')) || head.contains(QLatin1Char(' ')))
        return refusal;
    if (refusal.code.isEmpty()) refusal.code = head;
    refusal.fields =
        QJsonDocument::fromJson(message.mid(separator + 2).toUtf8()).object();
    return refusal;
}

QString formatText(const QJsonObject &document) {
    return QStringLiteral("%1  v%2 \u2192 v%3")
        .arg(document.value(QStringLiteral("schema")).toString())
        .arg(document.value(QStringLiteral("from")).toInt())
        .arg(document.value(QStringLiteral("to")).toInt());
}

} // namespace

ProjectMigrationWidget::ProjectMigrationWidget(RequestRpc request,
                                               QWidget *parent)
    : QWidget(parent), request_(std::move(request)) {
    setObjectName(QStringLiteral("pelican.projectMigration"));
    auto *layout = new QVBoxLayout(this);

    title_ = new QLabel(tr("This project was saved in an earlier format."),
                        this);
    title_->setObjectName(QStringLiteral("pelican.projectMigration.title"));
    QFont title_font = studioPanelFont();
    title_font.setBold(true);
    title_->setFont(title_font);
    layout->addWidget(title_);

    root_ = new QLabel(this);
    root_->setObjectName(QStringLiteral("pelican.projectMigration.root"));
    root_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    applyPanelNoticeStyle(root_);
    layout->addWidget(root_);

    versions_ = new QLabel(this);
    versions_->setObjectName(
        QStringLiteral("pelican.projectMigration.versions"));
    applyPanelNoticeStyle(versions_);
    versions_->hide();
    layout->addWidget(versions_);

    documents_ = new QTreeWidget(this);
    documents_->setObjectName(
        QStringLiteral("pelican.projectMigration.documents"));
    documents_->setColumnCount(2);
    documents_->setHeaderLabels({tr("Document"), tr("Format")});
    documents_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    documents_->setSelectionMode(QAbstractItemView::NoSelection);
    documents_->setToolTip(
        tr("Each document the engine moves to its own version, and the steps "
           "that move it. Rows in the attention colour are places a step "
           "could not rewrite by itself."));
    layout->addWidget(documents_, 1);

    manual_notice_ = new QLabel(this);
    manual_notice_->setObjectName(
        QStringLiteral("pelican.projectMigration.manualNotice"));
    applyPanelNoticeStyle(manual_notice_);
    manual_notice_->setStyleSheet(
        QStringLiteral("color: %1;").arg(QLatin1String(AttentionColor)));
    manual_notice_->hide();
    layout->addWidget(manual_notice_);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("pelican.projectMigration.status"));
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    applyPanelNoticeStyle(status_);
    status_->hide();
    layout->addWidget(status_);

    backup_ = new QLabel(this);
    backup_->setObjectName(QStringLiteral("pelican.projectMigration.backup"));
    backup_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    applyPanelNoticeStyle(backup_);
    backup_->hide();
    layout->addWidget(backup_);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    migrate_button_ = new QPushButton(tr("Migrate"), this);
    migrate_button_->setObjectName(
        QStringLiteral("pelican.projectMigration.migrate"));
    migrate_button_->setToolTip(
        tr("Rewrite these documents to this engine's versions. The documents "
           "as they are now are copied under .pelican/migration first."));
    migrate_button_->setEnabled(false);
    close_button_ = new QPushButton(tr("Don't Open"), this);
    close_button_->setObjectName(
        QStringLiteral("pelican.projectMigration.close"));
    close_button_->setToolTip(tr("Close the project without changing it."));
    buttons->addWidget(migrate_button_);
    buttons->addWidget(close_button_);
    layout->addLayout(buttons);

    connect(migrate_button_, &QPushButton::clicked, this,
            [this] { requestApply(); });
    connect(close_button_, &QPushButton::clicked, this, [this] {
        const QString root = project_root_;
        end();
        emit closeRequested(root);
    });
}

void ProjectMigrationWidget::begin(const QString &project_root,
                                   const QString &refusal) {
    end();
    project_root_ = project_root;
    root_->setText(QDir::toNativeSeparators(project_root));
    // What the refusal already names is listed now; the steps and what has
    // to be fixed by hand come with the plan.
    const auto documents = readRefusal({}, refusal)
                               .fields.value(QStringLiteral("documents"))
                               .toArray();
    for (const auto &value : documents) {
        const QJsonObject document = value.toObject();
        auto *row = new QTreeWidgetItem(documents_);
        row->setText(0, document.value(QStringLiteral("path")).toString());
        row->setText(1, formatText(document));
    }
    setStage(Stage::waiting_for_engine);
    setStatus(tr("Starting the engine to read the migration plan..."));
}

void ProjectMigrationWidget::engineReady() {
    // A player that came up after one that failed is asked again.
    if (stage_ != Stage::waiting_for_engine && stage_ != Stage::failed) return;
    requestPlan();
}

void ProjectMigrationWidget::engineOpenedWithoutMigrating() {
    if (stage_ != Stage::waiting_for_engine) return;
    fail(tr("The engine opened this project without migrating it; Studio and "
            "its player disagree on the project's format."));
}

void ProjectMigrationWidget::end() {
    stage_ = Stage::idle;
    project_root_.clear();
    plan_request_ = 0;
    apply_request_ = 0;
    expected_ = {};
    replanned_after_stale_ = false;
    root_->clear();
    documents_->clear();
    documents_->show();
    versions_->hide();
    manual_notice_->hide();
    backup_->hide();
    backup_->clear();
    setStatus({});
    migrate_button_->setEnabled(false);
    migrate_button_->show();
    close_button_->setEnabled(true);
    close_button_->show();
    title_->setText(tr("This project was saved in an earlier format."));
}

bool ProjectMigrationWidget::acceptRpcResult(qint64 request_id,
                                             const QByteArray &result_json) {
    if (request_id == 0 || (request_id != plan_request_ && request_id != apply_request_)) return false;
    const GuiTraceSpan trace{"migration_json_parse", double(result_json.size())};
    const QJsonObject result = QJsonDocument::fromJson(result_json).object();
    if (request_id == plan_request_) {
        plan_request_ = 0;
        showPlan(result);
        return true;
    }
    if (request_id == apply_request_) {
        apply_request_ = 0;
        showApplied(result);
        return true;
    }
    return false;
}

bool ProjectMigrationWidget::acceptRpcRejection(qint64 request_id,
                                                const QString &error_code,
                                                const QString &message) {
    if (request_id == 0 ||
        (request_id != plan_request_ && request_id != apply_request_))
        return false;
    const bool was_apply = request_id == apply_request_;
    plan_request_ = 0;
    apply_request_ = 0;
    const Refusal refusal = readRefusal(error_code, message);
    if (refusal.code == QLatin1String(RestartRequiredCode)) {
        // This player already applied a migration (another click, another
        // client): what it holds is stale, and a new engine opens the project.
        finishMigration({}, tr("The project was already migrated."));
        return true;
    }
    if (was_apply && refusal.code == QLatin1String(PlanStaleCode)) {
        // Something wrote a document after the plan was read. Nothing was
        // written; the plan is read again and shown again, and applying is
        // the person's decision again.
        replanned_after_stale_ = true;
        setStatus(tr("The project changed on disk; reading the plan again..."));
        requestPlan();
        return true;
    }
    if (was_apply && !expected_.isEmpty()) {
        // The transaction rolled back (project.migration_apply_failed@1) or a
        // step failed: the plan on screen still stands, and so does the
        // button.
        setStage(Stage::planned);
        setStatus(tr("The migration was not applied: %1").arg(oneLine(message)));
        status_->setToolTip(message);
        return true;
    }
    fail(message);
    return true;
}

bool ProjectMigrationWidget::acceptRpcFailure(qint64 request_id,
                                              const QString &message) {
    if (request_id == 0 ||
        (request_id != plan_request_ && request_id != apply_request_))
        return false;
    plan_request_ = 0;
    apply_request_ = 0;
    fail(message);
    return true;
}

void ProjectMigrationWidget::requestPlan() {
    QString error;
    const qint64 id =
        request_ ? request_(QStringLiteral("plan_project_migration"),
                            QJsonObject{}, &error)
                 : 0;
    if (id == 0) {
        fail(error.isEmpty() ? tr("The engine is not running.") : error);
        return;
    }
    plan_request_ = id;
    expected_ = {};
    setStage(Stage::reading_plan);
    if (!replanned_after_stale_) setStatus(tr("Reading the migration plan..."));
}

void ProjectMigrationWidget::requestApply() {
    if (stage_ != Stage::planned || apply_request_ != 0 || expected_.isEmpty())
        return;
    QString error;
    const qint64 id = request_(
        QStringLiteral("apply_project_migration"),
        QJsonObject{{QStringLiteral("expected"), expected_}}, &error);
    if (id == 0) {
        setStatus(tr("The migration was not sent: %1").arg(oneLine(error)));
        return;
    }
    apply_request_ = id;
    replanned_after_stale_ = false;
    setStage(Stage::applying);
    setStatus(tr("Migrating..."));
}

void ProjectMigrationWidget::showPlan(const QJsonObject &plan) {
    if (!plan.value(QStringLiteral("required")).toBool()) {
        finishMigration({}, tr("Nothing is left to migrate."));
        return;
    }

    const QJsonValue saved_by = plan.value(QStringLiteral("saved_by_engine"));
    versions_->setText(
        tr("Saved by engine %1; this engine is %2.")
            .arg(saved_by.isString() ? saved_by.toString()
                                     : tr("(not recorded)"),
                 plan.value(QStringLiteral("engine_version")).toString()));
    versions_->show();

    documents_->clear();
    const QBrush attention{QColor{QLatin1String(AttentionColor)}};
    int manual_count = 0;
    for (const auto &value : plan.value(QStringLiteral("documents")).toArray()) {
        const QJsonObject document = value.toObject();
        auto *row = new QTreeWidgetItem(documents_);
        row->setText(0, document.value(QStringLiteral("path")).toString());
        row->setText(1, formatText(document));
        for (const auto &step : document.value(QStringLiteral("steps")).toArray()) {
            auto *step_row = new QTreeWidgetItem(row);
            step_row->setText(0, tr("step"));
            step_row->setText(1, step.toString());
        }
        const auto manual_items =
            document.value(QStringLiteral("manual_items")).toArray();
        for (const auto &item_value : manual_items) {
            const QJsonObject item = item_value.toObject();
            auto *manual_row = new QTreeWidgetItem(row);
            manual_row->setText(0, tr("fix by hand"));
            const QString pointer =
                item.value(QStringLiteral("pointer")).toString();
            const QString message =
                item.value(QStringLiteral("message")).toString();
            manual_row->setText(
                1, pointer.isEmpty() ? message
                                     : QStringLiteral("%1 (%2)").arg(message,
                                                                     pointer));
            manual_row->setToolTip(
                1, item.value(QStringLiteral("code")).toString());
            manual_row->setForeground(0, attention);
            manual_row->setForeground(1, attention);
            ++manual_count;
        }
        if (!manual_items.isEmpty()) {
            row->setForeground(0, attention);
            row->setForeground(1, attention);
        }
        row->setExpanded(true);
    }
    // The engine applies what its steps could rewrite and reports the rest;
    // it does not stop on them. Said before the button, not after.
    manual_notice_->setText(
        tr("%n place(s) must be fixed by hand after migrating; the rest is "
           "applied.",
           nullptr, manual_count));
    manual_notice_->setVisible(manual_count > 0);

    expected_ = plan.value(QStringLiteral("expected")).toObject();
    setStage(Stage::planned);
    if (expected_.isEmpty()) {
        fail(tr("The engine's plan names no documents to bind the migration "
                "to."));
        return;
    }
    setStatus(replanned_after_stale_
                  ? tr("The project changed on disk, so the plan was read "
                       "again.")
                  : QString{});
}

void ProjectMigrationWidget::showApplied(const QJsonObject &result) {
    const QJsonValue backup = result.value(QStringLiteral("backup"));
    if (!result.value(QStringLiteral("applied")).toBool() ||
        !backup.isString()) {
        finishMigration({}, tr("Nothing is left to migrate."));
        return;
    }
    finishMigration(
        tr("The previous documents were copied to %1")
            .arg(QDir::toNativeSeparators(
                QDir(project_root_).filePath(backup.toString()))),
        tr("Migrated to engine %1.")
            .arg(result.value(QStringLiteral("saved_by_engine_after"))
                     .toString()));
}

void ProjectMigrationWidget::finishMigration(const QString &backup_line,
                                             const QString &status) {
    setStage(Stage::migrated);
    title_->setText(status);
    // What is left on screen is the one line that says where the old
    // documents are; the list and the buttons are done.
    documents_->hide();
    versions_->hide();
    manual_notice_->hide();
    migrate_button_->hide();
    close_button_->hide();
    setStatus({});
    setPanelNotice(backup_, backup_line);
    emit migrated(project_root_, backup_line);
}

void ProjectMigrationWidget::fail(const QString &message) {
    setStage(Stage::failed);
    setStatus(oneLine(message));
    status_->setToolTip(message);
}

void ProjectMigrationWidget::setStage(Stage stage) {
    stage_ = stage;
    migrate_button_->setEnabled(stage == Stage::planned);
    close_button_->setEnabled(stage != Stage::applying &&
                              stage != Stage::migrated);
}

void ProjectMigrationWidget::setStatus(const QString &message) {
    setPanelNotice(status_, message);
}

} // namespace PelicanStudio
