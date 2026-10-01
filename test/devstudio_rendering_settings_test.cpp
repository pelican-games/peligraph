// RSX: a render edit's commit is read one way in every panel, and the Render
// tools' Rendering settings tab (EL-3's edit_rendering_keys). Real Qt widgets
// over the real RPC transport: a child process (process_fixture_child
// rpc-hold) takes every request a panel sends and writes it back on stderr;
// this test answers in the engine's place, through the viewport's own reply
// signal.
//
// Decision 1: a commit that answers restart_required hands the restart to the
// owner's action with "Restarting the engine to apply this change (<code>)..."
// and Render features does not say "Applied without restarting"; one that
// does not ask for a restart calls nothing. Decision 3: the tab reads the
// root's rendering section, sends one edit_rendering_keys per change (set, or
// remove for the engine's default), switches off what a preset root refuses,
// and its edits are the render edit history's.

#include "renderconfigdigest.hpp"
#include "rendereditcompletion.hpp"
#include "renderedithistory.hpp"
#include "renderfeatureswidget.hpp"
#include "renderingsettingswidget.hpp"

// Everything embeddedviewport.hpp includes, first, so that only its own class
// is opened up (as in the Task Editor's test).
#include "engineattach.hpp"
#include "enginefailuremodel.hpp"
#include "engineprocess.hpp"
#include "nativewindowhost.hpp"
#include "viewportgeometry.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTest>
#include <QWidget>

#define private public
#include "embeddedviewport.hpp"
#undef private

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <string>

#ifndef PELICAN_PROCESS_FIXTURE_PATH
#error "PELICAN_PROCESS_FIXTURE_PATH must name the process fixture child"
#endif

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_rendering_settings_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

const std::string RootA(64, 'a');
const std::string RootB(64, 'b');
const std::string RootC(64, 'c');
const std::string BloomFeature = "engine://features/bloom.json";

// The process that takes the panels' requests, and the answers to them.
struct Transport {
    QApplication &app = application();
    EmbeddedViewport viewport;
    std::deque<Json> pending;
    QString buffer;

    Transport() {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}},
            &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        viewport.rpc_ready_ = true;
    }

    void receive(const QString &text) {
        buffer += text;
        for (qsizetype end = buffer.indexOf(QLatin1Char('\n')); end >= 0;
             end = buffer.indexOf(QLatin1Char('\n'))) {
            const QString line = buffer.left(end).trimmed();
            buffer.remove(0, end + 1);
            const QString prefix = QStringLiteral("rpc-hold-request:");
            if (line.startsWith(prefix))
                pending.push_back(Json::parse(line.mid(prefix.size()).toStdString()));
        }
    }

    // The next request of `method` within `milliseconds`; requests of other
    // methods stay unanswered.
    std::optional<Json> next(const std::string &method, int milliseconds = 5000) {
        QElapsedTimer clock;
        clock.start();
        while (true) {
            QApplication::processEvents();
            while (!pending.empty()) {
                Json request = std::move(pending.front());
                pending.pop_front();
                if (request.value("method", std::string{}) == method) return request;
            }
            if (clock.elapsed() >= milliseconds) return std::nullopt;
            QTest::qWait(5);
        }
    }

    void answer(const Json &request, const Json &result) {
        emit viewport.inspectorRpcSucceeded(request.at("id").get<qint64>(),
                                            QByteArray::fromStdString(result.dump()));
        QApplication::processEvents();
    }
};

template <typename Widget> Widget &required(QWidget &parent, const char *name) {
    auto *found = parent.findChild<Widget *>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

Json restartCommit(const std::string &digest) {
    return {{"status", "committed"},
            {"committed", true},
            {"published_generation", 6},
            {"source_digest", {{"hex", digest}}},
            {"documents", {{"passes/main.json", digest}}},
            {"restart_required", true},
            // The engine's own answer (RS-1): `detail` is a list.
            {"restart_reason",
             {{"code", "material_physical_contract_changed"},
              {"detail", Json::array({"color_formats"})},
              {"message", "changes the physical rendering contract of live material 0"}}}};
}

Json liveCommit(const std::string &digest) {
    return {{"status", "committed"},
            {"committed", true},
            {"published_generation", 7},
            {"source_digest", {{"hex", digest}}},
            {"documents", {{"passes/main.json", digest}}}};
}

const QString RestartLine = QStringLiteral(
    "Restarting the engine to apply this change (material_physical_contract_changed)...");

// --- Render features: one refresh, answered (the root lists bloom).
void featuresRefresh(Transport &engine) {
    const auto can_edit = engine.next("can_edit");
    REQUIRE(can_edit);
    engine.answer(*can_edit, {{"can_edit", true}});
    const auto current = engine.next("get_render_features");
    REQUIRE(current);
    engine.answer(*current,
                  {{"source_reference", "passes/main.json"},
                   {"source_digest", {{"hex", RootA}}},
                   {"feature_instances",
                    Json::array({{{"index", 0},
                                  {"reference", BloomFeature},
                                  {"parameters", Json::object()},
                                  {"editable", false},
                                  {"removable", true},
                                  {"scope", "root"},
                                  {"source_reference", "passes/main.json"},
                                  {"source_digest", {{"hex", RootA}}}}})}});
    const auto catalog = engine.next("list_render_features");
    REQUIRE(catalog);
    engine.answer(*catalog, {{"features", Json::array({{{"name", "bloom"},
                                                        {"reference", BloomFeature},
                                                        {"available", true}}})}});
    const auto templates = engine.next("get_graph_templates");
    REQUIRE(templates);
    engine.answer(*templates, {{"templates", Json::array()}});
}

// Remove bloom, Apply, and answer the ticket with `commit`.
void featuresRemoveBloom(Transport &engine, RenderFeaturesWidget &features, const Json &commit) {
    auto &list = required<QListWidget>(features, "pelican.renderFeatures.current");
    REQUIRE(list.count() == 1);
    list.setCurrentRow(0);
    QApplication::processEvents();
    auto &remove = required<QPushButton>(features, "pelican.renderFeatures.remove");
    REQUIRE(remove.isEnabled());
    remove.click();
    auto &apply = required<QPushButton>(features, "pelican.renderFeatures.apply");
    REQUIRE(apply.isEnabled());
    apply.click();
    const auto sent = engine.next("edit_render_features");
    REQUIRE(sent);
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-config-edit-1"}});
    const auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    CHECK(poll->at("params") == Json{{"ticket", "render-config-edit-1"}});
    engine.answer(*poll, commit);
}

// --- Rendering settings: the root as get_render_documents answers it.
Json rootDocuments(const std::string &digest, const Json &rendering) {
    const Json root{{"schema", "pelican.graph"},
                    {"version", 1},
                    {"nodes", Json::array()},
                    {"rendering", rendering}};
    return {{"documents",
             Json::array({{{"reference", "passes/main.json"},
                           {"kind", "root"},
                           {"digest", digest},
                           {"content", root.dump(2)}},
                          {{"reference", BloomFeature},
                           {"kind", "feature"},
                           {"digest", std::string(64, 'f')},
                           {"content", "{}"}}})}};
}

const Json DefaultDrawSort{{"opaque", {{"provider", "state_batched_v1"}}},
                           {"transparent", {{"provider", "back_to_front_v1"}}},
                           {"xr_view_policy", "logical_view_center"}};

void settingsRead(Transport &engine, const std::string &digest, const Json &rendering) {
    const auto read = engine.next("get_render_documents");
    REQUIRE(read);
    CHECK(read->at("params") == Json::object());
    engine.answer(*read, rootDocuments(digest, rendering));
}

} // namespace

TEST_CASE("RSX decision 1: a render edit that commits with restart_required restarts the "
          "engine through the one reading, and Render features does not say it applied live",
          "[devstudio][rsx][restart][render-features][widget]") {
    // The one reading, on its own.
    const auto reading = readRenderEditCompletion(restartCommit(RootB));
    CHECK(reading.committed);
    CHECK(reading.restart_required);
    CHECK(reading.restart_code == "material_physical_contract_changed");
    CHECK(QString::fromStdString(reading.restart_line) == RestartLine);
    CHECK(renderEditRestartLine({}) == "Restarting the engine to apply this change...");
    int called = 0;
    (void)completeRenderEdit(liveCommit(RootB), [&](const std::string &) { ++called; });
    (void)completeRenderEdit(Json{{"status", "rejected"}, {"committed", false},
                                  {"restart_required", true}},
                             [&](const std::string &) { ++called; });
    CHECK(called == 0);
    (void)completeRenderEdit(restartCommit(RootB), [&](const std::string &) { ++called; });
    CHECK(called == 1);

    // Render features, which used to say "Applied without restarting" for
    // every commit.
    Transport engine;
    auto digest = std::make_shared<RenderConfigDigest>();
    RenderFeaturesWidget features{&engine.viewport};
    features.setRenderConfigDigest(digest);
    QStringList restarts;
    features.setEngineRestartAction([&](const QString &reason) { restarts << reason; });
    featuresRefresh(engine);
    auto &notice = required<QLabel>(features, "pelican.renderFeatures.notice");

    SECTION("restart_required: the restart is asked for, with the reason's code") {
        featuresRemoveBloom(engine, features, restartCommit(RootB));
        REQUIRE(restarts == QStringList{RestartLine});
        CHECK(notice.text() == RestartLine);
        CHECK_FALSE(notice.text().contains(QStringLiteral("Applied without restarting")));
        // The other panels still learn the revision the commit wrote.
        CHECK(digest->current() == RootB);
        // Nothing is read from the engine about to stop.
        CHECK_FALSE(engine.next("get_render_features", 200));

        SECTION("declined: the line says why, and the list is read from the engine that stays") {
            features.engineRestartDeclined(QStringLiteral("The engine was not restarted."));
            CHECK(engine.next("get_render_features"));
            CHECK(notice.text() == QStringLiteral("The engine was not restarted."));
        }
    }
    SECTION("no restart: nothing is called, and the panel says it applied live") {
        featuresRemoveBloom(engine, features, liveCommit(RootB));
        CHECK(restarts.isEmpty());
        CHECK(notice.text() ==
              QStringLiteral("Applied without restarting (runtime generation 7)."));
        // A decline of a restart another panel asked for is not this one's.
        features.engineRestartDeclined(QStringLiteral("Someone else's."));
        CHECK(notice.text() != QStringLiteral("Someone else's."));
    }
}

TEST_CASE("RSX decision 3: Rendering settings shows the root's rendering keys, sends one "
          "edit_rendering_keys per change, and switches off what a preset root refuses",
          "[devstudio][rsx][rendering-settings][widget]") {
    // The history records the method and names its entry.
    CHECK(RenderEditHistory::recordsMethod("edit_rendering_keys"));
    CHECK(RenderEditHistory::labelFor(
              "edit_rendering_keys",
              Json{{"base_source_digest", RootA},
                   {"set", {{"shader_defines", Json::array({"RSX_TINT"})}}}}) ==
          "Set shader_defines");
    CHECK(RenderEditHistory::labelFor(
              "edit_rendering_keys",
              Json{{"base_source_digest", RootA}, {"remove", Json::array({"draw_sort"})}}) ==
          "Remove draw_sort");
    CHECK(RenderingSettingsWidget::editableKeys().size() == 8);

    Transport engine;
    auto digest = std::make_shared<RenderConfigDigest>();
    RenderingSettingsWidget settings{&engine.viewport};
    settings.setRenderConfigDigest(digest);
    QStringList restarts;
    settings.setEngineRestartAction([&](const QString &reason) { restarts << reason; });
    // Shown, as in the window: the table opens its editor on a shown view.
    settings.show();
    QApplication::processEvents();
    auto &notice = required<QLabel>(settings, "pelican.renderingSettings.notice");
    auto &draw_sort = required<QComboBox>(settings, "pelican.renderingSettings.draw_sort");
    auto &multisampling =
        required<QComboBox>(settings, "pelican.renderingSettings.multisampling");
    auto &resolver = required<QComboBox>(settings, "pelican.renderingSettings.resolver_version");
    auto &table = required<QTableWidget>(settings, "pelican.renderingSettings.shaderDefines.table");

    // A direct root: every field can be changed.
    settingsRead(engine, RootA,
                 {{"shader_defines", Json::array({"A=1"})}, {"draw_sort", DefaultDrawSort}});
    CHECK(digest->current() == RootA);
    CHECK(draw_sort.currentText() == QStringLiteral("State batched / back to front"));
    CHECK(multisampling.currentText() == QStringLiteral("Off (engine default)"));
    CHECK(multisampling.isEnabled());
    CHECK(resolver.isEnabled());
    REQUIRE(table.rowCount() == 1);
    CHECK(table.item(0, 0)->text() == QStringLiteral("A"));
    CHECK(table.item(0, 1)->text() == QStringLiteral("1"));

    // A define added the way a person adds one: Add, type the name, Return.
    required<QPushButton>(settings, "pelican.renderingSettings.shaderDefines.add").click();
    QApplication::processEvents();
    REQUIRE(table.rowCount() == 2);
    auto *editor = table.viewport()->findChild<QLineEdit *>();
    REQUIRE(editor != nullptr);
    QTest::keyClicks(editor, QStringLiteral("RSX_TINT"));
    QTest::keyClick(editor, Qt::Key_Return);
    auto sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    CHECK(sent->at("params") ==
          Json{{"base_source_digest", RootA},
               {"set", {{"shader_defines", Json::array({"A=1", "RSX_TINT"})}}}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-authored-pass-1"}});
    auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    CHECK(poll->at("params") == Json{{"ticket", "render-authored-pass-1"}});
    engine.answer(*poll, liveCommit(RootB));
    CHECK(restarts.isEmpty());
    CHECK(notice.text() == QStringLiteral("Saved shader_defines."));
    CHECK(digest->current() == RootB);
    // The root is read again: the fields show what it holds now.
    settingsRead(engine, RootB,
                 {{"shader_defines", Json::array({"A=1", "RSX_TINT"})},
                  {"draw_sort", DefaultDrawSort}});
    CHECK(table.rowCount() == 2);

    // Back to the engine's default: the key is removed, not written.
    draw_sort.setCurrentIndex(0);
    sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    CHECK(sent->at("params") ==
          Json{{"base_source_digest", RootB}, {"remove", Json::array({"draw_sort"})}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-authored-pass-2"}});
    poll = engine.next("get_edit_result");
    REQUIRE(poll);
    // Decision 1 here too: a commit that asks for a restart restarts.
    engine.answer(*poll, restartCommit(RootC));
    CHECK(restarts == QStringList{RestartLine});
    CHECK(notice.text() == RestartLine);
    CHECK_FALSE(engine.next("get_render_documents", 200));

    // Another panel (or an Undo) moved the document: the root is read again.
    // This one selects a preset: multisampling and resolver_version are off,
    // with the engine's reason.
    digest->publish(std::string(64, 'd'));
    settingsRead(engine, std::string(64, 'd'),
                 {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}});
    CHECK(draw_sort.currentText() == QStringLiteral("Engine default"));
    CHECK(draw_sort.isEnabled());
    CHECK_FALSE(multisampling.isEnabled());
    CHECK(multisampling.toolTip().startsWith(
        QStringLiteral("preset_root_takes_pipeline_settings_msaa")));
    CHECK_FALSE(resolver.isEnabled());
    CHECK(resolver.toolTip().startsWith(QStringLiteral("not_in_preset_root_keys")));

    // A key the engine refuses here is switched off with its reason, and the
    // fields go back to what the root holds.
    auto &strategy = required<QComboBox>(settings, "pelican.renderingSettings.render_strategy");
    CHECK(strategy.isEnabled());
    auto &xr = required<QPlainTextEdit>(settings, "pelican.renderingSettings.xr");
    auto &xr_apply = required<QPushButton>(settings, "pelican.renderingSettings.xr.apply");
    xr.setPlainText(QStringLiteral("{\"views\": 2}"));
    xr_apply.click();
    sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    CHECK(sent->at("params") == Json{{"base_source_digest", std::string(64, 'd')},
                                     {"set", {{"xr", {{"views", 2}}}}}});
    engine.answer(*sent, {{"status", "rejected"},
                          {"committed", false},
                          {"error",
                           {{"code", "rendering_key_not_allowed_here"},
                            {"key", "xr"},
                            {"reason", "test_reason"},
                            {"message", "not here"}}}});
    CHECK(notice.text().contains(QStringLiteral("rendering_key_not_allowed_here")));
    settingsRead(engine, std::string(64, 'd'),
                 {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}});
    CHECK_FALSE(xr.isEnabled());
    CHECK(xr.toolTip() == QStringLiteral("test_reason: not here"));
}

} // namespace PelicanStudio
