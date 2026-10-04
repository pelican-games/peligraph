// BS-1s: the engine Studio starts with --render-fallback keeps running when
// the project's rendering setup is refused at start, and draws a placeholder
// (engine BS-1). The row above the viewport says why and offers the way back
// -- on the real widget, with the test answering for the engine through the
// row's own hooks.

#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/renderfallback.hpp"
#include "problemsdock.hpp"
#include "renderfallbacknotice.hpp"

#include <QTreeWidget>

#include <QApplication>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextCursor>
#include <QLabel>
#include <QPushButton>

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
    static char name[] = "devstudio_render_fallback_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

// get_status of an engine on its placeholder, as BS-1's engine test reads it:
// the buffer port written without its element (CT-1's case).
Json fallbackStatus() {
    return Json{{"frame", 12},
                {"render_pipeline",
                 {{"state", "fallback"},
                  {"root", "passes/main_rendering_config.json"},
                  {"refusal",
                   {{"code", "graph.port_field_missing@1"},
                    {"node", "sim"},
                    {"port", "data"},
                    {"field", "element"},
                    {"document", "project://passes/main_rendering_config.json"},
                    {"message", "graph.port_field_missing@1 {\"document\":\"project://passes/"
                                "main_rendering_config.json\",\"field\":\"element\",\"node\":\"sim\","
                                "\"port\":\"data\"}"}}}}}};
}

Json readyStatus() {
    return Json{{"frame", 40},
                {"render_pipeline", {{"state", "ready"}, {"root", "passes/main_rendering_config.json"}}}};
}

struct Rig {
    int reloads = 0;
    int status_reads = 0;
    std::vector<QString> graph_opened;
    std::vector<QString> files_opened;
    std::vector<QString> restarts;
    std::vector<std::pair<bool, QString>> shown;
    // Decision 8: what the document's window asked the engine, its clock,
    // and the restores that committed.
    struct Request {
        std::int64_t id = 0;
        std::string method;
        Json params;
    };
    std::vector<Request> requests;
    std::int64_t next_id = 500;
    std::vector<std::function<void()>> tasks;
    std::vector<Json> restored;
    RenderFallbackNotice notice;

    Rig()
        : notice{RenderFallbackNotice::Hooks{
              .reload = [this] { ++reloads; },
              .open_graph = [this](const QString &node) { graph_opened.push_back(node); },
              .open_file = [this](const QString &document) { files_opened.push_back(document); },
              .restart = [this](const QString &reason) { restarts.push_back(reason); },
              .read_status = [this] { ++status_reads; },
              .request = [this](const std::string &method, const Json &params, std::string *) -> std::int64_t {
                  requests.push_back({++next_id, method, params});
                  return next_id;
              },
              .schedule = [this](int, std::function<void()> task) { tasks.push_back(std::move(task)); },
              .restored = [this](const Json &commit) { restored.push_back(commit); },
              .shown_changed = [this](bool on, const QString &line) { shown.emplace_back(on, line); },
          }} {}

    template <typename T>
    T *named(const char *name) {
        auto *found = notice.findChild<T *>(QString::fromLatin1(name));
        REQUIRE(found != nullptr);
        return found;
    }
    bool showing(const char *name) { return named<QPushButton>(name)->isVisibleTo(&notice); }
};

} // namespace

TEST_CASE("BS-1s decision 2: the placeholder is one sentence above the viewport -- the code's, the file "
          "and the node -- with Open in Graph and Open file",
          "[devstudio][render-fallback][bs1s]") {
    application();
    Rig rig;
    // Nothing to say before the engine has said anything, and nothing from an
    // engine before BS-1 (no render_pipeline) or one that is ready.
    CHECK(rig.notice.isHidden());
    rig.notice.observeStatus(Json{{"frame", 3}});
    CHECK(rig.notice.isHidden());
    rig.notice.observeStatus(readyStatus());
    CHECK(rig.notice.isHidden());
    CHECK(rig.shown.empty());

    rig.notice.observeStatus(fallbackStatus());
    CHECK_FALSE(rig.notice.isHidden());
    const QString sentence = QStringLiteral(
        "Showing a placeholder: the rendering setup was refused — Port data of sim needs element "
        "(graph.port_field_missing@1) in passes/main_rendering_config.json. Fix it, then Reload from disk.");
    CHECK(rig.notice.line()->text() == sentence);
    // The engine's own words stay for the tooltip and Copy message.
    CHECK(rig.notice.line()->toolTip().contains(QStringLiteral("rendering root: passes/main_rendering_config.json")));
    REQUIRE(rig.shown.size() == 1);
    CHECK(rig.shown.back() == std::pair{true, sentence});
    // The same reading again is not news.
    rig.notice.observeStatus(fallbackStatus());
    CHECK(rig.shown.size() == 1);

    CHECK(rig.showing("pelican.renderFallbackNotice.reload"));
    CHECK(rig.showing("pelican.renderFallbackNotice.openGraph"));
    CHECK(rig.showing("pelican.renderFallbackNotice.openFile"));
    CHECK_FALSE(rig.showing("pelican.renderFallbackNotice.restart"));
    rig.named<QPushButton>("pelican.renderFallbackNotice.openGraph")->click();
    CHECK(rig.graph_opened == std::vector<QString>{QStringLiteral("sim")});
    rig.named<QPushButton>("pelican.renderFallbackNotice.openFile")->click();
    CHECK(rig.files_opened == std::vector<QString>{QStringLiteral("passes/main_rendering_config.json")});

    // A refusal that names no node and no document: the root is the file,
    // nothing to open in the Graph, and a code Studio has no sentence for
    // reads as the engine wrote it.
    Json unnamed = fallbackStatus();
    unnamed["render_pipeline"]["refusal"] = {{"code", "render_pipeline_preflight_failed"},
                                             {"message", "the pipeline could not be prepared"}};
    rig.notice.observeStatus(unnamed);
    CHECK(rig.notice.line()->text() ==
          QStringLiteral("Showing a placeholder: the rendering setup was refused — the pipeline could not be "
                         "prepared (render_pipeline_preflight_failed) in passes/main_rendering_config.json. Fix "
                         "it, then Reload from disk."));
    CHECK_FALSE(rig.showing("pelican.renderFallbackNotice.openGraph"));
    CHECK(rig.showing("pelican.renderFallbackNotice.openFile"));

    // The Frame Plan's line is the same sentence (get_frame_plan carries the
    // same render_pipeline while the placeholder lasts).
    CHECK(renderFallbackSentence(readRenderPipeline(fallbackStatus())) == sentence.toStdString());
    CHECK(renderFallbackSentence(readRenderPipeline(readyStatus())).empty());
}

TEST_CASE("BS-1s decision 3: Reload from disk asks the reload; a reload or an edit that committed on the "
          "placeholder reads get_status again, and ready takes the notice away",
          "[devstudio][render-fallback][bs1s]") {
    application();
    Rig rig;
    const Json committed{{"status", "committed"}, {"operation", "edit_graph_document"}};
    // Before the placeholder nothing is read again for an edit.
    rig.notice.observeReply(committed);
    rig.notice.reloadCommitted();
    CHECK(rig.status_reads == 0);

    rig.notice.observeStatus(fallbackStatus());
    rig.named<QPushButton>("pelican.renderFallbackNotice.reload")->click();
    CHECK(rig.reloads == 1);
    // A reply that published nothing (accepted, rejected) is not followed.
    rig.notice.observeReply(Json{{"status", "accepted"}, {"ticket", "t1"}});
    rig.notice.observeReply(Json{{"status", "rejected"}, {"error", {{"code", "graph.port_field_missing@1"}}}});
    CHECK(rig.status_reads == 0);
    // A Graph edit that committed, a reload that committed, and a refused
    // reload: each is followed by get_status.
    rig.notice.observeReply(committed);
    CHECK(rig.status_reads == 1);
    rig.notice.reloadCommitted();
    CHECK(rig.status_reads == 2);
    rig.notice.observeReloadRefusal(Json{{"code", "graph.port_field_missing@1"}, {"message", "still broken"}});
    CHECK(rig.status_reads == 3);
    CHECK_FALSE(rig.notice.isHidden());

    // The engine publishes the project's setup: the row goes, once.
    rig.notice.observeStatus(readyStatus());
    CHECK(rig.notice.isHidden());
    REQUIRE(rig.shown.size() == 2);
    CHECK(rig.shown.back().first == false);
    rig.notice.observeReply(committed);
    CHECK(rig.status_reads == 3);

    // The engine going away (a restart, another project) takes it too.
    rig.notice.observeStatus(fallbackStatus());
    CHECK_FALSE(rig.notice.isHidden());
    rig.notice.engineGone();
    CHECK(rig.notice.isHidden());
}

TEST_CASE("BS-1s decision 4 / PB-1s decision 6: a fixed setup whose feature needs a module the engine started "
          "without is told by its code, render_feature_requires_restart@1, and its feature -- on the row above "
          "the viewport, in the reload's refusal and on the Problems row -- and Restart engine restarts it",
          "[devstudio][render-fallback][bs1s][pb1s]") {
    application();
    Rig rig;
    rig.notice.observeStatus(fallbackStatus());
    // Another refusal is not this one.
    rig.notice.observeReloadRefusal(Json{{"code", "graph.port_field_missing@1"}, {"message", "x"}});
    CHECK_FALSE(rig.showing("pelican.renderFallbackNotice.restart"));
    // Nor are the words alone: the module graph's own sentence, without the
    // code, says nothing Studio reads (PB-1s, decision 6).
    const Json words{{"code", "render_pipeline_preflight_failed"},
                     {"message", "Module 'struct Pelican::UiSystem' was first requested after the runtime module "
                                 "graph was frozen"}};
    CHECK(renderRestartFeature(words).empty());
    rig.notice.observeReloadRefusal(words);
    CHECK_FALSE(rig.showing("pelican.renderFallbackNotice.restart"));

    // The reload's refusal as engine PB-1 answers it: the code, the feature
    // and the module beside it.
    const Json frozen{{"code", "render_feature_requires_restart@1"},
                      {"feature", "debug_draw"},
                      {"module", "DebugDraw"},
                      {"message", "Render pipeline hot reload cannot enable feature 'debug_draw': feature "
                                  "'debug_draw' needs runtime module DebugDraw, which this engine was started "
                                  "without; restart the engine"}};
    CHECK(renderRestartFeature(frozen) == "debug_draw");
    // Relayed as error_fields, or quoted after the code, it is the same.
    CHECK(renderRestartFeature(Json{{"code", "render_feature_requires_restart@1"},
                                    {"error_fields", {{"feature", "ui"}, {"module", "UiRenderer"}}}}) == "ui");
    CHECK(renderRestartFeature(Json{{"code", "render_pipeline_preflight_failed"},
                                    {"message", "render_feature_requires_restart@1: {\"feature\":\"ui\","
                                                "\"module\":\"UiRenderer\"}"}}) == "ui");
    rig.notice.observeReloadRefusal(frozen);
    CHECK(rig.notice.line()->text() ==
          QStringLiteral("Restart the engine to enable debug_draw: the rendering setup needs it, and this engine "
                         "was started without it."));
    CHECK(rig.showing("pelican.renderFallbackNotice.restart"));
    CHECK_FALSE(rig.showing("pelican.renderFallbackNotice.reload"));
    // The next reading of the placeholder does not take the restart away.
    rig.notice.observeStatus(fallbackStatus());
    CHECK(rig.showing("pelican.renderFallbackNotice.restart"));
    rig.named<QPushButton>("pelican.renderFallbackNotice.restart")->click();
    REQUIRE(rig.restarts.size() == 1);
    CHECK(rig.restarts.back().contains(QStringLiteral("debug_draw")));

    // The same problem in the engine's list is a row whose fix is a restart:
    // a click goes nowhere, Restart engine under the list restarts.
    std::vector<ProblemRow> opened;
    std::vector<QString> restarts;
    std::vector<qint64> asked;
    ProblemsDock problems{ProblemsDock::Hooks{
        .send = [&](const QString &, const QJsonObject &, QString *) -> qint64 {
            asked.push_back(static_cast<qint64>(asked.size()) + 700);
            return asked.back();
        },
        .open = [&](const ProblemRow &row) { opened.push_back(row); },
        .restart = [&](const QString &reason) { restarts.push_back(reason); },
    }};
    problems.engineAvailable();
    problems.observeStatus(Json{{"problems", {{"revision", 3}, {"count", 1}}}});
    REQUIRE(asked.size() == 1);
    REQUIRE(problems.receiveResult(
        asked.back(),
        QByteArray::fromStdString(
            Json{{"revision", 3},
                 {"problems", Json::array({{{"id", "0123456789abcdef"},
                                            {"severity", "error"},
                                            {"source", "reload_rpc"},
                                            {"code", "render_feature_requires_restart@1"},
                                            {"message", frozen.at("message")},
                                            {"fields", {{"feature", "debug_draw"}, {"module", "DebugDraw"}}}}})}}
                .dump())));
    const auto &rows = problems.session().rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().place == ProblemRow::Place::Restart);
    CHECK(rows.front().restart_feature == "debug_draw");
    CHECK(rows.front().place_text == "Restart the engine");
    CHECK(rows.front().sentence ==
          "The rendering setup turns on debug_draw, whose runtime module DebugDraw this engine was started "
          "without; restart the engine (render_feature_requires_restart@1).");
    problems.openProblem(QStringLiteral("0123456789abcdef"));
    CHECK(opened.empty());
    problems.list()->setCurrentItem(problems.list()->topLevelItem(0));
    REQUIRE(problems.restartButton()->isVisibleTo(&problems));
    problems.restartButton()->click();
    REQUIRE(restarts.size() == 1);
    CHECK(restarts.back().contains(QStringLiteral("debug_draw")));
    // The restart takes the engine away; the new one starts ready.
    rig.notice.engineGone();
    rig.notice.observeStatus(readyStatus());
    CHECK(rig.notice.isHidden());
}

namespace {

// get_status of an engine started on a root whose last } is gone (BS-1b).
Json syntaxStatus() {
    return Json{{"render_pipeline",
                 {{"state", "fallback"},
                  {"root", "passes/main_rendering_config.json"},
                  {"refusal",
                   {{"code", "render_document_syntax@1"},
                    {"document", "project://passes/main_rendering_config.json"},
                    {"line", 3},
                    {"column", 1},
                    {"reason", "syntax error while parsing object - unexpected end of input; expected '}'"},
                    {"message", "render_document_syntax@1: project://passes/main_rendering_config.json is not "
                                "valid JSON at line 3, column 1: syntax error while parsing object - unexpected "
                                "end of input; expected '}'"}}}}}};
}

} // namespace

TEST_CASE("BS-1s decision 7: a render document that is not JSON is one sentence -- the file, the line and the "
          "column -- wherever the refusal is read",
          "[devstudio][render-fallback][bs1s]") {
    application();
    const std::string sentence =
        "passes/main_rendering_config.json is not valid JSON at line 3, column 1: syntax error while parsing "
        "object - unexpected end of input; expected '}' (render_document_syntax@1).";
    const Json error = syntaxStatus()["render_pipeline"]["refusal"];
    CHECK(renderDocumentSyntaxDescription(error) == sentence);
    // Without a line and a column the clause is left out.
    CHECK(renderDocumentSyntaxDescription(Json{{"code", "render_document_syntax@1"},
                                               {"document", "project://passes/feature.json"},
                                               {"reason", "unexpected end of input"}}) ==
          "passes/feature.json is not valid JSON: unexpected end of input (render_document_syntax@1).");
    // A rejection quoting its fields after the code reads the same (the Graph's
    // and the reload's refusals are read through knownRefusalDescription).
    const Json quoted{{"document", "project://passes/main_rendering_config.json"},
                      {"line", 3},
                      {"column", 1},
                      {"reason", "syntax error while parsing object - unexpected end of input; expected '}'"}};
    CHECK(knownRefusalDescription("render_document_syntax@1", "render_document_syntax@1: " + quoted.dump()) ==
          sentence);
    CHECK(renderDocumentSyntaxDescription(Json{{"code", "graph.port_field_missing@1"}, {"message", "x"}}).empty());

    Rig rig;
    rig.notice.observeStatus(syntaxStatus());
    CHECK(rig.notice.line()->text().toStdString() ==
          "Showing a placeholder: the rendering setup was refused \xE2\x80\x94 " +
              sentence.substr(0, sentence.size() - 1) + ". Fix it, then Reload from disk.");
    CHECK(rig.showing("pelican.renderFallbackNotice.showDocument"));
    CHECK(rig.showing("pelican.renderFallbackNotice.reload"));
    CHECK_FALSE(rig.showing("pelican.renderFallbackNotice.openGraph"));
    // A structural refusal offers no document window.
    rig.notice.observeStatus(fallbackStatus());
    CHECK_FALSE(rig.showing("pelican.renderFallbackNotice.showDocument"));
}

TEST_CASE("BS-1s decision 8: Show the document opens the bytes on disk at the line and the column, Save waits "
          "for JSON and restores it against the broken digest, a refusal is one sentence, and the commit "
          "closes the window",
          "[devstudio][render-fallback][bs1s]") {
    application();
    Rig rig;
    rig.notice.observeStatus(syntaxStatus());
    rig.named<QPushButton>("pelican.renderFallbackNotice.showDocument")->click();
    REQUIRE(rig.requests.size() == 1);
    CHECK(rig.requests.back().method == "get_render_documents");
    QPointer<QDialog> dialog = rig.notice.documentDialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->objectName() == QStringLiteral("pelican.renderDocumentDialog"));
    auto *text = dialog->findChild<QPlainTextEdit *>(QStringLiteral("pelican.renderDocumentDialog.text"));
    auto *notice = dialog->findChild<QLabel *>(QStringLiteral("pelican.renderDocumentDialog.notice"));
    auto *save = dialog->findChild<QPushButton *>(QStringLiteral("pelican.renderDocumentDialog.save"));
    REQUIRE(text != nullptr);
    REQUIRE(notice != nullptr);
    REQUIRE(save != nullptr);
    CHECK_FALSE(save->isEnabled());

    // The document as get_render_documents lists it: the raw bytes (CRLF
    // here), and the digest of those bytes.
    const std::string broken = "{\r\n  \"a\": 1\r\n";
    const std::string digest(64, 'b');
    const Json documents{{"documents",
                          Json::array({{{"reference", "passes/other.json"},
                                        {"kind", "preset"},
                                        {"digest", "x"},
                                        {"content", "{}"}},
                                       {{"reference", "passes/main_rendering_config.json"},
                                        {"kind", "root"},
                                        {"digest", digest},
                                        {"content", broken}}})}};
    CHECK(rig.notice.receiveResult(rig.requests.back().id, QByteArray::fromStdString(documents.dump())));
    CHECK(text->toPlainText() == QStringLiteral("{\n  \"a\": 1\n"));
    // The cursor where the parser stopped: line 3, column 1.
    CHECK(text->textCursor().blockNumber() == 2);
    CHECK(text->textCursor().positionInBlock() == 0);
    CHECK_FALSE(save->isEnabled());
    CHECK(notice->text().startsWith(QStringLiteral("Not valid JSON yet at line ")));

    // Fixed: Save can be pressed and restores it whole, CRLF kept, against
    // the digest of the broken bytes.
    text->setPlainText(QStringLiteral("{\n  \"a\": 1\n}"));
    CHECK(save->isEnabled());
    CHECK(notice->text().startsWith(QStringLiteral("Valid JSON")));
    save->click();
    REQUIRE(rig.requests.size() == 2);
    CHECK(rig.requests.back().method == "restore_render_documents");
    CHECK(rig.requests.back().params ==
          Json{{"documents", Json::array({{{"reference", "passes/main_rendering_config.json"},
                                           {"base_digest", digest},
                                           {"content", "{\r\n  \"a\": 1\r\n}"}}})}});
    CHECK_FALSE(save->isEnabled());
    // A ticket, followed until it answers.
    CHECK(rig.notice.receiveResult(rig.requests.back().id,
                                   QByteArray::fromStdString(Json{{"status", "accepted"}, {"ticket", "t1"}}.dump())));
    REQUIRE(rig.tasks.size() == 1);
    rig.tasks.back()();
    REQUIRE(rig.requests.size() == 3);
    CHECK(rig.requests.back().method == "get_edit_result");
    CHECK(rig.requests.back().params == Json{{"ticket", "t1"}});
    // Refused: one sentence, the window stays and Save can be pressed again.
    const Json rejected{{"status", "rejected"},
                        {"error",
                         {{"method", "restore_render_documents"},
                          {"code", "render_document_syntax@1"},
                          {"document", "project://passes/main_rendering_config.json"},
                          {"line", 1},
                          {"column", 2},
                          {"reason", "invalid literal"},
                          {"message", "render_document_syntax@1: ..."}}}};
    CHECK(rig.notice.receiveResult(rig.requests.back().id, QByteArray::fromStdString(rejected.dump())));
    CHECK(notice->text() == QStringLiteral("passes/main_rendering_config.json is not valid JSON at line 1, "
                                           "column 2: invalid literal (render_document_syntax@1)."));
    CHECK(save->isEnabled());
    CHECK(dialog != nullptr);

    // Saved again and committed: the commit is not taken (the window reads it
    // as every commit), restored is told, and the window goes.
    save->click();
    REQUIRE(rig.requests.size() == 4);
    CHECK(rig.requests.back().method == "restore_render_documents");
    const Json committed{{"status", "committed"},
                         {"operation", "restore_render_documents"},
                         {"documents", {{"passes/main_rendering_config.json", std::string(64, 'c')}}}};
    CHECK_FALSE(rig.notice.receiveResult(rig.requests.back().id, QByteArray::fromStdString(committed.dump())));
    REQUIRE(rig.restored.size() == 1);
    CHECK(rig.restored.back() == committed);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    CHECK(dialog == nullptr);
    // A reply nobody asked for is not this window's.
    CHECK_FALSE(rig.notice.receiveResult(9999, QByteArray{"{}"}));
}
