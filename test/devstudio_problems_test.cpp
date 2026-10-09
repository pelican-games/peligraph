// PB-1s: the Problems dock -- what is wrong with the project now, from the
// engine's list (engine PB-1's get_problems) and the Assets dock's contents
// check -- on the real widget, with the test answering for the engine
// through the dock's own hooks.

#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/materialsmodel.hpp"
#include "../src/devstudio/model/problemsmodel.hpp"
#include "problemsdock.hpp"
#include "renderfallbacknotice.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace PelicanStudio;
using Json = nlohmann::json;

namespace {

QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_problems_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

// The engine's problems as PB-1's test reads them.
Json syntaxProblem() {
    return Json{{"id", "00000000000000a1"},
                {"severity", "error"},
                {"source", "reload_rpc"},
                {"code", "render_document_syntax@1"},
                {"message", "render_document_syntax@1: project://passes/main_rendering_config.json is not "
                            "valid JSON at line 12, column 1: syntax error while parsing object"},
                {"fields",
                 {{"document", "project://passes/main_rendering_config.json"},
                  {"line", 12},
                  {"column", 1},
                  {"reason", "syntax error while parsing object"}}},
                {"document", "project://passes/main_rendering_config.json"},
                {"document_kind", "root"},
                {"line", 12},
                {"column", 1}};
}

Json nodeProblem(const char *source) {
    return Json{{"id", "00000000000000b2"},
                {"severity", "error"},
                {"source", source},
                {"code", "graph.port_field_missing@1"},
                {"message", "graph.port_field_missing@1 {\"document\":\"project://passes/main_rendering_config.json\","
                            "\"field\":\"element\",\"node\":\"sim\",\"port\":\"data\"}"},
                {"fields",
                 {{"document", "project://passes/main_rendering_config.json"},
                  {"node", "sim"},
                  {"port", "data"},
                  {"field", "element"}}},
                {"document", "project://passes/main_rendering_config.json"},
                {"document_kind", "root_graph"},
                {"node", "sim"},
                {"port", "data"}};
}

Json shaderProblem() {
    return Json{{"id", "00000000000000c3"},
                {"severity", "error"},
                {"source", "hot_reload"},
                {"code", "shader_reload_failed@1"},
                {"message", "shaders/tint.frag:5: error: 'colr' : undeclared identifier"},
                {"fields", {{"path", "C:/project/shaders/tint.frag"}, {"log", "..."}}},
                {"document", "project://shaders/tint.frag"},
                {"document_kind", "shader"},
                {"line", 5}};
}

Json routeProblem() {
    return Json{{"id", "00000000000000d4"},
                {"severity", "error"},
                {"source", "startup"},
                {"code", "material_route_unserved@1"},
                {"message", "material_route_unserved@1"},
                {"fields",
                 {{"unserved",
                   Json::array({{{"route", "deferred"},
                                 {"hint", "add a deferred geometry pass"},
                                 {"materials", Json::array({{{"name", "default"}, {"origin", "builtin"}}})}}})}}},
                {"document", "project://passes/main_rendering_config.json"},
                {"document_kind", "root_rendering"}};
}

// A warning: the engine lists none yet (PB-1 says error for everything it
// meets), so the test makes one up.
Json warningProblem() {
    return Json{{"id", "00000000000000e5"},
                {"severity", "warning"},
                {"source", "startup"},
                {"code", "material.name_unknown@1"},
                {"message", "material.name_unknown@1 {\"name\":\"brick\",\"reason\":\"not registered\"}"},
                {"fields", {{"name", "brick"}, {"reason", "not registered"}}},
                {"document", "project://materials/brick.material.json"},
                {"document_kind", "material"}};
}

Json statusAt(std::uint64_t revision, std::uint64_t count) {
    return Json{{"frame", 3}, {"problems", {{"revision", revision}, {"count", count}, {"errors", count}, {"warnings", 0}}}};
}

QByteArray bytes(const Json &json) { return QByteArray::fromStdString(json.dump()); }

struct Rig {
    struct Request {
        qint64 id = 0;
        QString method;
    };
    std::vector<Request> requests;
    qint64 next_id = 100;
    int status_reads = 0;
    std::vector<ProblemRow> opened;
    std::vector<QString> restarts;
    std::vector<std::pair<std::size_t, std::size_t>> counts;
    ProblemsDock dock;

    Rig()
        : dock{ProblemsDock::Hooks{
              .send = [this](const QString &method, const QJsonObject &, QString *) -> qint64 {
                  requests.push_back({++next_id, method});
                  return next_id;
              },
              .read_status = [this] { ++status_reads; },
              .open = [this](const ProblemRow &row) { opened.push_back(row); },
              .restart = [this](const QString &reason) { restarts.push_back(reason); },
              .counted = [this](std::size_t errors, std::size_t warnings) { counts.emplace_back(errors, warnings); },
          }} {
        dock.resize(900, 300);
        dock.show();
    }

    // The engine answers the last get_problems with `problems` at `revision`.
    void answer(std::uint64_t revision, Json problems) {
        REQUIRE_FALSE(requests.empty());
        REQUIRE(requests.back().method == QStringLiteral("get_problems"));
        REQUIRE(dock.receiveResult(requests.back().id,
                                   bytes(Json{{"revision", revision}, {"problems", std::move(problems)}})));
    }
    int problemReads() const {
        int reads = 0;
        for (const auto &request : requests) reads += request.method == QStringLiteral("get_problems") ? 1 : 0;
        return reads;
    }
    QTreeWidget *tree() const { return dock.list(); }
    QTreeWidgetItem *rowFor(const char *id) const {
        for (int index = 0; index < tree()->topLevelItemCount(); ++index)
            if (tree()->topLevelItem(index)->data(0, ProblemIdRole).toString() == QLatin1String{id})
                return tree()->topLevelItem(index);
        return nullptr;
    }
    void click(QTreeWidgetItem *item) {
        REQUIRE(item != nullptr);
        tree()->scrollToItem(item);
        const QRect rect = tree()->visualItemRect(item);
        REQUIRE_FALSE(rect.isEmpty());
        QTest::mouseClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        CHECK(tree()->currentItem() == item);
    }
};

} // namespace

TEST_CASE("PB-1s decision 1: one row a problem -- the severity, the code's sentence, the document, the line and "
          "column and where it is fixed -- sorted by severity or document, narrowed by severity and by words",
          "[devstudio][problems][pb1s]") {
    application();
    Rig rig;
    rig.dock.engineAvailable();
    rig.dock.observeStatus(statusAt(1, 3));
    rig.answer(1, Json::array({warningProblem(), syntaxProblem(), shaderProblem()}));
    REQUIRE(rig.tree()->topLevelItemCount() == 3);

    auto *syntax = rig.rowFor("00000000000000a1");
    REQUIRE(syntax != nullptr);
    CHECK(syntax->text(0) == QStringLiteral("Error"));
    CHECK(syntax->text(1).toStdString() ==
          "passes/main_rendering_config.json is not valid JSON at line 12, column 1: syntax error while parsing "
          "object (render_document_syntax@1).");
    CHECK(syntax->text(2) == QStringLiteral("passes/main_rendering_config.json"));
    CHECK(syntax->text(3) == QStringLiteral("12:1"));
    CHECK(syntax->text(4) == QStringLiteral("Text"));
    auto *shader = rig.rowFor("00000000000000c3");
    REQUIRE(shader != nullptr);
    CHECK(shader->text(1).toStdString() ==
          "tint.frag does not compile: shaders/tint.frag:5: error: 'colr' : undeclared identifier "
          "(shader_reload_failed@1).");
    CHECK(shader->text(3) == QStringLiteral("5"));
    CHECK(shader->text(4) == QStringLiteral("Shader file"));
    auto *warning = rig.rowFor("00000000000000e5");
    REQUIRE(warning != nullptr);
    CHECK(warning->text(0) == QStringLiteral("Warning"));
    CHECK(warning->text(4) == QStringLiteral("Materials: brick"));

    // Errors first, then by document.
    CHECK(rig.tree()->topLevelItem(0)->text(2) == QStringLiteral("passes/main_rendering_config.json"));
    CHECK(rig.tree()->topLevelItem(1)->text(2) == QStringLiteral("shaders/tint.frag"));
    CHECK(rig.tree()->topLevelItem(2) == warning);
    // By document: the material's comes first.
    rig.tree()->sortByColumn(2, Qt::AscendingOrder);
    CHECK(rig.tree()->topLevelItem(0) == warning);

    rig.dock.severityFilter()->setCurrentIndex(2);
    CHECK(warning->isHidden() == false);
    CHECK(syntax->isHidden());
    rig.dock.severityFilter()->setCurrentIndex(1);
    CHECK(warning->isHidden());
    CHECK_FALSE(shader->isHidden());
    rig.dock.severityFilter()->setCurrentIndex(0);
    rig.dock.textFilter()->setText(QStringLiteral("TINT"));
    CHECK_FALSE(shader->isHidden());
    CHECK(syntax->isHidden());
    CHECK(warning->isHidden());
    rig.dock.textFilter()->clear();
    CHECK_FALSE(syntax->isHidden());
}

TEST_CASE("PB-1s decision 2: get_problems is read when get_status.problems.revision moves, and the contents "
          "check's warnings and errors are rows beside the engine's",
          "[devstudio][problems][pb1s]") {
    application();
    Rig rig;
    rig.dock.engineAvailable();
    // The first reading is asked for at once.
    CHECK(rig.status_reads == 1);
    // An engine before PB-1 says nothing: nothing is read.
    rig.dock.observeStatus(Json{{"frame", 1}});
    CHECK(rig.problemReads() == 0);
    rig.dock.observeStatus(statusAt(4, 1));
    CHECK(rig.problemReads() == 1);
    // A second reading while the first is in flight is one more read after it.
    rig.dock.observeStatus(statusAt(5, 1));
    CHECK(rig.problemReads() == 1);
    rig.answer(4, Json::array({nodeProblem("startup")}));
    CHECK(rig.problemReads() == 2);
    rig.answer(5, Json::array({nodeProblem("startup")}));
    // The same revision again reads nothing.
    rig.dock.observeStatus(statusAt(5, 1));
    rig.dock.observeStatus(statusAt(5, 1));
    CHECK(rig.problemReads() == 2);
    REQUIRE(rig.tree()->topLevelItemCount() == 1);

    // The contents check: a warning and an error are rows; an info is not.
    AssetStoreVerification store;
    store.name = "main";
    store.manifest = "C:/project/assets.manifest.json";
    store.issues.push_back({"extra", "warning", "textures/stray.ktx2", "not in the manifest",
                            "C:/project/assets/textures/stray.ktx2"});
    store.issues.push_back({"content_differs", "error", "textures/rock.ktx2", "sha256 differs",
                            "C:/project/assets/textures/rock.ktx2"});
    store.issues.push_back({"case_differs", "info", "textures/Leaf.ktx2", "", "C:/project/assets/textures/Leaf.ktx2"});
    rig.dock.setAssetIssues({store});
    REQUIRE(rig.tree()->topLevelItemCount() == 3);
    const auto &rows = rig.dock.session().rows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[1].origin == ProblemRow::Origin::Assets);
    CHECK(rows[1].place == ProblemRow::Place::AssetFile);
    CHECK(rows[1].place_text == "Assets: stray.ktx2");
    CHECK(rows[1].sentence.starts_with("Extra against assets.manifest.json"));
    CHECK(rig.dock.countLine()->text() == QStringLiteral("2 errors, 1 warning"));

    // No clock reads get_status while others do; it does when nobody does.
    const int reads = rig.status_reads;
    QTest::qWait(2 * ProblemsDock::StatusPollMs + 300);
    CHECK(rig.status_reads >= reads + 1);
    rig.dock.engineUnavailable();
    // The engine's rows go with it; the check's stay.
    CHECK(rig.tree()->topLevelItemCount() == 2);
}

TEST_CASE("PB-1s decision 3 / RR-1s decisions 2 and 3: a click on a row goes where it is fixed -- the Graph's "
          "node for a start's refusal and a reload's alike, the document's text at the line or at the node, the "
          "shader file, the asset's row, the material, the rendering settings; Save restores the document",
          "[devstudio][problems][pb1s][rr1s]") {
    application();
    Rig rig;
    rig.dock.engineAvailable();
    rig.dock.observeStatus(statusAt(1, 5));
    auto reloaded_node = nodeProblem("reload_rpc");
    reloaded_node["id"] = "00000000000000b9";
    rig.answer(1, Json::array({nodeProblem("startup"), reloaded_node, syntaxProblem(), shaderProblem(),
                               routeProblem(), warningProblem()}));
    const auto place = [&](const char *id) {
        rig.opened.clear();
        rig.click(rig.rowFor(id));
        REQUIRE(rig.opened.size() == 1);
        return rig.opened.back();
    };
    const auto start = place("00000000000000b2");
    CHECK(start.place == ProblemRow::Place::GraphNode);
    CHECK(start.node == "sim");
    CHECK(rig.rowFor("00000000000000b2")->text(4) == QStringLiteral("Graph: sim"));
    // RR-1s, decision 3 (engine RR-1): after a refused reload the Graph reads
    // the version on disk, so a reload's node is fixed there too.
    const auto reload = place("00000000000000b9");
    CHECK(reload.place == ProblemRow::Place::GraphNode);
    CHECK(reload.node == "sim");
    CHECK(rig.rowFor("00000000000000b9")->text(4) == QStringLiteral("Graph: sim"));
    const auto syntax = place("00000000000000a1");
    CHECK(syntax.place == ProblemRow::Place::DocumentText);
    CHECK(syntax.document == "passes/main_rendering_config.json");
    CHECK(syntax.line == 12);
    CHECK(syntax.column == 1);
    CHECK(place("00000000000000c3").place == ProblemRow::Place::ShaderFile);
    const auto route = place("00000000000000d4");
    CHECK(route.place == ProblemRow::Place::RenderingSettings);
    CHECK(route.sentence.starts_with("No pass draws the deferred route, which default (engine built-in) takes"));
    const auto material = place("00000000000000e5");
    CHECK(material.place == ProblemRow::Place::Material);
    CHECK(material.material == "brick");
    // The detail line says where, for the row the click selected.
    rig.click(rig.rowFor("00000000000000a1"));
    CHECK(rig.dock.detailLine()->text().startsWith(QStringLiteral("passes/main_rendering_config.json:12:1 \u2014 ")));

    // The text it goes to: Show the document, at the line and column -- or,
    // without a line, at the node's id.
    std::vector<std::pair<std::string, Json>> sent;
    RenderFallbackNotice notice{RenderFallbackNotice::Hooks{
        .request = [&](const std::string &method, const Json &params, std::string *) -> std::int64_t {
            sent.emplace_back(method, params);
            return static_cast<std::int64_t>(sent.size()) + 900;
        },
        .schedule = [](int, std::function<void()>) {},
    }};
    const std::string content = "{\n  \"nodes\": [\n    {\"id\": \"sim\", \"op\": \"render.compute\"}\n  ]\n}\n";
    const auto open = [&](std::int64_t line, std::int64_t column, const QString &find) {
        sent.clear();
        notice.showDocumentAt(QStringLiteral("passes/main_rendering_config.json"), line, column, find);
        REQUIRE(sent.size() == 1);
        CHECK(sent.back().first == "get_render_documents");
        REQUIRE(notice.receiveResult(
            901, bytes(Json{{"documents", Json::array({{{"reference", "project://passes/main_rendering_config.json"},
                                                        {"digest", "d1"},
                                                        {"content", content}}})}})));
        REQUIRE(notice.documentDialog() != nullptr);
        auto *text = notice.documentDialog()->findChild<QPlainTextEdit *>(
            QStringLiteral("pelican.renderDocumentDialog.text"));
        REQUIRE(text != nullptr);
        return text->textCursor();
    };
    const auto at_line = open(3, 5, {});
    CHECK(at_line.blockNumber() == 2);
    CHECK(at_line.positionInBlock() == 4);
    const auto at_node = open(0, 0, QStringLiteral("\"sim\""));
    CHECK(at_node.blockNumber() == 2);
    CHECK(at_node.positionInBlock() == static_cast<int>(std::string{"    {\"id\": "}.size()));
    notice.documentDialog()->close();

    // RR-1s, decision 2: a refused reload's document too is restored through
    // the engine, against the digest get_render_documents lists (the engine's
    // base is the version on disk); Studio writes no file.
    (void)open(3, 5, {});
    auto *save = notice.documentDialog()->findChild<QPushButton *>(QStringLiteral("pelican.renderDocumentDialog.save"));
    REQUIRE(save != nullptr);
    REQUIRE(save->isEnabled());
    sent.clear();
    save->click();
    REQUIRE(sent.size() == 1);
    CHECK(sent.back().first == "restore_render_documents");
    CHECK(sent.back().second ==
          Json{{"documents", Json::array({{{"reference", "project://passes/main_rendering_config.json"},
                                           {"base_digest", "d1"},
                                           {"content", content}}})}});
    CHECK(notice.repair().state() == RenderDocumentRepair::State::Saving);
    CHECK(notice.documentDialog() != nullptr);
}

TEST_CASE("PB-1s decision 4: the count says how many errors and warnings, and the placeholder's row says See "
          "Problems",
          "[devstudio][problems][pb1s]") {
    application();
    CHECK(problemsCountLine(0, 0) == "No problems");
    CHECK(problemsCountLine(1, 0) == "1 error");
    CHECK(problemsCountLine(0, 2) == "2 warnings");
    CHECK(problemsCountLine(2, 1) == "2 errors, 1 warning");

    Rig rig;
    rig.dock.engineAvailable();
    rig.dock.observeStatus(statusAt(2, 2));
    rig.answer(2, Json::array({nodeProblem("startup"), warningProblem()}));
    REQUIRE_FALSE(rig.counts.empty());
    CHECK(rig.counts.back() == std::pair<std::size_t, std::size_t>{1, 1});
    CHECK(rig.dock.countLine()->text() == QStringLiteral("1 error, 1 warning"));

    RenderFallbackNotice notice{RenderFallbackNotice::Hooks{}};
    notice.setSeeProblems(true);
    notice.observeStatus(Json{
        {"render_pipeline",
         {{"state", "fallback"},
          {"root", "passes/main_rendering_config.json"},
          {"refusal",
           {{"code", "graph.port_field_missing@1"},
            {"node", "sim"},
            {"port", "data"},
            {"field", "element"},
            {"document", "project://passes/main_rendering_config.json"},
            {"message", "graph.port_field_missing@1"}}}}}});
    CHECK(notice.line()->text().toStdString() ==
          "Showing a placeholder: the rendering setup was refused \xE2\x80\x94 Port data of sim needs element "
          "(graph.port_field_missing@1) in passes/main_rendering_config.json. Fix it, then Reload from disk. See "
          "Problems.");
    // The Frame Plan's sentence (the session's) does not say it.
    CHECK(notice.session().notice().line.ends_with("Reload from disk."));
}

TEST_CASE("PB-1s decision 5: a row the engine no longer lists goes, and so does its selection",
          "[devstudio][problems][pb1s]") {
    application();
    Rig rig;
    rig.dock.engineAvailable();
    rig.dock.observeStatus(statusAt(7, 2));
    rig.answer(7, Json::array({syntaxProblem(), shaderProblem()}));
    rig.click(rig.rowFor("00000000000000a1"));
    REQUIRE(rig.tree()->currentItem() == rig.rowFor("00000000000000a1"));
    CHECK(rig.dock.detailLine()->isVisibleTo(&rig.dock));

    // Another revision, the other row still listed: the selection stays.
    rig.dock.observeStatus(statusAt(8, 2));
    rig.answer(8, Json::array({syntaxProblem(), shaderProblem()}));
    CHECK(rig.tree()->currentItem() == rig.rowFor("00000000000000a1"));

    // The document fixed and saved: its row goes with the selection.
    rig.dock.observeStatus(statusAt(9, 1));
    rig.answer(9, Json::array({shaderProblem()}));
    CHECK(rig.tree()->topLevelItemCount() == 1);
    CHECK(rig.rowFor("00000000000000a1") == nullptr);
    CHECK(rig.tree()->currentItem() == nullptr);
    CHECK(rig.tree()->selectedItems().isEmpty());
    CHECK_FALSE(rig.dock.detailLine()->isVisibleTo(&rig.dock));
    CHECK(rig.dock.countLine()->text() == QStringLiteral("1 error"));

    rig.dock.observeStatus(statusAt(10, 0));
    rig.answer(10, Json::array());
    CHECK(rig.tree()->topLevelItemCount() == 0);
    CHECK(rig.dock.countLine()->text() == QStringLiteral("No problems"));
}

TEST_CASE("RTL-3s decision 6: a shader record's and a payload's refusal read their fields -- the shader, what "
          "the layout wants -- and the row goes to the task's node with the cell's stage",
          "[devstudio][problems][rtl3s]") {
    // engine EB-3's fields, as the problem list carries them.
    const Json fields{{"task", "trace"},
                      {"ray_type", "shadow"},
                      {"group", "mesh"},
                      {"stage", "closesthit"},
                      {"shader", "project://shaders/wrong_record.rchit"},
                      {"expected", Json::array({{{"name", "material_index"}, {"offset", 0}, {"type", "u32"}},
                                                {{"name", "geometry_index"}, {"offset", 4}, {"type", "u32"}}})},
                      {"actual", Json::array({{{"offset", 0}, {"type", "u32"}}})}};
    // The message quotes another shader: the sentence is the fields'.
    const auto row = readEngineProblem(
        Json{{"id", "p1"}, {"severity", "error"}, {"source", "startup"},
             {"code", "pelican.ray_tracing.shader_record_abi_mismatch@1"},
             {"message", "pelican.ray_tracing.shader_record_abi_mismatch@1: shader 'other.rchit' entry 'main'"},
             // A reload's refusal of the root graph's task (engine EB-3).
             {"document_kind", "root_rendering"},
             {"node", "trace"},
             {"fields", fields}});
    REQUIRE(row.has_value());
    CHECK(row->sentence ==
          "Shader shaders/wrong_record.rchit declares a shader record that is not the ray group layout's: declare its "
          "fields in order (material_index u32, geometry_index u32), or take the shaderRecordEXT declaration out "
          "(ray type shadow, hit group mesh, stage closesthit) (pelican.ray_tracing.shader_record_abi_mismatch@1).");
    CHECK(row->place == ProblemRow::Place::GraphNode);
    CHECK(row->node == "trace");
    // A layout without a record, and a place no record is written.
    auto empty = fields;
    empty["expected"] = Json::array();
    empty["code"] = "pelican.ray_tracing.shader_record_abi_mismatch@1";
    CHECK(rayTracingRefusalDescription(empty).starts_with(
        "Shader shaders/wrong_record.rchit declares a shader record and the ray group layout has none: take its "
        "shaderRecordEXT declaration out"));
    auto nowhere = Json{{"code", "pelican.ray_tracing.shader_record_abi_mismatch@1"},
                        {"error_fields", {{"stage", "miss"}, {"miss", "sky"}, {"shader", "project://shaders/s.rmiss"},
                                          {"expected", nullptr}, {"actual", Json::array()}}}};
    CHECK(rayTracingRefusalDescription(nowhere) ==
          "Shader shaders/s.rmiss declares a shader record where none is written (its stage miss): take its "
          "shaderRecordEXT declaration out (miss sky) (pelican.ray_tracing.shader_record_abi_mismatch@1).");
    // The payload: the receiving shader and the caller.
    CHECK(rayTracingRefusalDescription(Json{{"code", "pelican.ray_tracing.payload_mismatch@1"},
                                            {"task", "trace"}, {"stage", "closesthit"},
                                            {"shader", "uvec.rchit"}, {"ray_type", "primary"},
                                            {"group", "foliage"}, {"expected", "vec4<f32>"},
                                            {"actual", "vec4<u32>"},
                                            {"caller", {{"stage", "raygen"}, {"shader", "eb3.rgen"}}}}) ==
          "Shader uvec.rchit takes the payload as vec4<u32>, and the ray it answers carries vec4<f32> (traced by "
          "eb3.rgen): make them one type (ray type primary, hit group foliage, stage closesthit) "
          "(pelican.ray_tracing.payload_mismatch@1).");
    // An engine before EB-3 (no expected): the table's sentence.
    CHECK(rayTracingRefusalDescription(Json{{"code", "pelican.ray_tracing.shader_record_abi_mismatch@1"},
                                            {"message", "pelican.ray_tracing.shader_record_abi_mismatch@1: shader 'a.rchit' entry 'main'"}})
              .starts_with("Shader a.rchit's shader record does not match"));
}

TEST_CASE("RTL-3s decision 8: a material its pass does not declare enough for goes to the pass -- the node "
          "of the root's graph, or the pass by name -- and the Materials list reads unusable",
          "[devstudio][problems][rtl3s]") {
    const Json fields{{"material", "QueryMaterial"},
                      {"material_document", "materials/query.material.json"},
                      {"pass", "forward_opaque"},
                      {"requirement", "ray_query"},
                      {"reason", "Raster pass uses the scene acceleration structure but does not declare ray_query"}};
    const auto problem = [&](const char *kind, const char *node) {
        Json value{{"id", std::string{"p-"} + kind}, {"severity", "error"}, {"source", "startup"},
                   {"code", "material.pass_contract_unmet@1"}, {"message", "material.pass_contract_unmet@1: ..."},
                   {"document", "project://material_pipeline.json"}, {"document_kind", kind}, {"fields", fields}};
        if (node != nullptr) value["node"] = node;
        return readEngineProblem(value).value();
    };
    const auto preset = problem("preset", nullptr);
    CHECK(preset.sentence ==
          "Material QueryMaterial queries the scene's acceleration structure, and pass forward_opaque, which draws "
          "it, does not declare ray_query: turn Ray query on for the pass (material.pass_contract_unmet@1).");
    // Not the Materials dock: the pass.
    CHECK(preset.place == ProblemRow::Place::GraphPass);
    CHECK(preset.pass == "forward_opaque");
    CHECK(preset.place_text == "Graph: forward_opaque");
    const auto graph_node = problem("root_graph", "material_pass");
    CHECK(graph_node.place == ProblemRow::Place::GraphNode);
    CHECK(graph_node.node == "material_pass");
    CHECK(problem("feature", nullptr).place == ProblemRow::Place::GraphPass);
    // list_materials' unusable entry.
    const auto list = parseMaterialList(Json{
        {"documents", Json::array({{{"path", "materials/query.material.json"}, {"status", "unusable"},
                                    {"reason", "r"}, {"pass", "forward_opaque"}, {"requirement", "ray_query"},
                                    {"materials", Json::array()}}})}});
    REQUIRE(list.documents.size() == 1);
    CHECK(list.documents[0].pass == "forward_opaque");
    CHECK(materialDocumentProblemLine(list.documents[0]) ==
          "materials/query.material.json: The material queries the scene's acceleration structure, and pass "
          "forward_opaque, which draws it, does not declare ray_query: turn Ray query on for the pass.");
}

TEST_CASE("AN-2 animation Problems open the affected object in the Inspector and clear on recovery",
          "[animation][problems][pb1]") {
    application();
    Rig rig;
    rig.dock.engineAvailable();
    rig.dock.observeStatus(statusAt(1, 1));
    const Json problem{{"id", "0000000000000a02"}, {"severity", "error"}, {"source", "ecs"},
                       {"code", "animation.component_invalid@1"},
                       {"message", "animation component clip 'character.glb#animation/Turn' is attached to a model without a glTF skin"},
                       {"document_kind", "scene"},
                       {"fields", {{"object", 42}, {"name", "Cube"}, {"scene_id", "default_scene"}}}};
    rig.answer(1, Json::array({problem}));
    auto *item = rig.rowFor("0000000000000a02");
    REQUIRE(item != nullptr);
    CHECK(item->text(4) == QStringLiteral("Inspector: Cube"));
    rig.click(item);
    REQUIRE(rig.opened.size() == 1);
    const auto &opened = rig.opened.front();
    CHECK(opened.place == ProblemRow::Place::SceneObject);
    CHECK(opened.object == 42);
    CHECK(opened.object_name == "Cube");
    CHECK(opened.scene_id == "default_scene");
    CHECK(opened.message == problem.at("message").get<std::string>());
    rig.dock.observeStatus(statusAt(2, 0));
    rig.answer(2, Json::array());
    CHECK(rig.tree()->topLevelItemCount() == 0);
}
