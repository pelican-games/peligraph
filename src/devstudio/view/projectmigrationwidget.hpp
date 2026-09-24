#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace PelicanStudio {

// MG-1. What Studio shows instead of the viewport while the project it opens
// was written by an earlier engine (project.migration_required@1): the plan
// the engine reads for it (plan_project_migration), and one button that
// applies exactly that plan (apply_project_migration {expected}) or one that
// does not open the project at all. The widget owns no engine: it asks
// through `request` and is handed the replies to the ids it asked with.
class ProjectMigrationWidget final : public QWidget {
    Q_OBJECT

  public:
    // Returns the request id, or 0 with `error` set when nothing was sent.
    using RequestRpc = std::function<qint64(
        const QString &method, const QJsonObject &params, QString *error)>;

    enum class Stage {
        idle,
        // The project needs migrating; the engine that serves the plan is
        // not up yet.
        waiting_for_engine,
        reading_plan,
        planned,
        applying,
        // Applied (or found nothing left to apply); the engine is restarted.
        migrated,
        // A plan or an apply the engine refused, or an engine that went away.
        failed,
    };

    explicit ProjectMigrationWidget(RequestRpc request,
                                    QWidget *parent = nullptr);

    // `refusal` is project.migration_required@1 as the engine or the offline
    // open wrote it: "<code>: {documents:[{path, schema, from, to}]}". The
    // documents it names are listed at once; their steps come with the plan.
    void begin(const QString &project_root, const QString &refusal);
    // The player that answers plan_project_migration is running: read the
    // plan now, unless one is already being read or shown. After a failure,
    // a new player is asked again.
    void engineReady();
    // The player opened the project as an engine instead of stopping at the
    // gate: this Studio and that player disagree on the project's format, and
    // an engine is never asked to migrate (MG-1b). Said, while waiting for it.
    void engineOpenedWithoutMigrating();
    // Back to idle, forgetting every request in flight.
    void end();

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    [[nodiscard]] bool active() const noexcept { return stage_ != Stage::idle; }
    [[nodiscard]] const QString &projectRoot() const noexcept {
        return project_root_;
    }

    // Each returns whether the id was one this widget asked with. A refusal
    // arrives as acceptRpcRejection (error_code may be empty, when the code
    // is only in the message) and is then no longer this widget's, so the
    // one-line failure that follows it for every caller is not handled twice.
    bool acceptRpcResult(qint64 request_id, const QByteArray &result_json);
    bool acceptRpcRejection(qint64 request_id, const QString &error_code,
                            const QString &message);
    bool acceptRpcFailure(qint64 request_id, const QString &message);

  signals:
    // The project's documents are at this engine's versions now; it can be
    // opened. `backup_line` is the one line naming where the old documents
    // went (empty when nothing was written).
    void migrated(const QString &project_root, const QString &backup_line);
    void closeRequested(const QString &project_root);

  private:
    RequestRpc request_;
    Stage stage_ = Stage::idle;
    QString project_root_;
    qint64 plan_request_ = 0;
    qint64 apply_request_ = 0;
    // The CAS apply is bound to, exactly as the plan returned it.
    QJsonObject expected_;
    bool replanned_after_stale_ = false;

    QLabel *title_ = nullptr;
    QLabel *root_ = nullptr;
    QLabel *versions_ = nullptr;
    QTreeWidget *documents_ = nullptr;
    QLabel *manual_notice_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *backup_ = nullptr;
    QPushButton *migrate_button_ = nullptr;
    QPushButton *close_button_ = nullptr;

    void requestPlan();
    void requestApply();
    void showPlan(const QJsonObject &plan);
    void showApplied(const QJsonObject &result);
    void finishMigration(const QString &backup_line, const QString &status);
    void fail(const QString &message);
    void setStage(Stage stage);
    void setStatus(const QString &message);
};

} // namespace PelicanStudio
