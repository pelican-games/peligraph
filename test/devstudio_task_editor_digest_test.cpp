// Q-15: the Task Editor (Render features' Compute and ray tracing tasks) on
// real Qt widgets, over the real RPC transport. A child process
// (process_fixture_child rpc-hold) takes every request the editor sends and
// writes it back on stderr; this test answers in the engine's place, through
// the viewport's own reply signal.
//
// What is judged: the digest the editor reads is told to the other panels,
// and another panel's commit (Q-13's shared digest) makes the editor read the
// document again at once, keeping the draft, so its next edit goes out
// against the revision the engine holds.

#include "renderconfigdigest.hpp"
#include "renderfeatureswidget.hpp"

// Everything embeddedviewport.hpp includes, first, so that only its own class
// is opened up. EmbeddedViewport has no test seam; as in the Inspector widget
// test, this translation unit reaches its process and readiness members so
// the editor runs over the viewport's real transport.
#include "engineattach.hpp"
#include "enginefailuremodel.hpp"
#include "engineprocess.hpp"
#include "nativewindowhost.hpp"
#include "viewportgeometry.hpp"

#include <QApplication>
#include <QAction>
#include <QListWidget>
#include <QMessageBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTest>
#include <QWidget>

#define private public
#include "embeddedviewport.hpp"
#undef private

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
    static char application_name[] = "pelican_task_editor_digest_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

const std::string RootA(64, 'a');
const std::string RootB(64, 'b');
const std::string RootC(64, 'c');
const std::string TaskDigest(64, 'd');

Json blurDeclaration(const std::string &shader) {
    return {{"name", "blur"},
            {"shader", shader},
            {"dispatch", {{"groups", Json::array({1, 1, 1})}}}};
}

// get_compute_task_authoring at `root`: one task a feature file declares.
Json taskContext(const std::string &root) {
    const auto declaration = blurDeclaration("project://shaders/blur");
    return {{"source_digest", {{"hex", root}}},
            {"tasks",
             Json::array({{{"name", "blur"},
                           {"kind", "compute"},
                           {"origin", "feature"},
                           {"editable", true},
                           {"source_reference", "project://features/blur.json"},
                           {"provider_feature", "blur"},
                           {"declaration", declaration},
                           {"declaration_json", declaration.dump(2)},
                           {"source_digest", {{"hex", TaskDigest}}}}})},
            {"resources", Json::array()}};
}

struct Rig {
    // First: every widget below needs the application.
    QApplication &app = application();
    EmbeddedViewport viewport;
    std::shared_ptr<RenderConfigDigest> digest =
        std::make_shared<RenderConfigDigest>();
    std::unique_ptr<RenderFeaturesWidget> features;
    std::deque<Json> pending;
    QString buffer;

    Rig() {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}},
            &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        viewport.rpc_ready_ = true;
        features = std::make_unique<RenderFeaturesWidget>(&viewport);
        features->setRenderConfigDigest(digest);
    }

    ~Rig() { features.reset(); }

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

    // The next request of `method` the panels send within `milliseconds`;
    // requests of other methods (Render features' own reads) stay unanswered.
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

    QDialog *editor() const {
        return features->findChild<QDialog *>(QStringLiteral("pelican.computeTasks"));
    }
};

template <typename Widget> Widget &required(QWidget &parent, const char *name) {
    auto *found = parent.findChild<Widget *>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("Q-15 the Task Editor reads the document again when another panel "
          "commits, keeping its draft",
          "[devstudio][task-editor][q15][q13][widget][negative-contrast]") {
    Rig rig;
    rig.features->editComputeTask(QStringLiteral("blur"));
    const auto first = rig.next("get_compute_task_authoring");
    REQUIRE(first);
    rig.answer(*first, taskContext(RootA));
    QDialog *dialog = rig.editor();
    REQUIRE(dialog != nullptr);
    // What the editor read, the other panels are told.
    CHECK(rig.digest->current() == RootA);

    auto &editor = required<QPlainTextEdit>(*dialog, "pelican.computeTasks.declaration");
    auto &apply = required<QPushButton>(*dialog, "pelican.computeTasks.apply");
    REQUIRE(editor.toPlainText().contains(QStringLiteral("project://shaders/blur")));
    const auto draft =
        QString::fromStdString(blurDeclaration("project://shaders/blur_wide").dump(2));
    editor.setPlainText(draft);

    // Control: nothing moved, nothing is read.
    CHECK_FALSE(rig.next("get_compute_task_authoring", 200));

    // Another panel's commit: read again at once, the draft kept.
    rig.digest->publish(RootB);
    const auto again = rig.next("get_compute_task_authoring");
    REQUIRE(again);
    rig.answer(*again, taskContext(RootB));
    CHECK(editor.toPlainText() == draft);

    // The edit goes out against the revision read, the task still guarded by
    // its own digest.
    REQUIRE(apply.isEnabled());
    apply.click();
    const auto edit = rig.next("edit_compute_task");
    REQUIRE(edit);
    const auto &params = edit->at("params");
    CHECK(params.at("base_source_digest") == RootB);
    CHECK(params.at("base_task_source_digest") == TaskDigest);
    CHECK(params.at("task").at("shader") == "project://shaders/blur_wide");

    // A move told while the edit is out is followed once it comes back.
    rig.digest->publish(RootC);
    CHECK_FALSE(rig.next("get_compute_task_authoring", 200));
    rig.answer(*edit, {{"status", "rejected"},
                       {"error",
                        {{"code", "gate_closed"},
                         {"message", "The editor gate is closed"}}}});
    const auto followed = rig.next("get_compute_task_authoring");
    REQUIRE(followed);
    rig.answer(*followed, taskContext(RootC));
    CHECK(editor.toPlainText() == draft);
}

TEST_CASE("LP-4a the Task Editor's Apply keeps a task's view_select, and a "
          "view_select refusal reads as its sentence",
          "[devstudio][task-editor][lp4a][view-select][widget]") {
    Rig rig;
    rig.features->editComputeTask(QStringLiteral("blur"));
    const auto read = rig.next("get_compute_task_authoring");
    REQUIRE(read);
    // The task runs for cascade 1 of the directional shadow.
    auto context = taskContext(RootA);
    auto &task = context["tasks"][0];
    task["declaration"]["view_family"] = "$shadow/directional";
    task["declaration"]["view_select"] = {{"cascade", 1}};
    task["declaration_json"] = task.at("declaration").dump(2);
    rig.answer(*read, context);
    QDialog *dialog = rig.editor();
    REQUIRE(dialog != nullptr);

    // Everything but the name may change; here the dispatch, through the form.
    required<QLineEdit>(*dialog, "pelican.computeTasks.dispatchAxis0")
        .setText(QStringLiteral("4"));
    required<QPushButton>(*dialog, "pelican.computeTasks.updateDispatch").click();
    auto &apply = required<QPushButton>(*dialog, "pelican.computeTasks.apply");
    REQUIRE(apply.isEnabled());
    apply.click();
    const auto edit = rig.next("edit_compute_task");
    REQUIRE(edit);
    const auto &sent = edit->at("params").at("task");
    INFO(sent.dump());
    CHECK(sent.at("dispatch").at("groups") == Json::array({4, 1, 1}));
    CHECK(sent.at("view_select") == Json{{"cascade", 1}});
    CHECK(sent.at("view_family") == "$shadow/directional");

    // The engine refuses the candidate for view_select.
    rig.answer(*edit, {{"status", "accepted"}, {"ticket", "lp4a-task"}});
    const auto result = rig.next("get_edit_result");
    REQUIRE(result);
    const Json fields{{"pass", "blur"},
                      {"family", "$shadow/directional"},
                      {"axis", "light"}};
    Json error = fields;
    error["code"] = "graph.view_select_runtime_axis@1";
    error["message"] = "graph.view_select_runtime_axis@1: " + fields.dump();
    rig.answer(*result, {{"status", "rejected"},
                         {"committed", false},
                         {"error", error}});
    CHECK(required<QLabel>(*dialog, "pelican.computeTasks.notice").text() ==
          QStringLiteral("blur: light of $shadow/directional is decided each "
                         "frame and cannot be selected "
                         "(graph.view_select_runtime_axis@1)."));
}

TEST_CASE("LP-4b the Task Editor's View field writes view_select into the "
          "declaration Apply sends",
          "[devstudio][task-editor][lp4b][view-select][widget]") {
    Rig rig;
    // The frame plan's view_axes for the task's node: a directional shadow.
    QStringList asked;
    std::vector<FramePlanViewAxis> axes{{"light", std::nullopt, true, {}},
                                        {"cascade", 3, false, {}}};
    rig.features->setTaskViewAxesProvider([&](const QString &task) {
        asked.push_back(task);
        return axes;
    });
    const auto open = [&](const Json &view_select) -> QDialog * {
        rig.features->editComputeTask(QStringLiteral("blur"));
        const auto read = rig.next("get_compute_task_authoring");
        REQUIRE(read);
        auto context = taskContext(RootA);
        auto &task = context["tasks"][0];
        task["declaration"]["view_family"] = "$shadow/directional";
        if (!view_select.is_null())
            task["declaration"]["view_select"] = view_select;
        task["declaration_json"] = task.at("declaration").dump(2);
        rig.answer(*read, context);
        return rig.editor();
    };
    const auto apply = [&](QDialog &dialog) {
        auto &button = required<QPushButton>(dialog, "pelican.computeTasks.apply");
        REQUIRE(button.isEnabled());
        button.click();
        const auto edit = rig.next("edit_compute_task");
        REQUIRE(edit);
        return edit->at("params").at("task");
    };

    SECTION("cascade 1 goes out in the declaration") {
        QDialog *dialog = open(nullptr);
        REQUIRE(dialog != nullptr);
        CHECK(asked.contains(QStringLiteral("blur")));
        CHECK(required<QLabel>(*dialog, "pelican.computeTasks.view.light").text() ==
              QStringLiteral("light: every view, decided each frame"));
        auto &cascade = required<QComboBox>(*dialog, "pelican.computeTasks.view.cascade");
        CHECK(cascade.currentText() == QStringLiteral("All views"));
        CHECK(cascade.count() == 4);
        cascade.setCurrentIndex(cascade.findText(QStringLiteral("1")));
        // The draft says so before Apply.
        const auto draft = Json::parse(
            required<QPlainTextEdit>(*dialog, "pelican.computeTasks.declaration")
                .toPlainText()
                .toStdString());
        CHECK(draft.at("view_select") == Json{{"cascade", 1}});
        const auto sent = apply(*dialog);
        INFO(sent.dump());
        CHECK(sent.at("view_select") == Json{{"cascade", 1}});
        CHECK(sent.at("view_family") == "$shadow/directional");
        CHECK(sent.at("dispatch") == Json{{"groups", Json::array({1, 1, 1})}});
    }

    SECTION("back to All views: the declaration goes out without view_select") {
        QDialog *dialog = open(Json{{"cascade", 1}});
        REQUIRE(dialog != nullptr);
        auto &cascade = required<QComboBox>(*dialog, "pelican.computeTasks.view.cascade");
        CHECK(cascade.currentText() == QStringLiteral("1"));
        cascade.setCurrentIndex(0);
        const auto sent = apply(*dialog);
        INFO(sent.dump());
        CHECK_FALSE(sent.contains("view_select"));
        CHECK(sent.at("view_family") == "$shadow/directional");
    }

    SECTION("the field left alone: view_select goes back as declared") {
        QDialog *dialog = open(Json{{"cascade", 2}});
        REQUIRE(dialog != nullptr);
        const auto sent = apply(*dialog);
        CHECK(sent.at("view_select") == Json{{"cascade", 2}});
    }

    SECTION("retyping the JSON moves the field with it") {
        QDialog *dialog = open(nullptr);
        REQUIRE(dialog != nullptr);
        auto &editor =
            required<QPlainTextEdit>(*dialog, "pelican.computeTasks.declaration");
        auto task = Json::parse(editor.toPlainText().toStdString());
        task["view_select"] = {{"cascade", 2}};
        editor.setPlainText(QString::fromStdString(task.dump(2)));
        QApplication::processEvents();
        CHECK(required<QComboBox>(*dialog, "pelican.computeTasks.view.cascade")
                  .currentText() == QStringLiteral("2"));
    }

    SECTION("a family without axes: no field") {
        axes.clear();
        QDialog *dialog = open(nullptr);
        REQUIRE(dialog != nullptr);
        CHECK(required<QWidget>(*dialog, "pelican.computeTasks.view").isHidden());
        CHECK(dialog->findChild<QComboBox *>(
                  QStringLiteral("pelican.computeTasks.view.cascade")) == nullptr);
    }
}


// --- LP-4f2: Render features' "Replace with loop version...", on the real
// panel over the real transport (the test answers for the engine).

namespace {

const std::string PresetDigest(64, 'p');
const std::string BloomTemplate = "engine://graphs/bloom_loop.json";
const std::string CascadeTemplate = "engine://graphs/shadow_cascade_loop.json";
const std::string BloomFeature = "engine://features/bloom.json";
const std::string ShadowFeature = "engine://features/shadow_directional.json";
const std::string SsaoFeature = "engine://features/ssao.json";

Json lp4f2Instance(const std::string &reference, const std::string &scope, int index,
                   const std::string &document, const std::string &digest) {
    return {{"index", index},
            {"reference", reference},
            {"parameters", Json::object()},
            {"parameter_schema", nullptr},
            {"editable", false},
            {"removable", scope == "root"},
            {"scope", scope},
            {"source_reference", document},
            {"source_digest", {{"hex", digest}}}};
}

// get_graph_templates, cut to what the panel reads: bloom (removes its
// feature) and the cascade (replaces a pass of the directional shadow).
Json lp4f2Templates() {
    return {{"templates",
             Json::array(
                 {{{"reference", BloomTemplate},
                   {"title", "Bloom loop"},
                   {"decomposes",
                    {{"feature", BloomFeature},
                     {"remove_feature", true},
                     {"shader_defines", Json::array({"PELICAN_FEATURE_BLOOM_KNEE=0.7"})}}},
                   {"document",
                    {{"definitions", {{"bloom_down_step", Json::object()}}},
                     {"nodes", Json::array({{{"id", "bloom_lit"}, {"op", "render.target_ref"}},
                                            {{"id", "bloom_down"}, {"op", "graph.loop"}}})}}}},
                  {{"reference", CascadeTemplate},
                   {"title", "Shadow cascade loop"},
                   {"decomposes",
                    {{"feature", ShadowFeature},
                     {"pass_replacements", {{"shadow_depth", "shadow_cascades"}}}}},
                   {"document",
                    {{"definitions", {{"shadow_cascade", Json::object()}}},
                     {"nodes", Json::array({{{"id", "shadow_cascades"},
                                             {"op", "graph.loop"}}})}}}}})}};
}

// One refresh of the panel, answered: it can edit, the root lists `root`
// features and the preset (`preset_document`, unless empty) lists `preset`.
void lp4f2Refresh(Rig &rig, const std::vector<std::string> &root,
                  const std::vector<std::string> &preset = {},
                  const std::string &preset_document = {}) {
    const auto can_edit = rig.next("can_edit");
    REQUIRE(can_edit);
    rig.answer(*can_edit, {{"can_edit", true}});
    const auto current = rig.next("get_render_features");
    REQUIRE(current);
    auto instances = Json::array();
    for (std::size_t index = 0; index < root.size(); ++index)
        instances.push_back(lp4f2Instance(root[index], "root", static_cast<int>(index),
                                          "passes/main.json", RootA));
    for (std::size_t index = 0; index < preset.size(); ++index)
        instances.push_back(lp4f2Instance(preset[index], "preset", static_cast<int>(index),
                                          preset_document, PresetDigest));
    rig.answer(*current, {{"source_reference", "passes/main.json"},
                          {"source_digest", {{"hex", RootA}}},
                          {"feature_instances", instances}});
    const auto catalog = rig.next("list_render_features");
    REQUIRE(catalog);
    rig.answer(*catalog,
               {{"features", Json::array({{{"name", "bloom"}, {"reference", BloomFeature}, {"available", true}},
                                          {{"name", "shadow_directional"}, {"reference", ShadowFeature}, {"available", true}},
                                          {{"name", "ssao"}, {"reference", SsaoFeature}, {"available", true}}})}});
    const auto templates = rig.next("get_graph_templates");
    REQUIRE(templates);
    rig.answer(*templates, lp4f2Templates());
}

QListWidget &lp4f2List(Rig &rig) {
    return required<QListWidget>(*rig.features, "pelican.renderFeatures.current");
}

void lp4f2Pick(Rig &rig, const QString &text) {
    auto &list = lp4f2List(rig);
    const auto found = list.findItems(text, Qt::MatchExactly);
    REQUIRE(found.size() == 1);
    list.setCurrentItem(found.front());
    QApplication::processEvents();
}

QMessageBox *lp4f2Prompt(Rig &rig) {
    return rig.features->findChild<QMessageBox *>(
        QStringLiteral("pelican.renderFeatures.replaceConfirm"));
}

} // namespace

TEST_CASE("LP-4f2 Replace with loop version is offered on a feature the engine ships one for, "
          "and on no other",
          "[devstudio][render-features][lp4f2][widget][negative-contrast]") {
    Rig rig;
    lp4f2Refresh(rig, {BloomFeature, SsaoFeature});
    auto &button = required<QPushButton>(*rig.features, "pelican.renderFeatures.replaceWithLoop");
    auto *action = rig.features->findChild<QAction *>(
        QStringLiteral("pelican.renderFeatures.replaceWithLoopVersion"));
    REQUIRE(action != nullptr);

    lp4f2Pick(rig, QStringLiteral("bloom"));
    CHECK_FALSE(button.isHidden());
    CHECK(button.isEnabled());
    CHECK(action->isEnabled());
    CHECK(rig.features->loopVersionOffered(QString::fromStdString(BloomFeature)) ==
          std::optional<QString>{QString{}});

    // ssao has no loop version: no button, and the Frame Plan is told none.
    lp4f2Pick(rig, QStringLiteral("ssao"));
    CHECK(button.isHidden());
    CHECK_FALSE(action->isEnabled());
    CHECK_FALSE(rig.features->loopVersionOffered(QString::fromStdString(SsaoFeature)).has_value());
    // Nor is a feature the project does not list.
    CHECK_FALSE(rig.features->loopVersionOffered(QString::fromStdString(ShadowFeature)).has_value());
    // Fired by name on it anyway: nothing asked, nothing sent.
    action->trigger();
    QApplication::processEvents();
    CHECK(lp4f2Prompt(rig) == nullptr);
    CHECK_FALSE(rig.next("apply_graph_template", 100));
}

TEST_CASE("LP-4f2 Replace with loop version confirms, sends the digests the feature lives "
          "under, and hands the loop it added to the Graph tab",
          "[devstudio][render-features][lp4f2][widget]") {
    const std::string how = GENERATE(std::string{"root"}, std::string{"preset"});
    CAPTURE(how);
    Rig rig;
    if (how == "root")
        lp4f2Refresh(rig, {BloomFeature});
    else
        lp4f2Refresh(rig, {}, {BloomFeature}, "project://passes/local-pipeline.json");
    QStringList loops;
    rig.features->setLoopVersionAppliedAction([&](const QString &loop) { loops << loop; });
    int commits = 0;
    QObject::connect(rig.features.get(), &RenderFeaturesWidget::renderConfigCommitted,
                     [&] { ++commits; });

    // The Frame Plan's way in: by the feature's reference.
    rig.features->replaceWithLoopVersion(QString::fromStdString(BloomFeature));
    QApplication::processEvents();
    auto *prompt = lp4f2Prompt(rig);
    REQUIRE(prompt != nullptr);
    // Nothing goes before the question is answered.
    CHECK_FALSE(rig.next("apply_graph_template", 100));
    CHECK(prompt->text().contains(QStringLiteral("Bloom loop")));
    CHECK(prompt->text().contains(QStringLiteral("The feature bloom is removed")));
    CHECK(prompt->text().contains(QStringLiteral("PELICAN_FEATURE_BLOOM_KNEE=0.7")));
    CHECK(prompt->text().contains(QStringLiteral("Undo takes it back")));
    required<QPushButton>(*prompt, "pelican.renderFeatures.replaceConfirm.ok").click();
    QApplication::processEvents();

    const auto sent = rig.next("apply_graph_template");
    REQUIRE(sent);
    Json expected{{"base_source_digest", RootA}, {"template", BloomTemplate}};
    if (how == "preset") expected["base_preset_digest"] = PresetDigest;
    CHECK(sent->at("params") == expected);

    // Accepted with what it applied, then committed.
    rig.answer(*sent, {{"status", "accepted"},
                       {"ticket", "gt-1"},
                       {"applied",
                        {{"template", BloomTemplate},
                         {"feature", BloomFeature},
                         {"nodes", Json::array({"bloom_lit", "bloom_down"})}}}});
    CHECK(loops.isEmpty());
    const auto poll = rig.next("get_edit_result");
    REQUIRE(poll);
    CHECK(poll->at("params") == Json{{"ticket", "gt-1"}});
    rig.answer(*poll, {{"status", "committed"},
                       {"committed", true},
                       {"source_digest", {{"hex", RootB}}},
                       {"documents", {{"passes/main.json", RootB}}}});
    CHECK(loops == QStringList{QStringLiteral("bloom_down")});
    CHECK(commits == 1);
    CHECK(rig.digest->current() == RootB);
    CHECK(required<QLabel>(*rig.features, "pelican.renderFeatures.notice").text() ==
          QStringLiteral("Replaced bloom with Bloom loop. Undo takes it back."));
    // The list is read again, and the line stays while it is.
    lp4f2Refresh(rig, {});
    CHECK(required<QLabel>(*rig.features, "pelican.renderFeatures.notice").text() ==
          QStringLiteral("Replaced bloom with Bloom loop. Undo takes it back."));
}

TEST_CASE("LP-4f2 Replace with loop version says why it cannot, before and after it is sent",
          "[devstudio][render-features][lp4f2][widget][refusal]") {
    SECTION("Cancel sends nothing") {
        Rig rig;
        lp4f2Refresh(rig, {BloomFeature});
        rig.features->replaceWithLoopVersion(QString::fromStdString(BloomFeature));
        QApplication::processEvents();
        auto *prompt = lp4f2Prompt(rig);
        REQUIRE(prompt != nullptr);
        required<QPushButton>(*prompt, "pelican.renderFeatures.replaceConfirm.cancel").click();
        QApplication::processEvents();
        CHECK_FALSE(rig.next("apply_graph_template", 100));
    }
    SECTION("an engine preset's feature: fork first, nothing sent") {
        Rig rig;
        lp4f2Refresh(rig, {}, {ShadowFeature}, "engine://render_pipelines/hybrid_v1.json");
        const auto reason = rig.features->loopVersionOffered(QString::fromStdString(ShadowFeature));
        REQUIRE(reason.has_value());
        CHECK(*reason == QStringLiteral(
                             "shadow_directional is a feature of the engine preset "
                             "engine://render_pipelines/hybrid_v1.json: fork the preset into the "
                             "project first (Features tab, Fork), then replace the feature there."));
        lp4f2Pick(rig, QStringLiteral("shadow_directional (preset)"));
        auto &button =
            required<QPushButton>(*rig.features, "pelican.renderFeatures.replaceWithLoop");
        CHECK_FALSE(button.isHidden());
        CHECK_FALSE(button.isEnabled());
        CHECK(button.toolTip() == *reason);
        rig.features->replaceWithLoopVersion(QString::fromStdString(ShadowFeature));
        QApplication::processEvents();
        CHECK(lp4f2Prompt(rig) == nullptr);
        CHECK_FALSE(rig.next("apply_graph_template", 100));
        CHECK(required<QLabel>(*rig.features, "pelican.renderFeatures.notice")
                  .toolTip()
                  .contains(QStringLiteral("fork the preset")));
    }
    SECTION("the engine's refusal reads as its sentence") {
        Rig rig;
        lp4f2Refresh(rig, {ShadowFeature});
        rig.features->replaceWithLoopVersion(QString::fromStdString(ShadowFeature));
        QApplication::processEvents();
        auto *prompt = lp4f2Prompt(rig);
        REQUIRE(prompt != nullptr);
        CHECK(prompt->text().contains(QStringLiteral("The feature shadow_directional stays")));
        required<QPushButton>(*prompt, "pelican.renderFeatures.replaceConfirm.ok").click();
        QApplication::processEvents();
        const auto sent = rig.next("apply_graph_template");
        REQUIRE(sent);
        CHECK(sent->at("params") ==
              Json{{"base_source_digest", RootA}, {"template", CascadeTemplate}});
        rig.answer(*sent, {{"status", "rejected"},
                           {"error",
                            {{"code", "graph.definition_duplicate@1"},
                             {"message", "graph.definition_duplicate@1 {\"definition\":\"shadow_cascade\"}"},
                             {"definition", "shadow_cascade"},
                             {"template", CascadeTemplate}}}});
        CHECK(required<QLabel>(*rig.features, "pelican.renderFeatures.notice").toolTip() ==
              QStringLiteral("The graph already has a definition named shadow_cascade, which the "
                             "loop version adds -- perhaps it is in already (Undo takes an "
                             "earlier one back), or rename that definition first "
                             "(graph.definition_duplicate@1)."));
        // Nothing stays pending: the row offers it again.
        lp4f2Pick(rig, QStringLiteral("shadow_directional"));
        CHECK(required<QPushButton>(*rig.features, "pelican.renderFeatures.replaceWithLoop")
                  .isEnabled());
    }
}
} // namespace PelicanStudio
