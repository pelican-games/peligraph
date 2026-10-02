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

#include "featureparameterform.hpp"
#include "featurerefusalfix.hpp"
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
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSlider>
#include <QToolButton>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTest>
#include <QTreeWidget>
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

// --- UX-10, decision 1.
const std::string SkyFeature = "engine://features/sky_ambient.json";

Json uxSchema() {
    const auto scalar = [](const char *name, const char *type, Json range, Json fallback) {
        Json declaration{{"name", name}, {"type", type}, {"default", fallback}};
        if (!range.is_null()) declaration["range"] = range;
        return declaration;
    };
    return {{"schema", "pelican.render_feature_parameters"},
            {"version", 1},
            {"scalars",
             Json::array({scalar("color_r", "float", {0.0, 64.0}, 0.12),
                          scalar("color_g", "float", {0.0, 64.0}, 0.16),
                          scalar("color_b", "float", {0.0, 64.0}, 0.24),
                          scalar("sky_intensity", "float", {0.0, 64.0}, 1.0),
                          scalar("cascade_count", "int", {1, 8}, 1),
                          scalar("plane_offset", "float", {-10.0, 10.0}, 0.0),
                          scalar("plane_x", "float", {-10.0, 10.0}, 0.0),
                          scalar("plane_y", "float", {-10.0, 10.0}, 1.0),
                          scalar("plane_z", "float", {-10.0, 10.0}, 0.0),
                          scalar("stabilize", "bool", nullptr, true)})},
            {"shader_assets",
             Json::array({{{"name", "prefilter_shader"},
                           {"stage", "compute"},
                           {"default", "engine://prefilter"}}})}};
}

void uxFeaturesRead(Transport &engine, const std::string &digest, const Json &parameters) {
    const auto can_edit = engine.next("can_edit");
    REQUIRE(can_edit);
    engine.answer(*can_edit, {{"can_edit", true}});
    const auto current = engine.next("get_render_features");
    REQUIRE(current);
    engine.answer(*current, {{"source_reference", "passes/main.json"},
                             {"source_digest", {{"hex", digest}}},
                             {"feature_instances",
                              Json::array({{{"index", 0},
                                            {"reference", SkyFeature},
                                            {"parameters", parameters},
                                            {"parameter_schema", uxSchema()},
                                            {"editable", true},
                                            {"removable", true},
                                            {"scope", "root"},
                                            {"source_reference", "passes/main.json"},
                                            {"source_digest", {{"hex", digest}}}}})}});
}

// The parameter edit the panel sent, accepted; and the poll for its result.
std::pair<Json, Json> uxSent(Transport &engine, const std::string &digest) {
    const auto sent = engine.next("edit_render_features", 3000);
    REQUIRE(sent);
    const auto &params = sent->at("params");
    CHECK(params.at("base_source_digest") == digest);
    const auto &operation = params.at("operations").at(0);
    CHECK(operation.at("op") == "set_parameters");
    CHECK(operation.at("index") == 0);
    CHECK(operation.at("scope") == "root");
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-config-edit-ux10"}});
    const auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    return {operation.at("parameters"), *poll};
}

template <typename Widget> Widget &dialogChild(QDialog &dialog, const char *name) {
    auto *found = dialog.findChild<Widget *>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

TEST_CASE("UX-10 decision 1: a feature's parameters are fields made from its schema -- a colour, "
          "a vector, a slider -- and a change is sent at once, gathered while it moves",
          "[devstudio][ux10][render-features][widget]") {
    // The rows: r/g/b one colour, x/y/z/offset one vector, wherever they sit.
    const auto rows = featureParameterRows(
        QJsonDocument::fromJson(QByteArray::fromStdString(uxSchema().dump())).object());
    REQUIRE(rows.size() == 6);
    using Kind = FeatureParameterRow::Kind;
    CHECK(rows[0].kind == Kind::Color);
    CHECK(rows[0].label == QStringLiteral("color"));
    CHECK(rows[0].names == QStringList{"color_r", "color_g", "color_b"});
    CHECK(rows[1].kind == Kind::Number);
    CHECK(rows[2].type == QStringLiteral("int"));
    CHECK(rows[3].kind == Kind::Vector);
    CHECK(rows[3].names == QStringList{"plane_x", "plane_y", "plane_z", "plane_offset"});
    CHECK(rows[4].kind == Kind::Boolean);
    CHECK(rows[5].kind == Kind::ShaderAsset);
    CHECK(rows[5].stage == QStringLiteral("compute"));

    // SC-1: a materialized feature's schema says the engine's own default
    // beside the value it was cut with; Reset goes back to the engine's.
    (void)application();
    {
        const auto schema = QJsonDocument::fromJson(R"({"scalars": [
            {"name": "color_r", "type": "float", "range": [0, 64], "default": 0.7, "engine_default": 0.12},
            {"name": "color_g", "type": "float", "range": [0, 64], "default": 0.16, "engine_default": 0.16},
            {"name": "color_b", "type": "float", "range": [0, 64], "default": 0.24, "engine_default": 0.24},
            {"name": "sky_intensity", "type": "float", "range": [0, 64], "default": 1.0}]})")
                                .object();
        FeatureParameterForm form{schema, QJsonObject{}, QJsonObject{{"color_r", 0.7}}};
        int edits = 0;
        form.setEditedCallback([&](bool) { ++edits; });
        form.show();
        auto *colour_reset = form.findChild<QToolButton *>(QStringLiteral("pelican.featureParameters.color.reset"));
        REQUIRE(colour_reset != nullptr);
        // 0.7 is shown, and it is not the engine's 0.12: Reset is offered.
        CHECK(form.findChild<QDoubleSpinBox *>(QStringLiteral("pelican.featureParameters.color_r"))->value() == 0.7);
        CHECK(colour_reset->isVisibleTo(&form));
        CHECK(colour_reset->toolTip().contains(QStringLiteral("0.12")));
        CHECK_FALSE(form.findChild<QToolButton *>(QStringLiteral("pelican.featureParameters.sky_intensity.reset"))
                        ->isVisibleTo(&form));
        colour_reset->click();
        CHECK(edits == 1);
        CHECK(form.authored().value(QStringLiteral("color_r")).toDouble() == 0.12);
        CHECK_FALSE(colour_reset->isVisibleTo(&form));
    }

    Transport engine;
    auto digest = std::make_shared<RenderConfigDigest>();
    RenderFeaturesWidget features{&engine.viewport};
    features.setRenderConfigDigest(digest);
    uxFeaturesRead(engine, RootA, Json::object());
    auto &list = required<QListWidget>(features, "pelican.renderFeatures.current");
    REQUIRE(list.count() == 1);
    list.setCurrentRow(0);
    QApplication::processEvents();
    auto &open = required<QPushButton>(features, "pelican.renderFeatures.parameters");
    REQUIRE(open.isEnabled());
    open.click();
    QApplication::processEvents();
    auto *dialog = features.findChild<QDialog *>(QStringLiteral("pelican.featureParameters"));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->isVisible());
    // Opened, not run: the panel goes on under it.
    CHECK_FALSE(dialog->isModal());
    // Return in a field gives the value; it does not close the dialog.
    QTest::keyClick(dialog->findChild<QDoubleSpinBox *>(QStringLiteral("pelican.featureParameters.color_r")),
                    Qt::Key_Return);
    QApplication::processEvents();
    REQUIRE(dialog->isVisible());
    // The typed rows: an int without decimals, a vector's fourth, the
    // shader's default offered by name.
    CHECK(dialogChild<QDoubleSpinBox>(*dialog, "pelican.featureParameters.cascade_count").decimals() ==
          0);
    (void)dialogChild<QDoubleSpinBox>(*dialog, "pelican.featureParameters.plane_offset");
    auto &shader = dialogChild<QComboBox>(*dialog, "pelican.featureParameters.prefilter_shader");
    CHECK(shader.currentData().toString() == QStringLiteral("engine://prefilter"));

    // The colour: its numbers under the swatch, and its strength (the range
    // goes past 1). A number typed goes at once, with the colour it is part of.
    auto &red = dialogChild<QDoubleSpinBox>(*dialog, "pelican.featureParameters.color_r");
    CHECK(red.value() == 0.12);
    CHECK(dialogChild<QDoubleSpinBox>(*dialog, "pelican.featureParameters.color.strength").value() ==
          1.0);
    (void)dialogChild<QPushButton>(*dialog, "pelican.featureParameters.color.swatch");
    red.setValue(0.5);
    auto [parameters, poll] = uxSent(engine, RootA);
    const Json coloured{{"color_r", 0.5}, {"color_g", 0.16}, {"color_b", 0.24}};
    CHECK(parameters == coloured);
    engine.answer(poll, liveCommit(RootB));
    CHECK(digest->current() == RootB);
    uxFeaturesRead(engine, RootB, coloured);

    // A slider moved three times within 150 ms is one edit, with where it
    // stopped.
    auto &slider = dialogChild<QSlider>(*dialog, "pelican.featureParameters.sky_intensity.slider");
    auto &intensity = dialogChild<QDoubleSpinBox>(*dialog, "pelican.featureParameters.sky_intensity");
    slider.setValue(100);
    QTest::qWait(30);
    slider.setValue(200);
    QTest::qWait(30);
    slider.setValue(300);
    CHECK(intensity.value() == 19.2);
    std::tie(parameters, poll) = uxSent(engine, RootB);
    auto brighter = coloured;
    brighter["sky_intensity"] = 19.2;
    CHECK(parameters == brighter);
    engine.answer(poll, liveCommit(RootC));
    uxFeaturesRead(engine, RootC, brighter);

    // Reset: offered on the changed row only, and back to the default.
    auto &reset = dialogChild<QToolButton>(*dialog, "pelican.featureParameters.sky_intensity.reset");
    CHECK(reset.isVisibleTo(dialog));
    CHECK_FALSE(dialogChild<QToolButton>(*dialog, "pelican.featureParameters.plane.reset")
                    .isVisibleTo(dialog));
    reset.click();
    std::tie(parameters, poll) = uxSent(engine, RootC);
    CHECK(parameters == coloured);
    CHECK(intensity.value() == 1.0);
    const std::string RootD(64, 'd');
    engine.answer(poll, liveCommit(RootD));
    uxFeaturesRead(engine, RootD, coloured);

    // An Undo (another revision of the document): the fields show it.
    const std::string RootE(64, 'e');
    digest->publish(RootE);
    uxFeaturesRead(engine, RootE, Json::object());
    CHECK(red.value() == 0.12);
    CHECK_FALSE(dialogChild<QToolButton>(*dialog, "pelican.featureParameters.color.reset")
                    .isVisibleTo(dialog));
    // Nothing more was sent.
    CHECK_FALSE(engine.next("edit_render_features", 300));
}

TEST_CASE("UX-10 decision 3: Rendering settings picks material_route_default from Auto / "
          "Forward and sends set_material_route_default; the restart it needs is asked for",
          "[devstudio][ux10][rendering-settings][widget]") {
    CHECK(RenderEditHistory::recordsMethod("set_material_route_default"));
    Transport engine;
    auto digest = std::make_shared<RenderConfigDigest>();
    RenderingSettingsWidget settings{&engine.viewport};
    settings.setRenderConfigDigest(digest);
    QStringList restarts;
    settings.setEngineRestartAction([&](const QString &reason) { restarts << reason; });
    settings.show();
    QApplication::processEvents();
    auto &route = required<QComboBox>(settings, "pelican.renderingSettings.material_route_default");
    settingsRead(engine, RootA, Json::object());
    // Not written: the engine's default.
    CHECK(route.currentText() == QStringLiteral("Auto (engine default)"));
    CHECK(route.isEnabled());
    route.setCurrentIndex(route.findData(QStringLiteral("forward")));
    auto sent = engine.next("set_material_route_default");
    REQUIRE(sent);
    CHECK(sent->at("params") ==
          Json{{"base_source_digest", RootA}, {"material_route_default", "forward"}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-route-1"}});
    auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    engine.answer(*poll, restartCommit(RootB));
    CHECK(restarts == QStringList{RestartLine});
    // The engine that comes back reads the root: Forward is what it holds.
    settings.engineRestartDeclined(QStringLiteral("not restarted"));
    settingsRead(engine, RootB, {{"material_route_default", "forward"}});
    CHECK(route.currentText() == QStringLiteral("Forward"));
    // Back to Auto is written as "auto" (the engine reads it at start).
    route.setCurrentIndex(0);
    sent = engine.next("set_material_route_default");
    REQUIRE(sent);
    CHECK(sent->at("params").at("material_route_default") == "auto");
}

TEST_CASE("UX-10 decision 7: draw_sort and render_strategy are picked from the providers the "
          "engine registered (a game's marked); an engine without the list keeps the fixed entries",
          "[devstudio][ux10][rendering-settings][widget]") {
    Transport engine;
    RenderingSettingsWidget settings{&engine.viewport};
    settings.show();
    QApplication::processEvents();
    auto &draw_sort = required<QComboBox>(settings, "pelican.renderingSettings.draw_sort");
    auto &strategy = required<QComboBox>(settings, "pelican.renderingSettings.render_strategy");
    settingsRead(engine, RootA, {{"render_strategy", {{"provider", "builtin.authored_config_v1"},
                                                      {"name", "main"}}}});
    // EL-6's answer, written by hand.
    auto asked = engine.next("get_registered_providers");
    REQUIRE(asked);
    CHECK(asked->at("params") == Json::object());
    engine.answer(*asked,
                  {{"draw_sort", Json::array({{{"name", "back_to_front_v1"}, {"source", "engine"}, {"provider_version", 1}},
                                              {{"name", "my_sort"}, {"source", "game"}, {"provider_version", 1}}})},
                   {"pass_implementation", Json::array()},
                   {"region_replacements", Json::array()},
                   {"graph_transforms", Json::array()},
                   {"render_strategy",
                    Json::array({{{"name", "builtin.authored_config_v1"}, {"source", "engine"}, {"provider_version", 1}},
                                 {{"name", "my_strategy"}, {"source", "game"}, {"provider_version", 1}}})}});
    QStringList sorts;
    for (int index = 0; index < draw_sort.count(); ++index) sorts << draw_sort.itemText(index);
    CHECK(sorts == QStringList{"Engine default", "back_to_front_v1 / back_to_front_v1",
                               "back_to_front_v1 / my_sort (game)", "my_sort (game) / back_to_front_v1",
                               "my_sort (game) / my_sort (game)"});
    // The provider the root names is the one shown; picking another keeps
    // the name it reads.
    CHECK(strategy.currentText() == QStringLiteral("builtin.authored_config_v1"));
    strategy.setCurrentIndex(strategy.findText(QStringLiteral("my_strategy (game)")));
    auto sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    CHECK(sent->at("params") ==
          Json{{"base_source_digest", RootA},
               {"set", {{"render_strategy", {{"provider", "my_strategy"}, {"name", "main"}}}}}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-keys-7"}});
    auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    engine.answer(*poll, liveCommit(RootB));
    // Read again: the list is asked again (a game reload changes it); this
    // engine has no such method, and the fixed entries come back.
    settingsRead(engine, RootB, Json::object());
    asked = engine.next("get_registered_providers");
    REQUIRE(asked);
    emit engine.viewport.inspectorRpcFailed(asked->at("id").get<qint64>(),
                                            QStringLiteral("RPC -32601: method not found"));
    QApplication::processEvents();
    CHECK(draw_sort.count() == 3);
    CHECK(draw_sort.itemText(1) == QStringLiteral("State batched / back to front"));
    CHECK(strategy.count() == 1);
}

TEST_CASE("UX-10 decision 4: Snapshots are rows -- + adds one after the opaque pass, x takes it "
          "away -- and XR's view execution is a combo, the whole xr JSON under Advanced",
          "[devstudio][ux10][rendering-settings][widget]") {
    Transport engine;
    auto digest = std::make_shared<RenderConfigDigest>();
    RenderingSettingsWidget settings{&engine.viewport};
    settings.setRenderConfigDigest(digest);
    settings.show();
    QApplication::processEvents();
    settingsRead(engine, RootA, Json::object());
    auto &add = required<QPushButton>(settings, "pelican.renderingSettings.snapshots.add");
    CHECK(add.isEnabled());
    CHECK(settings.findChild<QLineEdit *>(QStringLiteral("pelican.renderingSettings.snapshots.row0.name")) ==
          nullptr);
    // +: one row, named and placed without typing.
    add.click();
    auto sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    const Json one = Json::array({{{"name", "opaque_snapshot"}, {"after", "post_ldr"}}});
    CHECK(sent->at("params") == Json{{"base_source_digest", RootA}, {"set", {{"snapshots", one}}}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-keys-1"}});
    auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    engine.answer(*poll, liveCommit(RootB));
    settingsRead(engine, RootB, {{"snapshots", one}});
    QApplication::processEvents();
    auto &name = required<QLineEdit>(settings, "pelican.renderingSettings.snapshots.row0.name");
    CHECK(name.text() == QStringLiteral("opaque_snapshot"));
    auto &after = required<QComboBox>(settings, "pelican.renderingSettings.snapshots.row0.after");
    CHECK(after.count() == 1);
    CHECK(after.currentData().toString() == QStringLiteral("post_ldr"));
    // v1 takes one snapshot: + is off while there is one.
    CHECK_FALSE(add.isEnabled());
    // x: the key is removed.
    required<QPushButton>(settings, "pelican.renderingSettings.snapshots.row0.remove").click();
    sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    CHECK(sent->at("params") ==
          Json{{"base_source_digest", RootB}, {"remove", Json::array({"snapshots"})}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-keys-2"}});
    poll = engine.next("get_edit_result");
    REQUIRE(poll);
    engine.answer(*poll, liveCommit(RootC));
    // XR: view_execution picked; the rest of xr (its profiles) kept as it is.
    const Json profiles{{"multiview_auto", {{"min_views", 2}}}};
    settingsRead(engine, RootC, {{"xr", profiles}});
    auto &execution = required<QComboBox>(settings, "pelican.renderingSettings.xr.viewExecution");
    CHECK(execution.currentText() == QStringLiteral("Auto (engine default)"));
    auto &advanced = required<QPushButton>(settings, "pelican.renderingSettings.xr.advanced");
    auto &xr = required<QPlainTextEdit>(settings, "pelican.renderingSettings.xr");
    CHECK_FALSE(xr.isVisibleTo(&settings));
    advanced.click();
    CHECK(xr.isVisibleTo(&settings));
    execution.setCurrentIndex(execution.findData(QStringLiteral("multiview")));
    sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    auto multiview = profiles;
    multiview["view_execution"] = "multiview";
    CHECK(sent->at("params") == Json{{"base_source_digest", RootC}, {"set", {{"xr", multiview}}}});
}


// --- UX-12b (engine FR-1): refusals the engine names with what is missing,
// and the one press that supplies it.
const Json DeviceDepth{{"semantic", "pelican.render.depth@1"},
                       {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
const Json OpaqueImage{{"semantic", "pelican.render.legacy_opaque_image@1"},
                       {"arguments", Json::object()}};

// Render features read with no feature, and a catalog of sky_ambient as
// list_render_features gives it (FR-1's required_targets; `insertion` only
// from an engine with FR-2).
void ux12bFeaturesRead(Transport &engine, const std::string &digest, const Json &extra = Json::object()) {
    const auto can_edit = engine.next("can_edit");
    REQUIRE(can_edit);
    engine.answer(*can_edit, {{"can_edit", true}});
    const auto current = engine.next("get_render_features");
    REQUIRE(current);
    engine.answer(*current, {{"source_reference", "passes/main.json"},
                             {"source_digest", {{"hex", digest}}},
                             {"feature_instances", Json::array()}});
    const auto catalog = engine.next("list_render_features");
    REQUIRE(catalog);
    Json sky{{"name", "sky_ambient"},
             {"reference", SkyFeature},
             {"available", true},
             {"required_targets",
              Json::array({{{"name", "lit_color"}, {"type", OpaqueImage}, {"usage", {"COLOR_ATTACHMENT"}}},
                           {{"name", "scene_depth"}, {"type", DeviceDepth}, {"usage", {"SAMPLED"}}}})}};
    sky.update(extra);
    engine.answer(*catalog, {{"features", Json::array({sky})}});
    const auto templates = engine.next("get_graph_templates");
    REQUIRE(templates);
    engine.answer(*templates, {{"templates", Json::array()}});
}

// What the panel reads before it asks or sends: the authoring context and
// the Graph's nodes.
void ux12bDocumentRead(Transport &engine, const Json &targets, const Json &nodes,
                       const Json &passes = Json::array()) {
    const auto context = engine.next("get_render_authoring_context");
    REQUIRE(context);
    engine.answer(*context, {{"render_targets", targets},
                             {"graphs", Json::array({{{"graph", "main_render"}, {"passes", passes}}})}});
    const auto graph = engine.next("get_graph_instances");
    REQUIRE(graph);
    engine.answer(*graph, {{"nodes", nodes}, {"definitions", Json::object()}});
}

// One edit of `method`, accepted, and its ticket answered with `result`.
Json ux12bEdit(Transport &engine, const std::string &method, const Json &result) {
    const auto sent = engine.next(method);
    REQUIRE(sent);
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "ux12b-" + method}});
    const auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    CHECK(poll->at("params") == Json{{"ticket", "ux12b-" + method}});
    engine.answer(*poll, result);
    return sent->at("params");
}

Json ux12bRejected(Json error) {
    return {{"status", "rejected"}, {"committed", false}, {"error", std::move(error)}};
}

void ux12bPickCatalog(RenderFeaturesWidget &features) {
    auto &catalog = required<QTreeWidget>(features, "pelican.renderFeatures.catalog");
    REQUIRE(catalog.topLevelItemCount() == 1);
    catalog.setCurrentItem(catalog.topLevelItem(0));
    QApplication::processEvents();
}

TEST_CASE("UX-12b decision 1: a feature's required targets are offered before it is added, and "
          "Add with targets declares them, references them in the Graph and adds it, in order",
          "[devstudio][ux12b][render-features][widget]") {
    // The model: what is missing, the line, and the ops.
    const Json sky_entry{{"required_targets",
                          Json::array({{{"name", "lit_color"}, {"type", OpaqueImage}},
                                       {{"name", "scene_depth"}, {"type", DeviceDepth}, {"usage", {"SAMPLED"}}}})}};
    const auto required_targets = featureRequiredTargets(sky_entry);
    REQUIRE(required_targets.size() == 2);
    // lit_color is declared: only the depth is missing, and the Graph has no
    // reference to it.
    const auto missing = missingRequiredTargets(required_targets, {"lit_color"}, Json::array());
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].name == "scene_depth");
    CHECK(missing[0].declare);
    CHECK(missing[0].reference);
    CHECK(requiredTargetsQuestion("sky_ambient", missing) ==
          "sky_ambient needs scene_depth (depth). Add it with the feature?");
    CHECK(requiredTargetsQuestion("sky_ambient", missingRequiredTargets(required_targets, {}, Json::array())) ==
          "sky_ambient needs lit_color (color) and scene_depth (depth). Add them with the feature?");
    CHECK(missingRequiredTargets(required_targets, {"lit_color", "scene_depth"}, Json::array()).empty());
    // The refusal names one target the same way.
    const auto named = requiredTargetFromRefusal({{"code", "feature.required_target_missing@1"},
                                                  {"feature", "sky_ambient"},
                                                  {"target", "scene_depth"},
                                                  {"type", DeviceDepth},
                                                  {"document", SkyFeature}});
    REQUIRE(named);
    CHECK(named->name == "scene_depth");

    Transport engine;
    auto digest = std::make_shared<RenderConfigDigest>();
    RenderFeaturesWidget features{&engine.viewport};
    features.setRenderConfigDigest(digest);
    features.show();
    ux12bFeaturesRead(engine, RootA);
    auto &notice = required<QLabel>(features, "pelican.renderFeatures.notice");
    auto &with_targets = required<QPushButton>(features, "pelican.renderFeatures.addWithTargets");
    CHECK_FALSE(with_targets.isVisibleTo(&features));

    // Add: the document is read, and what it lacks asked about.
    ux12bPickCatalog(features);
    required<QPushButton>(features, "pelican.renderFeatures.add").click();
    ux12bDocumentRead(engine, Json::array({{{"name", "lit_color"}}}), Json::array());
    CHECK(notice.text() == QStringLiteral("sky_ambient needs scene_depth (depth). Add it with the feature?"));
    REQUIRE(with_targets.isVisibleTo(&features));
    REQUIRE(with_targets.isEnabled());

    // Add with targets: read again, then (UX-12d, decision 2) the targets,
    // their references and the feature as one edit_batch.
    with_targets.click();
    ux12bDocumentRead(engine, Json::array({{{"name", "lit_color"}}}),
                      Json::array({{{"id", "scene_depth_ref"}, {"op", "render.image"}}}));
    const std::string RootD(64, 'd');
    const auto batch = ux12bEdit(engine, "edit_batch", liveCommit(RootD));
    CHECK(batch ==
          Json{{"base_source_digest", RootA},
               {"edits",
                Json::array(
                    {{{"method", "edit_render_targets"},
                      {"params",
                       {{"ops", Json::array({{{"op", "add"},
                                              {"target",
                                               {{"name", "scene_depth"},
                                                {"extent_scale", 1.0},
                                                {"format", "D32_SFLOAT"},
                                                {"format_class", "data"},
                                                {"usage", {"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}}}}}})}}}},
                     // Beside a node that already has the plain name.
                     {{"method", "edit_graph_document"},
                      {"params",
                       {{"ops", Json::array({{{"op", "add_node"},
                                              {"scope", "root"},
                                              {"node",
                                               {{"id", "scene_depth_ref_2"},
                                                {"op", "render.target_ref"},
                                                {"parameters", {{"name", "scene_depth"}, {"type", DeviceDepth}}},
                                                {"outputs", {"resource"}}}}}})}}}},
                     {{"method", "edit_render_features"},
                      {"params", {{"operations", Json::array({{{"op", "add"}, {"feature", SkyFeature}}})}}}}})}});
    CHECK(digest->current() == RootD);
    CHECK(notice.text() == QStringLiteral("Added sky_ambient with the render targets it needs."));
    ux12bFeaturesRead(engine, RootD);
    // UX-12d, decision 4: what it draws is looked at; here nothing to show.
    ux12bDocumentRead(engine, Json::array({{{"name", "lit_color"}}}), Json::array());
    CHECK(notice.text() == QStringLiteral("Added sky_ambient with the render targets it needs."));

    // The refusal of a plain Apply offers the same button.
    ux12bPickCatalog(features);
    required<QPushButton>(features, "pelican.renderFeatures.add").click();
    // (A document that declares both: nothing asked.)
    ux12bDocumentRead(engine, Json::array({{{"name", "lit_color"}}, {{"name", "scene_depth"}}}), Json::array());
    CHECK_FALSE(with_targets.isVisibleTo(&features));
    required<QPushButton>(features, "pelican.renderFeatures.apply").click();
    (void)ux12bEdit(engine, "edit_render_features",
                    ux12bRejected({{"code", "feature.required_target_missing@1"},
                                   {"message", "sky_ambient needs scene_depth"},
                                   {"feature", "sky_ambient"},
                                   {"target", "scene_depth"},
                                   {"type", DeviceDepth},
                                   {"document", SkyFeature}}));
    CHECK(notice.text() == QStringLiteral("sky_ambient needs scene_depth (depth). Add it with the feature?"));
    CHECK(with_targets.isVisibleTo(&features));
}

TEST_CASE("UX-12b decision 2: a feature with no pass to go after is refused as such, nothing "
          "added for it stays, and Place offers the pass on the Graph tab",
          "[devstudio][ux12b][render-features][widget]") {
    // The model: FR-2's named refusal (develop 0b15fb62's shape), and an engine without it.
    const auto named = insertionAnchorMissing({{"code", "feature.insertion_anchor_missing@1"},
                                               {"feature", "sky_ambient"},
                                               {"anchor", "deferred_lighting"},
                                               {"candidates", {"lighting_pass", "present", "post_ldr"}},
                                               {"document", SkyFeature}});
    REQUIRE(named);
    CHECK(named->anchor == "deferred_lighting");
    CHECK(named->candidates == std::vector<std::string>{"lighting_pass", "present", "post_ldr"});
    const auto sentence = insertionAnchorMissing(
        {{"code", "render_pipeline_preflight_failed"},
         {"message", "render feature pass insert anchor was not found: deferred_lighting"}});
    REQUIRE(sentence);
    CHECK(sentence->anchor == "deferred_lighting");
    CHECK_FALSE(insertionAnchorMissing({{"code", "x"}, {"message", "something else"}}));
    CHECK(insertionAnchorLine("sky_ambient", "deferred_lighting", "Deferred lighting") ==
          "sky_ambient goes after deferred_lighting, which this pipeline does not have. "
          "Place Deferred lighting first.");
    const Json fr2{{"insertion", Json::array({{{"pass", "sky_background"},
                                               {"anchor", "deferred_lighting"},
                                               {"position", "after"}}})}};
    CHECK(missingInsertionAnchors(fr2, {"render_fullscreen"}) == std::vector<std::string>{"deferred_lighting"});
    CHECK(missingInsertionAnchors(fr2, {"deferred_lighting"}).empty());
    // A canonical anchor is in every composed pipeline.
    CHECK(missingInsertionAnchors({{"insertion", Json::array({{{"pass", "x"}, {"anchor", "post_ldr"}, {"position", "after"}}})}}, {}).empty());

    Transport engine;
    RenderFeaturesWidget features{&engine.viewport};
    QStringList placed;
    features.setPlaceInsertionAnchorAction([&](const QString &anchor) {
        placed << anchor;
        return true;
    });
    features.show();
    auto &notice = required<QLabel>(features, "pelican.renderFeatures.notice");
    auto &place = required<QPushButton>(features, "pelican.renderFeatures.placeAnchor");
    const QString line = QStringLiteral(
        "sky_ambient goes after deferred_lighting, which this pipeline does not have. "
        "Place Deferred lighting first.");

    SECTION("an engine without FR-2: the composer's sentence, for the batch that added the targets "
            "with it; nothing is taken back, since nothing was written (UX-12d, decision 2)") {
        ux12bFeaturesRead(engine, RootA);
        ux12bPickCatalog(features);
        required<QPushButton>(features, "pelican.renderFeatures.add").click();
        ux12bDocumentRead(engine, Json::array(), Json::array());
        required<QPushButton>(features, "pelican.renderFeatures.addWithTargets").click();
        ux12bDocumentRead(engine, Json::array(), Json::array());
        (void)ux12bEdit(engine, "edit_batch",
                        ux12bRejected({{"code", "render_pipeline_preflight_failed"},
                                       {"message", "render feature pass insert anchor was not found: "
                                                   "deferred_lighting"}}));
        CHECK(notice.text().startsWith(line));
        CHECK_FALSE(engine.next("edit_graph_document", 200));
    }
    SECTION("an engine with FR-2: insertion is looked at before anything is sent") {
        ux12bFeaturesRead(engine, RootA,
                          {{"insertion", Json::array({{{"pass", "sky_background"},
                                                       {"anchor", "deferred_lighting"},
                                                       {"position", "after"}}})}});
        ux12bPickCatalog(features);
        required<QPushButton>(features, "pelican.renderFeatures.add").click();
        ux12bDocumentRead(engine, Json::array(), Json::array(),
                          Json::array({{{"name", "render_fullscreen"}}, {{"name", "present"}}}));
        CHECK(notice.text() == line);
        CHECK_FALSE(engine.next("edit_render_targets", 200));
    }
    REQUIRE(place.isVisibleTo(&features));
    CHECK(place.text() == QStringLiteral("Place Deferred lighting in the Graph"));
    place.click();
    CHECK(placed == QStringList{QStringLiteral("deferred_lighting")});
    CHECK_FALSE(place.isVisibleTo(&features));
}

TEST_CASE("UX-12b decision 3: a snapshot's after is picked from the places the pipeline offers, "
          "and an unknown one is refused with them",
          "[devstudio][ux12b][rendering-settings][widget]") {
    CHECK(snapshotAfterUnknownLine({{"code", "snapshot.after_unknown@1"},
                                    {"snapshot", "opaque_snapshot"},
                                    {"after", "opaque"},
                                    {"candidates", {"post_ldr", "present"}}}) ==
          std::optional<std::string>{"Snapshot opaque_snapshot cannot be taken after opaque: this "
                                     "pipeline takes it after post_ldr or present."});
    Transport engine;
    RenderingSettingsWidget settings{&engine.viewport};
    settings.show();
    QApplication::processEvents();
    settingsRead(engine, RootA, Json::object());
    auto context = engine.next("get_render_authoring_context");
    REQUIRE(context);
    engine.answer(*context, {{"snapshot_after_candidates", {"post_ldr", "present"}}});
    // +: after the first place the pipeline offers.
    required<QPushButton>(settings, "pelican.renderingSettings.snapshots.add").click();
    auto sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    const Json one = Json::array({{{"name", "opaque_snapshot"}, {"after", "post_ldr"}}});
    CHECK(sent->at("params") == Json{{"base_source_digest", RootA}, {"set", {{"snapshots", one}}}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-keys-ux12b-1"}});
    auto poll = engine.next("get_edit_result");
    REQUIRE(poll);
    engine.answer(*poll, liveCommit(RootB));
    settingsRead(engine, RootB, {{"snapshots", one}});
    QApplication::processEvents();
    auto &after = required<QComboBox>(settings, "pelican.renderingSettings.snapshots.row0.after");
    REQUIRE(after.count() == 2);
    CHECK(after.itemText(0) == QStringLiteral("After opaque (post_ldr)"));
    CHECK(after.itemData(1).toString() == QStringLiteral("present"));
    after.setCurrentIndex(1);
    sent = engine.next("edit_rendering_keys");
    REQUIRE(sent);
    CHECK(sent->at("params") ==
          Json{{"base_source_digest", RootB},
               {"set", {{"snapshots", Json::array({{{"name", "opaque_snapshot"}, {"after", "present"}}})}}}});
    engine.answer(*sent, {{"status", "accepted"}, {"ticket", "render-keys-ux12b-2"}});
    poll = engine.next("get_edit_result");
    REQUIRE(poll);
    engine.answer(*poll, ux12bRejected({{"code", "snapshot.after_unknown@1"},
                                        {"message", "unknown"},
                                        {"snapshot", "opaque_snapshot"},
                                        {"after", "present"},
                                        {"candidates", {"post_ldr"}}}));
    CHECK(required<QLabel>(settings, "pelican.renderingSettings.notice").text() ==
          QStringLiteral("Snapshot opaque_snapshot cannot be taken after present: this pipeline "
                         "takes it after post_ldr."));
}

TEST_CASE("UX-12b decision 4 / UX-12d decision 3: a target_ref the engine expects another type of "
          "offers Fix, which writes that type and the reading ports' and sends the refused change "
          "again, in one edit_batch",
          "[devstudio][ux12b][render-features][widget]") {
    auto array_depth = DeviceDepth;
    array_depth["arguments"]["topology"] = "two_d_array";
    const Json refusal{{"code", "graph.target_ref_type_mismatch@1"},
                       {"message", "shadow_map_ref declares two_d"},
                       {"node", "shadow_map_ref"},
                       {"name", "shadow_map"},
                       {"field", "topology"},
                       {"declared", "two_d"},
                       {"actual", "two_d_array"},
                       {"expected", {{"type", array_depth}, {"layers", 3}}}};
    const auto fix = targetRefTypeFix(refusal);
    REQUIRE(fix);
    CHECK(targetRefTypeFixLabel(*fix) == "Fix: make shadow_map_ref 2D array depth (3 layers)");

    Transport engine;
    RenderFeaturesWidget features{&engine.viewport};
    features.show();
    uxFeaturesRead(engine, RootA, Json::object());
    auto &list = required<QListWidget>(features, "pelican.renderFeatures.current");
    list.setCurrentRow(0);
    QApplication::processEvents();
    required<QPushButton>(features, "pelican.renderFeatures.parameters").click();
    QApplication::processEvents();
    auto *dialog = features.findChild<QDialog *>(QStringLiteral("pelican.featureParameters"));
    REQUIRE(dialog != nullptr);
    dialogChild<QDoubleSpinBox>(*dialog, "pelican.featureParameters.cascade_count").setValue(3);
    auto [parameters, poll] = uxSent(engine, RootA);
    CHECK(parameters == Json{{"cascade_count", 3}});
    engine.answer(poll, ux12bRejected(refusal));
    auto &button = required<QPushButton>(features, "pelican.renderFeatures.fixTargetRef");
    REQUIRE(button.isVisibleTo(&features));
    CHECK(button.text() == QStringLiteral("Fix: make shadow_map_ref 2D array depth (3 layers)"));

    button.click();
    const Json reference{{"id", "shadow_map_ref"},
                         {"op", "render.target_ref"},
                         {"parameters", {{"name", "shadow_map"}, {"type", DeviceDepth}}},
                         {"outputs", {"resource"}}};
    // UX-12d, decision 3: the lighting pass reads it through a port that
    // declares a 2D depth; another port, and a node of no declared ports,
    // are left as they are.
    const Json lighting{{"id", "deferred_lighting"},
                        {"op", "render.fullscreen"},
                        {"inputs",
                         {{"shadow_map", {{"node", "shadow_map_ref"}, {"port", "resource"}}},
                          {"albedo", {{"node", "gbuffer"}, {"port", "albedo"}}}}},
                        {"parameters",
                         {{"uses_light_data", true},
                          {"ports",
                           {{"shadow_map", {{"access", "read"}, {"type", DeviceDepth}}},
                            {"albedo", {{"access", "read"}, {"type", OpaqueImage}}}}}}}};
    const Json plain{{"id", "debug_view"},
                     {"op", "render.fullscreen"},
                     {"inputs", {{"image", {{"node", "shadow_map_ref"}, {"port", "resource"}}}}}};
    const auto graph = engine.next("get_graph_instances");
    REQUIRE(graph);
    engine.answer(*graph, {{"nodes", Json::array({reference, lighting, plain})}, {"definitions", Json::object()}});
    auto fixed = reference;
    fixed["parameters"]["type"] = array_depth;
    auto lit = lighting.at("parameters");
    lit["ports"]["shadow_map"]["type"] = array_depth;
    // One edit_batch: the refused change first, then the type and the port
    // in the same graph edit -- in two graph edits, or alone, it is refused.
    const auto batch = ux12bEdit(engine, "edit_batch", liveCommit(RootB));
    REQUIRE(batch.at("edits").size() == 2);
    CHECK(batch.at("base_source_digest") == RootA);
    const auto &again = batch.at("edits").at(0);
    CHECK(again.at("method") == "edit_render_features");
    CHECK(again.at("params").at("operations").at(0).at("op") == "set_parameters");
    CHECK(again.at("params").at("operations").at(0).at("parameters") == Json{{"cascade_count", 3}});
    CHECK(batch.at("edits").at(1) ==
          Json{{"method", "edit_graph_document"},
               {"params",
                {{"ops", Json::array({{{"op", "replace_node"}, {"scope", "root"}, {"node", fixed}},
                                      {{"op", "set_node_parameters"},
                                       {"scope", "root"},
                                       {"node", "deferred_lighting"},
                                       {"parameters", lit}}})}}}});
    CHECK(required<QLabel>(features, "pelican.renderFeatures.notice").text() ==
          QStringLiteral("shadow_map_ref takes the type the engine expects."));
}

TEST_CASE("UX-12b decision 5: material_route_unserved@1 is one sentence on what to do",
          "[devstudio][ux12b][rendering-settings][widget]") {
    Transport engine;
    RenderingSettingsWidget settings{&engine.viewport};
    settings.show();
    QApplication::processEvents();
    settingsRead(engine, RootA, Json::object());
    auto &route = required<QComboBox>(settings, "pelican.renderingSettings.material_route_default");
    route.setCurrentIndex(route.findData(QStringLiteral("forward")));
    auto sent = engine.next("set_material_route_default");
    REQUIRE(sent);
    engine.answer(*sent, ux12bRejected({{"code", "material_route_unserved@1"},
                                        {"message", "material_route_unserved@1: materials 0, 1"}}));
    CHECK(required<QLabel>(settings, "pelican.renderingSettings.notice").text() ==
          QStringLiteral("No forward material pass serves the route, so it stays on auto — add a "
                         "forward material pass first."));
}

// --- UX-12d (engine FR-3): a writer for a target no pass draws, edit_batch,
// and a target the feature draws that nothing reads.

const Json ColorSignal{{"semantic", "pelican.render.color_signal@1"},
                       {"arguments", {{"range", "extended"}, {"reference", "scene"}, {"transfer", "linear"}}}};

// The Blank-built deferred Graph (FR-3's fixture, cut down): the material
// pass draws its depth and albedo into render.images of its own; the
// lighting pass reads the albedo through the pass's port and draws its colour
// into an image of its own.
Json ux12dNodes(const Json &extra = Json::array()) {
    auto nodes = Json::array(
        {{{"id", "render_material_pass__albedo"}, {"op", "render.image"}, {"parameters", {{"type", ColorSignal}}}},
         {{"id", "render_material_pass__depth"}, {"op", "render.image"}, {"parameters", {{"type", DeviceDepth}}}},
         {{"id", "render_material_pass"},
          {"op", "render.material_pass"},
          {"inputs",
           {{"albedo", {{"node", "render_material_pass__albedo"}, {"port", "resource"}}},
            {"depth", {{"node", "render_material_pass__depth"}, {"port", "resource"}}}}}},
         {{"id", "deferred_lighting__color"}, {"op", "render.image"}, {"parameters", {{"type", ColorSignal}}}},
         {{"id", "deferred_lighting"},
          {"op", "render.fullscreen"},
          {"inputs",
           {{"albedo", {{"node", "render_material_pass"}, {"port", "albedo"}}},
            {"color", {{"node", "deferred_lighting__color"}, {"port", "resource"}}}}},
          {"parameters",
           {{"ports",
             {{"albedo", {{"access", "read"}, {"type", ColorSignal}}},
              {"color", {{"access", "write"}, {"attachment", "color"}, {"type", ColorSignal}}}}}}}}});
    for (const auto &node : extra) nodes.push_back(node);
    return nodes;
}

// sky_ambient as FR-3's list_render_features gives it.
Json ux12dSky() {
    return {{"required_targets",
             Json::array({{{"name", "lit_color"}, {"type", OpaqueImage}, {"needs_writer", false}},
                          {{"name", "scene_depth"},
                           {"type", DeviceDepth},
                           {"usage", {"SAMPLED"}},
                           {"needs_writer", true}}})}};
}

TEST_CASE("UX-12d decision 1: a target no pass would draw is asked about with the output that "
          "draws it, connected in the same edit; a refusal that names writers offers Write from",
          "[devstudio][ux12d][render-features][widget]") {
    // The model: the material pass's depth is the one depth output of the
    // Graph; the lighting pass's albedo is read through a pass's port.
    const auto writers = graphWriterCandidates(DeviceDepth, ux12dNodes());
    REQUIRE(writers.size() == 1);
    CHECK(writerCandidateLabel(writers[0]) == "render_material_pass.depth");
    CHECK(writers[0].resource == "render_material_pass__depth");
    const auto colours = graphWriterCandidates(OpaqueImage, ux12dNodes());
    REQUIRE(colours.size() == 2);
    CHECK(writerCandidateLabel(colours[1]) == "deferred_lighting.color");
    const Json refusal{{"code", "feature.required_target_unwritten@1"},
                       {"message", "sky_ambient reads scene_depth, which no pass writes"},
                       {"feature", "sky_ambient"},
                       {"target", "scene_depth"},
                       {"type", DeviceDepth},
                       {"writer_candidates",
                        Json::array({{{"node", "render_material_pass"},
                                      {"pass", "render_material_pass"},
                                      {"port", "depth"},
                                      {"resource", "render_material_pass__depth"}},
                                     {{"node", "prepass"}, {"pass", "prepass"}, {"port", "depth"}, {"resource", "pre"}},
                                     {{"pass", "opaque"}, {"port", "depth"}, {"resource", "opaque_depth"}}})},
                       {"document", SkyFeature}};
    const auto unwritten = requiredTargetUnwritten(refusal);
    REQUIRE(unwritten);
    REQUIRE(unwritten->candidates.size() == 3);
    CHECK(writerCandidateLabel(unwritten->candidates[2]) == "opaque.depth");
    CHECK(requiredTargetUnwrittenLine(*unwritten) ==
          "sky_ambient reads scene_depth, which no pass writes before it. Write it from one of the "
          "passes below.");

    Transport engine;
    RenderFeaturesWidget features{&engine.viewport};
    features.show();
    ux12bFeaturesRead(engine, RootA, ux12dSky());
    auto &notice = required<QLabel>(features, "pelican.renderFeatures.notice");
    auto &with_targets = required<QPushButton>(features, "pelican.renderFeatures.addWithTargets");
    auto &writer = required<QComboBox>(features, "pelican.renderFeatures.writer");
    auto &write_from = required<QPushButton>(features, "pelican.renderFeatures.writeFrom");

    // Add: neither target is declared, and scene_depth needs a writer.
    ux12bPickCatalog(features);
    required<QPushButton>(features, "pelican.renderFeatures.add").click();
    ux12bDocumentRead(engine, Json::array(), ux12dNodes());
    CHECK(notice.text() == QStringLiteral("sky_ambient needs lit_color (color) and scene_depth (depth). "
                                          "Add them with the feature, and write scene_depth from "
                                          "render_material_pass.depth?"));
    CHECK(with_targets.isVisibleTo(&features));
    CHECK_FALSE(writer.isVisibleTo(&features)); // one output: nothing to pick

    // Add with targets: the references and the writer's connection are one
    // graph edit of the batch.
    with_targets.click();
    ux12bDocumentRead(engine, Json::array(), ux12dNodes());
    const auto batch = ux12bEdit(engine, "edit_batch", liveCommit(RootB));
    REQUIRE(batch.at("edits").size() == 3);
    CHECK(batch.at("edits").at(0).at("method") == "edit_render_targets");
    const auto &graph = batch.at("edits").at(1);
    CHECK(graph.at("method") == "edit_graph_document");
    const auto &ops = graph.at("params").at("ops");
    REQUIRE(ops.size() == 3);
    CHECK(ops.at(0).at("node").at("id") == "lit_color_ref");
    CHECK(ops.at(1).at("node").at("id") == "scene_depth_ref");
    CHECK(ops.at(2) == Json{{"op", "set_input"},
                            {"node", "render_material_pass"},
                            {"port", "depth"},
                            {"source", {{"node", "scene_depth_ref"}, {"port", "resource"}}}});
    CHECK(batch.at("edits").at(2).at("method") == "edit_render_features");
    CHECK(notice.text() == QStringLiteral("Added sky_ambient with the render targets it needs; "
                                          "render_material_pass.depth writes scene_depth."));
    ux12bFeaturesRead(engine, RootB, ux12dSky());
    ux12bDocumentRead(engine, Json::array(), ux12dNodes());

    // A plain Apply refused with the writers the engine names (a pass of
    // the rendering section is no graph port, and is not offered).
    ux12bPickCatalog(features);
    required<QPushButton>(features, "pelican.renderFeatures.add").click();
    ux12bDocumentRead(engine, Json::array({{{"name", "lit_color"}}, {{"name", "scene_depth"}}}), ux12dNodes());
    required<QPushButton>(features, "pelican.renderFeatures.apply").click();
    (void)ux12bEdit(engine, "edit_render_features", ux12bRejected(refusal));
    CHECK(notice.text().startsWith(QStringLiteral("sky_ambient reads scene_depth, which no pass writes")));
    REQUIRE(writer.isVisibleTo(&features));
    CHECK(writer.count() == 2);
    REQUIRE(write_from.isVisibleTo(&features));
    CHECK(write_from.text() == QStringLiteral("Write scene_depth from render_material_pass.depth"));
    writer.setCurrentIndex(1);
    CHECK(write_from.text() == QStringLiteral("Write scene_depth from prepass.depth"));
    write_from.click();
    const Json reference{{"id", "scene_depth_ref"},
                         {"op", "render.target_ref"},
                         {"parameters", {{"name", "scene_depth"}, {"type", DeviceDepth}}}};
    ux12bDocumentRead(engine, Json::array({{{"name", "lit_color"}}, {{"name", "scene_depth"}}}),
                      ux12dNodes(Json::array({reference})));
    const auto again = ux12bEdit(engine, "edit_batch", liveCommit(RootC));
    CHECK(again == Json{{"base_source_digest", RootB},
                        {"edits",
                         Json::array({{{"method", "edit_graph_document"},
                                       {"params",
                                        {{"ops", Json::array({{{"op", "set_input"},
                                                               {"node", "prepass"},
                                                               {"port", "depth"},
                                                               {"source",
                                                                {{"node", "scene_depth_ref"},
                                                                 {"port", "resource"}}}}})}}}},
                                      {{"method", "edit_render_features"},
                                       {"params",
                                        {{"operations",
                                          Json::array({{{"op", "add"}, {"feature", SkyFeature}}})}}}}})}});
}

TEST_CASE("UX-12d decision 2: a refused edit_batch names the edit it met (batch_index), and "
          "nothing is taken back",
          "[devstudio][ux12d][render-features][widget]") {
    CHECK(batchIndexOf({{"code", "x"}, {"batch_index", 1}}) == std::optional<std::size_t>{1});
    CHECK_FALSE(batchIndexOf({{"code", "x"}}));
    Transport engine;
    RenderFeaturesWidget features{&engine.viewport};
    features.show();
    ux12bFeaturesRead(engine, RootA);
    ux12bPickCatalog(features);
    required<QPushButton>(features, "pelican.renderFeatures.add").click();
    ux12bDocumentRead(engine, Json::array(), Json::array());
    required<QPushButton>(features, "pelican.renderFeatures.addWithTargets").click();
    ux12bDocumentRead(engine, Json::array(), Json::array());
    const auto sent = engine.next("edit_batch");
    REQUIRE(sent);
    CHECK(sent->at("params").at("edits").size() == 3);
    engine.answer(*sent, ux12bRejected({{"code", "graph.node_duplicate@1"},
                                        {"message", "graph node already exists"},
                                        {"node", "scene_depth_ref"},
                                        {"batch_index", 1}}));
    CHECK(required<QLabel>(features, "pelican.renderFeatures.notice").toolTip() ==
          QStringLiteral("sky_ambient was refused at the Graph's target references: graph node "
                         "already exists (graph.node_duplicate@1)"));
    // The engine wrote nothing, so nothing is sent to take it back.
    CHECK_FALSE(engine.next("edit_render_targets", 200));
    CHECK_FALSE(engine.next("edit_graph_document", 50));
    CHECK(required<QPushButton>(features, "pelican.renderFeatures.add").isEnabled());
}

TEST_CASE("UX-12d decision 4: a target the feature draws that nothing reads is offered to "
          "present, which reads it, and the pass the feature goes after draws into it first",
          "[devstudio][ux12d][render-features][widget]") {
    const Json present_declaration{{"type", "fullscreen"},
                                   {"input", {"deferred_lighting__color"}},
                                   {"output", {{"color", {"display"}}, {"depth", nullptr}}},
                                   {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
    const auto context = [&](const std::string &digest) {
        return Json{{"source_digest", {{"hex", digest}}},
                    {"config_kind", "direct"},
                    {"render_targets", Json::array({{{"name", "lit_color"}}, {{"name", "scene_depth"}}})},
                    {"graphs",
                     Json::array({{{"name", "main_render"},
                                   {"passes",
                                    Json::array({{{"name", "deferred_lighting"},
                                                  {"declaration",
                                                   {{"input", {"render_material_pass__albedo"}},
                                                    {"output", {{"color", {"deferred_lighting__color"}}}}}}},
                                                 {{"name", "sky_background"},
                                                  {"declaration",
                                                   {{"input", {"scene_depth"}},
                                                    {"output", {{"color", {"lit_color"}}, {"depth", nullptr}}}}},
                                                  {"provenance",
                                                   {{"source", "feature"},
                                                    {"provider_feature", "sky_ambient"},
                                                    {"provider_reference", SkyFeature}}}},
                                                 {{"name", "present"},
                                                  {"declaration", present_declaration},
                                                  {"editable_root_pass", true},
                                                  {"root_declaration", present_declaration}}})}}})}};
    };
    const Json lit_ref{{"id", "lit_color_ref"},
                       {"op", "render.target_ref"},
                       {"parameters", {{"name", "lit_color"}, {"type", OpaqueImage}}}};
    auto sky = ux12dSky();
    sky["insertion"] = Json::array({{{"pass", "sky_background"}, {"anchor", "deferred_lighting"}, {"position", "after"}}});
    sky["name"] = "sky_ambient";
    const auto plan = showTargetPlan(sky, SkyFeature, context(RootB), ux12dNodes(Json::array({lit_ref})));
    REQUIRE(plan);
    CHECK(plan->target == "lit_color");
    CHECK(plan->present == "present");
    CHECK(plan->present_inputs == std::vector<std::string>{"lit_color"});
    // The lighting pass's colour port declares a colour signal: the
    // target_ref (the catalog's untyped image) takes that type first.
    const Json writes = Json::array({{{"op", "set_node_parameters"},
                                      {"scope", "root"},
                                      {"node", "lit_color_ref"},
                                      {"parameters", {{"name", "lit_color"}, {"type", ColorSignal}}}},
                                     {{"op", "set_input"},
                                      {"node", "deferred_lighting"},
                                      {"port", "color"},
                                      {"source", {{"node", "lit_color_ref"}, {"port", "resource"}}}}});
    CHECK(plan->graph_ops == writes);
    CHECK(showTargetLine(*plan) == "sky_ambient draws into lit_color, which nothing reads. Connect it to "
                                   "present to see it.");
    // Read by present: nothing to offer.
    auto shown = context(RootB);
    shown["graphs"][0]["passes"][2]["declaration"]["input"] = Json::array({"lit_color"});
    CHECK_FALSE(showTargetPlan(sky, SkyFeature, shown, ux12dNodes(Json::array({lit_ref}))));

    Transport engine;
    RenderFeaturesWidget features{&engine.viewport};
    features.show();
    sky.erase("name");
    ux12bFeaturesRead(engine, RootA, sky);
    const auto read = [&](const std::string &digest) {
        const auto asked = engine.next("get_render_authoring_context");
        REQUIRE(asked);
        engine.answer(*asked, context(digest));
        const auto graph = engine.next("get_graph_instances");
        REQUIRE(graph);
        engine.answer(*graph, {{"nodes", ux12dNodes(Json::array({lit_ref}))}, {"definitions", Json::object()}});
    };
    // The targets are declared and the anchor is there: Add asks nothing, and
    // Apply adds it.
    ux12bPickCatalog(features);
    required<QPushButton>(features, "pelican.renderFeatures.add").click();
    read(RootA);
    required<QPushButton>(features, "pelican.renderFeatures.apply").click();
    (void)ux12bEdit(engine, "edit_render_features", liveCommit(RootB));
    ux12bFeaturesRead(engine, RootB, sky);
    read(RootB);
    auto &notice = required<QLabel>(features, "pelican.renderFeatures.notice");
    CHECK(notice.toolTip().endsWith(QStringLiteral(
        "sky_ambient draws into lit_color, which nothing reads. Connect it to present to see it.")));
    auto &show = required<QPushButton>(features, "pelican.renderFeatures.showTarget");
    REQUIRE(show.isVisibleTo(&features));
    CHECK(show.text() == QStringLiteral("Connect lit_color to present"));

    // The press: the anchor's colour into the target (alone it composes),
    // then present's inputs (the root's pass) on the revision that wrote.
    show.click();
    read(RootB);
    const std::string RootD(64, 'd');
    const auto writer = ux12bEdit(engine, "edit_graph_document", liveCommit(RootC));
    CHECK(writer == Json{{"base_source_digest", RootB}, {"ops", writes}});
    const auto present = ux12bEdit(engine, "edit_preset_pass", liveCommit(RootD));
    CHECK(present.at("base_source_digest") == RootC);
    CHECK(present.at("graph") == "main_render");
    CHECK(present.at("pass_name") == "present");
    CHECK(present.at("input") == Json::array({"lit_color"}));
    CHECK(present.at("shader").at("fragment") == "engine://passthrough");
    CHECK(notice.text() == QStringLiteral("present shows lit_color."));
}

} // namespace PelicanStudio
