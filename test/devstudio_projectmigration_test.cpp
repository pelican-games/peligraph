#include "projectmigrationwidget.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <utility>
#include <vector>

// MG-1 (GUI). The migration panel, driven by the replies an engine gives it.
// Production registers no migration step yet, so every reply here is written
// by hand in the shape src/project/projectmigration.cpp gives it: a plan, an
// apply, a stale plan, and a player that already applied one.

namespace {

using PelicanStudio::ProjectMigrationWidget;
using Stage = ProjectMigrationWidget::Stage;

struct Request {
    QString method;
    QJsonObject params;
};

// The engine side: every request is recorded, and each gets the next id.
struct FakeEngine {
    std::vector<Request> requests;
    qint64 next_id = 40;
    bool refuse_to_send = false;

    ProjectMigrationWidget::RequestRpc requester() {
        return [this](const QString &method, const QJsonObject &params,
                      QString *error) -> qint64 {
            if (refuse_to_send) {
                if (error != nullptr) *error = QStringLiteral("not running");
                return 0;
            }
            requests.push_back({method, params});
            return ++next_id;
        };
    }
    qint64 lastId() const { return next_id; }
};

QApplication &application() {
    static int argument_count = 1;
    static char name[] = "pelican_devstudio_projectmigration_test";
    static char *arguments[] = {name, nullptr};
    static QApplication app{argument_count, arguments};
    return app;
}

template <typename T> T *named(QWidget &root, const char *name) {
    auto *found = root.findChild<T *>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return found;
}

QByteArray bytes(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

const QString RequiredRefusal = QStringLiteral(
    "project.migration_required@1: {\"documents\":[{\"path\":\"project.json\","
    "\"schema\":\"pelican.project\",\"from\":1,\"to\":2},{\"path\":"
    "\"scenes/main.scene.json\",\"schema\":\"pelican.scene\",\"from\":2,"
    "\"to\":3}]}");

QJsonObject expectedDigests(const QString &scene_digest) {
    return QJsonObject{
        {QStringLiteral("project.json"), QStringLiteral("sha256:aa11")},
        {QStringLiteral("scenes/main.scene.json"), scene_digest},
    };
}

QJsonObject planReply(const QString &root, const QString &scene_digest) {
    const QJsonArray documents{
        QJsonObject{
            {QStringLiteral("path"), QStringLiteral("project.json")},
            {QStringLiteral("schema"), QStringLiteral("pelican.project")},
            {QStringLiteral("from"), 1},
            {QStringLiteral("to"), 2},
            {QStringLiteral("steps"),
             QJsonArray{QStringLiteral("test.project_1_to_2")}},
            {QStringLiteral("digest"), QStringLiteral("sha256:aa11")},
            {QStringLiteral("manual_items"), QJsonArray{}},
        },
        QJsonObject{
            {QStringLiteral("path"), QStringLiteral("scenes/main.scene.json")},
            {QStringLiteral("schema"), QStringLiteral("pelican.scene")},
            {QStringLiteral("from"), 2},
            {QStringLiteral("to"), 3},
            {QStringLiteral("steps"),
             QJsonArray{QStringLiteral("test.scene_2_to_3")}},
            {QStringLiteral("digest"), scene_digest},
            {QStringLiteral("manual_items"),
             QJsonArray{QJsonObject{
                 {QStringLiteral("code"),
                  QStringLiteral("pelican.scene.renamed_component@1")},
                 {QStringLiteral("pointer"),
                  QStringLiteral("/scenes/main/objects/0")},
                 {QStringLiteral("message"),
                  QStringLiteral("rename the old light by hand")},
             }}},
        },
    };
    return QJsonObject{
        {QStringLiteral("project_root"), root},
        {QStringLiteral("required"), true},
        {QStringLiteral("engine_version"), QStringLiteral("0.2.0")},
        {QStringLiteral("saved_by_engine"), QStringLiteral("0.1.0")},
        {QStringLiteral("documents"), documents},
        {QStringLiteral("expected"), expectedDigests(scene_digest)},
    };
}

QJsonObject appliedReply(const QString &root) {
    QJsonObject reply = planReply(root, QStringLiteral("sha256:bb22"));
    reply[QStringLiteral("applied")] = true;
    reply[QStringLiteral("backup")] =
        QStringLiteral(".pelican/migration/20260925T010203Z");
    reply[QStringLiteral("saved_by_engine_after")] = QStringLiteral("0.2.0");
    reply[QStringLiteral("restart_required")] = true;
    return reply;
}

struct Signals {
    std::vector<std::pair<QString, QString>> migrated;
    std::vector<QString> closed;
};

void observe(ProjectMigrationWidget &panel, Signals &seen) {
    QObject::connect(&panel, &ProjectMigrationWidget::migrated, &panel,
                     [&seen](const QString &root, const QString &line) {
                         seen.migrated.emplace_back(root, line);
                     });
    QObject::connect(&panel, &ProjectMigrationWidget::closeRequested, &panel,
                     [&seen](const QString &root) { seen.closed.push_back(root); });
}

} // namespace

TEST_CASE("MG-1 the migration panel shows the plan and applies exactly it",
          "[devstudio][migration][mg1]") {
    application();
    FakeEngine engine;
    ProjectMigrationWidget panel{engine.requester()};
    Signals seen;
    observe(panel, seen);
    const QString root = QDir::tempPath() + QStringLiteral("/mg1_project");

    auto *documents = named<QTreeWidget>(panel, "pelican.projectMigration.documents");
    auto *migrate = named<QPushButton>(panel, "pelican.projectMigration.migrate");
    auto *close = named<QPushButton>(panel, "pelican.projectMigration.close");
    auto *manual = named<QLabel>(panel, "pelican.projectMigration.manualNotice");
    auto *versions = named<QLabel>(panel, "pelican.projectMigration.versions");
    auto *status = named<QLabel>(panel, "pelican.projectMigration.status");
    auto *backup = named<QLabel>(panel, "pelican.projectMigration.backup");

    // The refusal alone: what it names is listed, nothing is asked yet (the
    // engine that serves the plan is not up), and nothing can be applied.
    panel.begin(root, RequiredRefusal);
    REQUIRE(panel.stage() == Stage::waiting_for_engine);
    REQUIRE(documents->topLevelItemCount() == 2);
    REQUIRE(documents->topLevelItem(1)->text(0) ==
            QStringLiteral("scenes/main.scene.json"));
    REQUIRE(documents->topLevelItem(1)->text(1).contains(QStringLiteral("v2")));
    REQUIRE(engine.requests.empty());
    REQUIRE_FALSE(migrate->isEnabled());

    panel.engineReady();
    REQUIRE(engine.requests.size() == 1);
    REQUIRE(engine.requests.back().method ==
            QStringLiteral("plan_project_migration"));
    const qint64 plan_id = engine.lastId();
    // Asking twice is not reading twice.
    panel.engineReady();
    REQUIRE(engine.requests.size() == 1);

    const QJsonObject plan = planReply(root, QStringLiteral("sha256:bb22"));
    REQUIRE(panel.acceptRpcResult(plan_id, bytes(plan)));
    REQUIRE(panel.stage() == Stage::planned);

    // Per document: its path, its format from -> to, its steps, and what a
    // step could not rewrite, in the attention colour.
    REQUIRE(documents->topLevelItemCount() == 2);
    QTreeWidgetItem *scene = documents->topLevelItem(1);
    REQUIRE(scene->text(0) == QStringLiteral("scenes/main.scene.json"));
    REQUIRE(scene->text(1).contains(QStringLiteral("pelican.scene")));
    REQUIRE(scene->text(1).contains(QStringLiteral("v2")));
    REQUIRE(scene->text(1).contains(QStringLiteral("v3")));
    REQUIRE(scene->childCount() == 2);
    REQUIRE(scene->child(0)->text(1) == QStringLiteral("test.scene_2_to_3"));
    QTreeWidgetItem *hand = scene->child(1);
    REQUIRE(hand->text(1).contains(QStringLiteral("rename the old light by hand")));
    REQUIRE(hand->text(1).contains(QStringLiteral("/scenes/main/objects/0")));
    REQUIRE(hand->foreground(1).color() == QColor(QStringLiteral("#d98c00")));
    REQUIRE(documents->topLevelItem(0)->child(0)->text(1) ==
            QStringLiteral("test.project_1_to_2"));
    // A document with nothing to fix by hand is not drawn as one.
    REQUIRE(documents->topLevelItem(0)->foreground(0).color() !=
            QColor(QStringLiteral("#d98c00")));
    // Said before the button, not after: the rest is still applied.
    REQUIRE_FALSE(manual->isHidden());
    REQUIRE(manual->text().contains(QStringLiteral("1 place")));
    REQUIRE(versions->text().contains(QStringLiteral("0.1.0")));
    REQUIRE(versions->text().contains(QStringLiteral("0.2.0")));
    REQUIRE(migrate->isEnabled());

    // Migrate sends the plan's `expected`, unchanged, once.
    migrate->click();
    REQUIRE(engine.requests.size() == 2);
    REQUIRE(engine.requests.back().method ==
            QStringLiteral("apply_project_migration"));
    REQUIRE(engine.requests.back().params.value(QStringLiteral("expected")) ==
            QJsonValue{plan.value(QStringLiteral("expected"))});
    REQUIRE(panel.stage() == Stage::applying);
    REQUIRE_FALSE(migrate->isEnabled());
    REQUIRE_FALSE(close->isEnabled());
    migrate->click();
    REQUIRE(engine.requests.size() == 2);
    const qint64 apply_id = engine.lastId();

    SECTION("a stale plan is read again, shown again, and applied only when "
            "asked") {
        const QString stale = QStringLiteral(
            "project.migration_plan_stale@1: {\"expected\":{},\"actual\":{}}");
        REQUIRE(panel.acceptRpcRejection(
            apply_id, QStringLiteral("project.migration_plan_stale@1"), stale));
        // The one-line failure that follows every refusal is not handled
        // twice.
        REQUIRE_FALSE(panel.acceptRpcFailure(apply_id, stale));
        REQUIRE(engine.requests.size() == 3);
        REQUIRE(engine.requests.back().method ==
                QStringLiteral("plan_project_migration"));
        REQUIRE_FALSE(migrate->isEnabled());

        const QJsonObject moved = planReply(root, QStringLiteral("sha256:cc33"));
        REQUIRE(panel.acceptRpcResult(engine.lastId(), bytes(moved)));
        REQUIRE(panel.stage() == Stage::planned);
        REQUIRE(status->text().contains(QStringLiteral("read again")));
        REQUIRE(engine.requests.size() == 3);
        REQUIRE(seen.migrated.empty());

        migrate->click();
        REQUIRE(engine.requests.size() == 4);
        REQUIRE(engine.requests.back().params.value(QStringLiteral("expected")) ==
                QJsonValue{expectedDigests(QStringLiteral("sha256:cc33"))});
    }

    SECTION("an applied migration names its backup in one line and restarts") {
        REQUIRE(panel.acceptRpcResult(apply_id, bytes(appliedReply(root))));
        REQUIRE(panel.stage() == Stage::migrated);
        REQUIRE(seen.migrated.size() == 1);
        REQUIRE(seen.migrated.front().first == root);
        const QString expected_backup = QDir::toNativeSeparators(
            QDir(root).filePath(QStringLiteral(".pelican/migration/20260925T010203Z")));
        REQUIRE(seen.migrated.front().second.contains(expected_backup));
        REQUIRE_FALSE(backup->isHidden());
        REQUIRE(backup->text() == seen.migrated.front().second);
        REQUIRE_FALSE(backup->text().contains(QLatin1Char('\n')));
        // The list and the buttons are done with.
        REQUIRE(migrate->isHidden());
        REQUIRE(documents->isHidden());
        REQUIRE(seen.closed.empty());
    }

    SECTION("a migration the engine could not apply leaves the plan and the "
            "button") {
        const QString failed = QStringLiteral(
            "project.migration_apply_failed@1: {\"message\":\"disk full\"}");
        REQUIRE(panel.acceptRpcRejection(
            apply_id, QStringLiteral("project.migration_apply_failed@1"),
            failed));
        REQUIRE(panel.stage() == Stage::planned);
        REQUIRE(status->text().contains(QStringLiteral("not applied")));
        REQUIRE(migrate->isEnabled());
        REQUIRE(seen.migrated.empty());
        REQUIRE(engine.requests.size() == 2);
    }

    SECTION("a player that already applied one says restart, and it is "
            "restarted") {
        // The gate's refusal carries the code only in its message.
        REQUIRE(panel.acceptRpcRejection(
            apply_id, QString{},
            QStringLiteral("project.migration_restart_required@1: "
                           "{\"message\":\"start a new player\"}")));
        REQUIRE(seen.migrated.size() == 1);
        REQUIRE(seen.migrated.front().second.isEmpty());
    }

    SECTION("an engine that went away before answering is said so") {
        REQUIRE(panel.acceptRpcFailure(
            apply_id, QStringLiteral("pelican_player exited")));
        REQUIRE(panel.stage() == Stage::failed);
        REQUIRE(status->text().contains(QStringLiteral("exited")));
        REQUIRE(seen.migrated.empty());
        REQUIRE(close->isEnabled());
    }
}

TEST_CASE("MG-1 Don't Open closes the project without migrating it",
          "[devstudio][migration][mg1]") {
    application();
    FakeEngine engine;
    ProjectMigrationWidget panel{engine.requester()};
    Signals seen;
    observe(panel, seen);
    const QString root = QDir::tempPath() + QStringLiteral("/mg1_project");

    panel.begin(root, RequiredRefusal);
    panel.engineReady();
    REQUIRE(panel.acceptRpcResult(
        engine.lastId(), bytes(planReply(root, QStringLiteral("sha256:bb22")))));
    named<QPushButton>(panel, "pelican.projectMigration.close")->click();
    REQUIRE(seen.closed == std::vector<QString>{root});
    REQUIRE(seen.migrated.empty());
    REQUIRE(panel.stage() == Stage::idle);
    // Nothing was applied, and a late reply is no longer this panel's.
    REQUIRE(engine.requests.size() == 1);
    REQUIRE_FALSE(panel.acceptRpcResult(engine.lastId(), bytes(QJsonObject{})));
}

TEST_CASE("MG-1 a plan with nothing left to migrate opens the project",
          "[devstudio][migration][mg1]") {
    application();
    FakeEngine engine;
    ProjectMigrationWidget panel{engine.requester()};
    Signals seen;
    observe(panel, seen);
    const QString root = QDir::tempPath() + QStringLiteral("/mg1_project");

    panel.begin(root, RequiredRefusal);
    panel.engineReady();
    QJsonObject nothing = planReply(root, QStringLiteral("sha256:bb22"));
    nothing[QStringLiteral("required")] = false;
    nothing[QStringLiteral("documents")] = QJsonArray{};
    nothing[QStringLiteral("expected")] = QJsonObject{};
    REQUIRE(panel.acceptRpcResult(engine.lastId(), bytes(nothing)));
    REQUIRE(seen.migrated.size() == 1);
    // Nothing was applied, so nothing is sent.
    REQUIRE(engine.requests.size() == 1);
}
