// RU-1: the render edit history (Studio's Undo / Redo of render edits, on
// RD-1's get_render_documents / restore_render_documents). No Qt and no
// engine: the transport is a list of what was sent, and the test answers each
// request as the engine does.
//
// What is judged is what goes on the wire: the documents read before an edit,
// the base_digest and content an Undo and a Redo restore with, and that an
// edit which changed nothing, one that created a file, and a refusal each
// leave the history as the spec says.

#include "renderedithistory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

const std::string Root = "passes/main.json";
const std::string Preset = "project://passes/local-pipeline.json";
const std::string RootA(64, 'a');
const std::string PresetBefore(64, '1');
const std::string PresetAfter(64, '2');
const std::string PresetAgain(64, '3');

struct Sent {
    std::int64_t id = 0;
    std::string method;
    Json params;
};

struct Rig {
    std::vector<Sent> sent;
    std::vector<std::function<void()>> scheduled;
    std::int64_t next_id = 1;
    int restores_committed = 0;
    RenderEditHistory history{RenderEditHistory::Transport{
        .request = [this](const std::string &method, const Json &params, std::string *) {
            sent.push_back(Sent{next_id, method, params});
            return next_id++;
        },
        .schedule = [this](int, std::function<void()> task) {
            scheduled.push_back(std::move(task));
        },
    }};

    Rig() {
        history.restored = [this](const Json &, bool) { ++restores_committed; };
    }

    const Sent &last() const {
        REQUIRE_FALSE(sent.empty());
        return sent.back();
    }

    // A panel sends `method`: the history hears it before and after, as the
    // viewport tells it. Answers the panel's request id.
    std::int64_t panelSends(const std::string &method, const Json &params) {
        history.beforeRequest(method, params);
        const auto id = 1000 + next_id++;
        history.requestSent(id, method, params);
        return id;
    }

    void answer(std::int64_t id, const Json &result) { CHECK(history.receiveResult(id, result)); }

    void runScheduled() {
        auto tasks = std::move(scheduled);
        scheduled.clear();
        for (auto &task : tasks) task();
    }

    // One edit, the whole way: its documents read before it, accepted, a
    // pending poll, then the commit.
    void edit(const std::string &method, const Json &params, const Json &before_documents,
              const Json &commit) {
        const auto before = sent.size();
        const auto panel = panelSends(method, params);
        if (!before_documents.is_null()) {
            REQUIRE(sent.size() == before + 1);
            CHECK(sent.back().method == "get_render_documents");
            // All of them: which ones the edit writes is known only after.
            CHECK(sent.back().params == Json::object());
            answer(sent.back().id, Json{{"documents", before_documents}});
        } else {
            CHECK(sent.size() == before);
        }
        answer(panel, Json{{"status", "accepted"}, {"ticket", "t-" + method}});
        REQUIRE(last().method == "get_edit_result");
        CHECK(last().params == Json{{"ticket", "t-" + method}});
        answer(last().id, Json{{"ticket", "t-" + method}, {"status", "pending"}});
        REQUIRE(scheduled.size() == 1);
        runScheduled();
        REQUIRE(last().method == "get_edit_result");
        answer(last().id, commit);
    }
};

Json document(const std::string &reference, const std::string &kind, const std::string &digest,
              const std::string &content) {
    return {{"reference", reference}, {"kind", kind}, {"digest", digest}, {"content", content}};
}

Json committed(const Json &documents) {
    return {{"status", "committed"},
            {"committed", true},
            {"source_digest", {{"hex", RootA}}},
            {"documents", documents}};
}

const Json PresetEdit{{"base_source_digest", RootA},
                      {"base_preset_digest", PresetBefore},
                      {"graph", "main_render"},
                      {"pass_name", "present"}};

Json bothBefore() {
    return Json::array({document(Root, "root", RootA, "root bytes"),
                        document(Preset, "preset", PresetBefore, "preset before")});
}

// An edit of the preset's pass, recorded.
void recordPresetEdit(Rig &rig) {
    rig.edit("edit_preset_pass", PresetEdit, bothBefore(), committed({{Preset, PresetAfter}}));
    REQUIRE(rig.history.undoStack().size() == 1);
}

} // namespace

TEST_CASE("RU-1 an edit is recorded with the documents it wrote, and Undo / Redo restore them "
          "under the digests the commits named",
          "[devstudio][render-history][ru1][negative-contrast]") {
    Rig rig;
    recordPresetEdit(rig);
    const auto &entry = rig.history.undoStack().back();
    CHECK(entry.label == "Edit pass present");
    CHECK_FALSE(entry.barrier);
    // Only what the commit wrote: the root it did not touch is not kept.
    REQUIRE(entry.before.size() == 1);
    CHECK(entry.before.at(Preset).content == "preset before");
    CHECK(entry.after.at(Preset) == PresetAfter);
    CHECK(rig.history.undoLabel() == "Edit pass present");
    CHECK(rig.history.canUndo());
    CHECK_FALSE(rig.history.canRedo());

    // Undo: the document as it is now is read first (for Redo) ...
    REQUIRE(rig.history.undo());
    CHECK(rig.history.busy());
    REQUIRE(rig.last().method == "get_render_documents");
    CHECK(rig.last().params == Json{{"references", Json::array({Preset})}});
    rig.answer(rig.last().id,
               Json{{"documents", Json::array({document(Preset, "preset", PresetAfter,
                                                        "preset after")})}});
    // ... then the content before the edit goes back, under the digest the
    // edit's commit wrote. Control: the digest read before the edit is the
    // wrong base -- the engine holds PresetAfter now.
    REQUIRE(rig.last().method == "restore_render_documents");
    const auto &undo_documents = rig.last().params.at("documents");
    REQUIRE(undo_documents.size() == 1);
    CHECK(undo_documents[0].at("reference") == Preset);
    CHECK(undo_documents[0].at("base_digest") == PresetAfter);
    CHECK(undo_documents[0].at("base_digest") != PresetBefore);
    CHECK(undo_documents[0].at("content") == "preset before");
    rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "undo-1"}});
    REQUIRE(rig.last().method == "get_edit_result");
    CHECK(rig.last().params == Json{{"ticket", "undo-1"}});
    rig.answer(rig.last().id, Json{{"status", "pending"}});
    rig.runScheduled();
    REQUIRE(rig.last().method == "get_edit_result");
    rig.answer(rig.last().id, committed({{Preset, PresetBefore}}));
    CHECK_FALSE(rig.history.busy());
    CHECK(rig.restores_committed == 1);
    CHECK(rig.history.undoStack().empty());
    REQUIRE(rig.history.redoStack().size() == 1);
    CHECK(rig.history.redoLabel() == "Edit pass present");
    CHECK(rig.history.line() == "Undid \"Edit pass present\".");

    // Redo: the other way -- the content Undo read, under the digest Undo's
    // commit wrote.
    REQUIRE(rig.history.redo());
    REQUIRE(rig.last().method == "get_render_documents");
    CHECK(rig.last().params == Json{{"references", Json::array({Preset})}});
    rig.answer(rig.last().id,
               Json{{"documents", Json::array({document(Preset, "preset", PresetBefore,
                                                        "preset before")})}});
    REQUIRE(rig.last().method == "restore_render_documents");
    const auto &redo_documents = rig.last().params.at("documents");
    REQUIRE(redo_documents.size() == 1);
    CHECK(redo_documents[0].at("base_digest") == PresetBefore);
    CHECK(redo_documents[0].at("content") == "preset after");
    rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "redo-1"}});
    rig.answer(rig.last().id, committed({{Preset, PresetAgain}}));
    CHECK(rig.restores_committed == 2);
    CHECK(rig.history.redoStack().empty());
    REQUIRE(rig.history.undoStack().size() == 1);
    // The next Undo's base is what the Redo's commit wrote.
    CHECK(rig.history.undoStack().back().after.at(Preset) == PresetAgain);
    CHECK(rig.history.line() == "Redid \"Edit pass present\".");

    // A new edit after an Undo drops what could be redone.
    REQUIRE(rig.history.undo());
    rig.answer(rig.last().id,
               Json{{"documents", Json::array({document(Preset, "preset", PresetAgain,
                                                        "preset after")})}});
    CHECK(rig.last().params.at("documents")[0].at("base_digest") == PresetAgain);
    rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "undo-2"}});
    rig.answer(rig.last().id, committed({{Preset, PresetBefore}}));
    REQUIRE(rig.history.redoStack().size() == 1);
    rig.edit("decompose_material_pass", Json{{"pass", "deferred_geometry"}}, bothBefore(),
             committed({{Root, std::string(64, 'e')}, {Preset, std::string(64, 'f')}}));
    CHECK(rig.history.redoStack().empty());
    REQUIRE(rig.history.undoStack().size() == 1);
    CHECK(rig.history.undoLabel() == "Decompose deferred_geometry");
    // Both documents it wrote come back in one restore.
    REQUIRE(rig.history.undo());
    rig.answer(rig.last().id,
               Json{{"documents",
                     Json::array({document(Root, "root", std::string(64, 'e'), "root now"),
                                  document(Preset, "preset", std::string(64, 'f'),
                                           "preset now")})}});
    REQUIRE(rig.last().method == "restore_render_documents");
    CHECK(rig.last().params.at("documents") ==
          Json::array({{{"reference", Root}, {"base_digest", std::string(64, 'e')},
                        {"content", "root bytes"}},
                       {{"reference", Preset}, {"base_digest", std::string(64, 'f')},
                        {"content", "preset before"}}}));
}

TEST_CASE("RU-1 an edit that changed nothing, or was refused, is not recorded",
          "[devstudio][render-history][ru1]") {
    Rig rig;
    rig.edit("edit_render_targets",
             Json{{"ops", Json::array({{{"op", "remove"}, {"name", "scratch"}}})}}, bothBefore(),
             Json{{"status", "committed"},
                  {"committed", true},
                  {"no_change", true},
                  {"documents", Json::object()}});
    CHECK(rig.history.undoStack().empty());
    CHECK_FALSE(rig.history.canUndo());
    CHECK_FALSE(rig.history.undo());
    CHECK(rig.history.line() == "Nothing to undo.");

    // Refused at once: not followed any further.
    const auto before = rig.sent.size();
    const auto panel = rig.panelSends("edit_compute_task", Json{{"task_name", "blur"}});
    rig.answer(rig.sent.back().id, Json{{"documents", bothBefore()}});
    rig.answer(panel, Json{{"status", "rejected"},
                           {"error", {{"code", "external_modification"}}}});
    CHECK(rig.sent.size() == before + 1);
    CHECK(rig.history.undoStack().empty());
    // Refused at commit: the same.
    rig.edit("edit_compute_task", Json{{"task_name", "blur"}}, bothBefore(),
             Json{{"status", "rejected"},
                  {"committed", false},
                  {"error", {{"code", "render_pipeline_preflight_failed"}}}});
    CHECK(rig.history.undoStack().empty());

    // A request the history does not follow is not read before.
    const auto quiet = rig.sent.size();
    rig.panelSends("edit_graph_document", Json{{"ops", Json::array()}});
    rig.panelSends("get_frame_plan", Json::object());
    CHECK(rig.sent.size() == quiet);
}

TEST_CASE("RU-1 an edit that created or removed a file is a barrier Undo stops at",
          "[devstudio][render-history][ru1]") {
    Rig rig;
    recordPresetEdit(rig);
    // Add pass: a barrier by what it is. Nothing is read before it.
    rig.edit("add_authored_pass",
             Json{{"graph", "main_render"}, {"insert", "after:present"}, {"pass", {{"name", "bloom"}}}},
             Json(), committed({{Root, std::string(64, 'b')}}));
    REQUIRE(rig.history.undoStack().size() == 2);
    CHECK(rig.history.undoStack().back().barrier);
    CHECK(rig.history.undoLabel() == "Add pass bloom");
    // Undo is offered (it says why it stops), and sends nothing.
    CHECK(rig.history.canUndo());
    const auto before = rig.sent.size();
    CHECK_FALSE(rig.history.undo());
    CHECK(rig.sent.size() == before);
    CHECK(rig.history.line() ==
          "Cannot undo past \"Add pass bloom\": it created or removed files. (RD-1b)");
    // Still there: the edit before it cannot be reached.
    CHECK(rig.history.undoStack().size() == 2);

    SECTION("an edit whose commit wrote a document that was not there before") {
        rig.edit("edit_render_targets",
                 Json{{"ops", Json::array({{{"op", "add"}, {"target", {{"name", "hdr"}}}}})}},
                 bothBefore(), committed({{"project://render_targets.json", std::string(64, 'c')}}));
        CHECK(rig.history.undoStack().back().barrier);
        CHECK(rig.history.undoLabel() == "Add render target hdr");
        CHECK_FALSE(rig.history.undo());
    }
    SECTION("an edit whose commit erased a document") {
        rig.edit("edit_render_features",
                 Json{{"ops", Json::array({{{"op", "remove"}, {"feature", "bloom"}}})}},
                 bothBefore(), committed({{Preset, nullptr}}));
        CHECK(rig.history.undoStack().back().barrier);
        CHECK_FALSE(rig.history.undo());
    }
    SECTION("Make editable") {
        rig.edit("edit_render_features",
                 Json{{"ops", Json::array({{{"op", "materialize"}, {"scope", "preset"}}})}}, Json(),
                 committed({{Root, std::string(64, 'd')}}));
        CHECK(rig.history.undoStack().back().barrier);
        CHECK(rig.history.undoLabel() == "Make feature editable");
    }
}

TEST_CASE("RU-1 a restore the engine refuses says so in one line and keeps the history",
          "[devstudio][render-history][ru1]") {
    Rig rig;
    recordPresetEdit(rig);
    const auto undo_to_restore = [&] {
        REQUIRE(rig.history.undo());
        rig.answer(rig.last().id,
                   Json{{"documents", Json::array({document(Preset, "preset", PresetAgain,
                                                            "changed by the Graph tab")})}});
        REQUIRE(rig.last().method == "restore_render_documents");
    };

    SECTION("external_modification names the document and the way out") {
        undo_to_restore();
        rig.answer(rig.last().id,
                   Json{{"status", "rejected"},
                        {"committed", false},
                        {"error",
                         {{"code", "external_modification"},
                          {"message", "Render document " + Preset + " changed since it was read"},
                          {"method", "restore_render_documents"},
                          {"reference", Preset},
                          {"expected_digest", PresetAgain},
                          {"provided_digest", PresetAfter},
                          {"mismatches", Json::array()}}}});
        CHECK(rig.history.line() ==
              "\"Edit pass present\" cannot be undone: " + Preset +
                  " changed since. Undo the later edit first, or refresh.");
        CHECK_FALSE(rig.history.busy());
        REQUIRE(rig.history.undoStack().size() == 1);
        CHECK(rig.history.redoStack().empty());
        CHECK(rig.restores_committed == 0);
        // Asked again, it is sent again, with the same base.
        undo_to_restore();
        CHECK(rig.last().params.at("documents")[0].at("base_digest") == PresetAfter);
    }
    SECTION("at commit, without a reference") {
        undo_to_restore();
        rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "undo-x"}});
        rig.answer(rig.last().id, Json{{"status", "rejected"},
                                       {"committed", false},
                                       {"error",
                                        {{"code", "external_modification"},
                                         {"message", "render config source changed"}}}});
        CHECK(rig.history.line() ==
              "\"Edit pass present\" cannot be undone: a render document changed since. "
              "Undo the later edit first, or refresh.");
        CHECK(rig.history.undoStack().size() == 1);
    }
    SECTION("render_pipeline_preflight_failed is the engine's own sentence") {
        undo_to_restore();
        rig.answer(rig.last().id,
                   Json{{"status", "rejected"},
                        {"committed", false},
                        {"error",
                         {{"code", "render_pipeline_preflight_failed"},
                          {"message", "pass present reads scene_color, which nothing writes"}}}});
        CHECK(rig.history.line() == "pass present reads scene_color, which nothing writes");
        CHECK(rig.history.undoStack().size() == 1);
    }
    SECTION("the read before it is refused") {
        REQUIRE(rig.history.undo());
        rig.answer(rig.last().id,
                   Json{{"status", "rejected"},
                        {"committed", false},
                        {"error",
                         {{"code", "render_document_not_writable"},
                          {"message", "Render document x cannot be read or restored here"}}}});
        CHECK(rig.history.line() ==
              "\"Edit pass present\" cannot be undone: Render document x cannot be read or "
              "restored here");
        CHECK(rig.history.undoStack().size() == 1);
    }
    SECTION("the transport fails") {
        REQUIRE(rig.history.undo());
        CHECK(rig.history.receiveFailure(rig.last().id, "RPC -32000: gone"));
        CHECK(rig.history.line() == "\"Edit pass present\" cannot be undone: RPC -32000: gone");
        CHECK_FALSE(rig.history.busy());
    }
}

TEST_CASE("RU-1 Undo waits for what is in flight, and an engine that went away drops it",
          "[devstudio][render-history][ru1]") {
    Rig rig;
    recordPresetEdit(rig);
    // A panel's edit is on its way: Undo would restore under it.
    const auto panel = rig.panelSends("edit_preset_pass", PresetEdit);
    rig.answer(rig.sent.back().id, Json{{"documents", bothBefore()}});
    const auto before = rig.sent.size();
    CHECK_FALSE(rig.history.undo());
    CHECK(rig.history.line() == "Wait for the render edit in flight.");
    CHECK(rig.sent.size() == before);
    // An Undo in flight: a second one waits.
    rig.history.abandonInFlight();
    CHECK_FALSE(rig.history.waitingFor(panel));
    REQUIRE(rig.history.undo());
    CHECK_FALSE(rig.history.undo());
    CHECK(rig.history.line() == "Wait for the Undo or Redo in flight.");
    // The engine goes: the history stays, nothing is in flight.
    rig.history.abandonInFlight();
    CHECK_FALSE(rig.history.busy());
    CHECK(rig.history.undoStack().size() == 1);
    // Another project: nothing of it applies.
    rig.history.clear();
    CHECK(rig.history.undoStack().empty());
    CHECK_FALSE(rig.history.canUndo());
}

} // namespace PelicanStudio
