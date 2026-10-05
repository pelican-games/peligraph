// RU-1: the render edit history (Studio's Undo / Redo of render edits, on
// RD-1's get_render_documents / restore_render_documents). No Qt and no
// engine: the transport is a list of what was sent, and the test answers each
// request as the engine does.
//
// What is judged is what goes on the wire: the documents read before an edit,
// the base_digest and content an Undo and a Redo restore with, and that an
// edit which changed nothing and a refusal each leave the history as the spec
// says. RU-1b (on RD-1b): an edit that created or removed a document is
// undone and redone like any other, and a root that changed since only in
// placement does not refuse the Undo.

#include "renderedithistory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

const std::string Root = "passes/main.json";
const std::string Preset = "project://passes/local-pipeline.json";
const std::string Fragment = "project://passes/authoring/pass-0123456789abcdef0123.json";
const std::string Feature = "project://features/bloom.json";
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

    // One edit, the whole way: its documents read before it (every one,
    // unnamed; remove_authored_pass also names its fragment), accepted, a
    // pending poll, then the commit, and the read of what the commit wrote.
    // A null `before_documents` leaves the read before unanswered; a null
    // `after_documents` the read after. `named` answers the named read.
    void edit(const std::string &method, const Json &params, const Json &before_documents,
              const Json &commit, const Json &after_documents = Json(),
              const Json &named = Json()) {
        const auto before = sent.size();
        const auto panel = panelSends(method, params);
        const bool names = method == "remove_authored_pass";
        REQUIRE(sent.size() == before + (names ? 2 : 1));
        const auto snapshot = sent[before];
        CHECK(snapshot.method == "get_render_documents");
        // All of them: which ones the edit writes is known only after.
        CHECK(snapshot.params == Json::object());
        if (!before_documents.is_null())
            answer(snapshot.id, Json{{"documents", before_documents}});
        if (names) {
            const auto read = sent[before + 1];
            CHECK(read.method == "get_render_documents");
            CHECK(read.params ==
                  Json{{"references", Json::array({params.at("fragment_reference")})}});
            answer(read.id, named.is_null()
                                ? Json{{"status", "rejected"},
                                       {"error", {{"code", "render_document_not_writable"}}}}
                                : named);
        }
        answer(panel, Json{{"status", "accepted"}, {"ticket", "t-" + method}});
        REQUIRE(last().method == "get_edit_result");
        CHECK(last().params == Json{{"ticket", "t-" + method}});
        answer(last().id, Json{{"ticket", "t-" + method}, {"status", "pending"}});
        REQUIRE(scheduled.size() == 1);
        runScheduled();
        REQUIRE(last().method == "get_edit_result");
        const auto committed_at = sent.size();
        answer(last().id, commit);
        if (sent.size() == committed_at) return;
        // Decision 3: what the commit wrote is read at once, by name.
        REQUIRE(sent.size() == committed_at + 1);
        CHECK(last().method == "get_render_documents");
        auto written = Json::array();
        for (const auto &[reference, digest] : commit.at("documents").items())
            if (digest.is_string()) written.push_back(reference);
        CHECK(last().params == Json{{"references", written}});
        if (!after_documents.is_null())
            answer(last().id, Json{{"documents", after_documents}});
    }

    // An Undo or a Redo the whole way: the read of the documents as they are
    // now, the restore (whose documents are returned), and its commit.
    Json restore(bool undoing, const Json &current, const Json &commit) {
        REQUIRE((undoing ? history.undo() : history.redo()));
        REQUIRE(last().method == "get_render_documents");
        answer(last().id, Json{{"documents", current}});
        REQUIRE(last().method == "restore_render_documents");
        const auto documents = last().params.at("documents");
        answer(last().id, Json{{"status", "accepted"}, {"ticket", "restore"}});
        REQUIRE(last().method == "get_edit_result");
        answer(last().id, commit);
        return documents;
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

Json restored(const std::string &reference, const Json &base, const Json &content) {
    return {{"reference", reference}, {"base_digest", base}, {"content", content}};
}

const Json PresetEdit{{"base_source_digest", RootA},
                      {"base_preset_digest", PresetBefore},
                      {"graph", "main_render"},
                      {"pass_name", "present"}};

Json bothBefore() {
    return Json::array({document(Root, "root", RootA, "root bytes"),
                        document(Preset, "preset", PresetBefore, "preset before")});
}

Json rootBefore() { return Json::array({document(Root, "root", RootA, "root bytes")}); }

// An edit of the preset's pass, recorded.
void recordPresetEdit(Rig &rig) {
    rig.edit("edit_preset_pass", PresetEdit, bothBefore(), committed({{Preset, PresetAfter}}));
    REQUIRE(rig.history.undoStack().size() == 1);
}

const Json AddPass{{"graph", "main_render"},
                   {"insert", "after:forward_opaque"},
                   {"pass", {{"name", "tint"}}}};

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
    REQUIRE(entry.before.at(Preset));
    CHECK(entry.before.at(Preset)->content == "preset before");
    REQUIRE(entry.written.at(Preset));
    CHECK(entry.written.at(Preset)->digest == PresetAfter);
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
    CHECK(rig.history.undoStack().back().written.at(Preset)->digest == PresetAgain);
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
          Json::array({restored(Root, std::string(64, 'e'), "root bytes"),
                       restored(Preset, std::string(64, 'f'), "preset before")}));
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

TEST_CASE("RU-1b Add pass is undone by restoring the root and deleting its fragment in one "
          "restore, and Redo writes the fragment again",
          "[devstudio][render-history][ru1b][negative-contrast]") {
    Rig rig;
    const std::string RootAdded(64, 'b');
    const std::string FragmentAdded(64, 'c');
    rig.edit("add_authored_pass", AddPass, rootBefore(),
             committed({{Fragment, FragmentAdded}, {Root, RootAdded}}));
    REQUIRE(rig.history.undoStack().size() == 1);
    const auto &entry = rig.history.undoStack().back();
    CHECK(entry.label == "Add pass tint");
    // Control: RU-1 kept this as a barrier Undo stopped at.
    CHECK_FALSE(entry.barrier);
    // The fragment was not there before the edit.
    CHECK_FALSE(entry.before.at(Fragment).has_value());
    CHECK(entry.before.at(Root)->content == "root bytes");

    // Undo reads both as they are now, and sends one restore: the root as
    // it was, and the fragment deleted under the digest the commit named.
    REQUIRE(rig.history.undo());
    REQUIRE(rig.last().method == "get_render_documents");
    CHECK(rig.last().params == Json{{"references", Json::array({Root, Fragment})}});
    rig.answer(rig.last().id,
               Json{{"documents",
                     Json::array({document(Root, "root", RootAdded, "root with the pass"),
                                  document(Fragment, "feature", FragmentAdded,
                                           "fragment bytes")})}});
    REQUIRE(rig.last().method == "restore_render_documents");
    CHECK(rig.last().params.at("documents") ==
          Json::array({restored(Root, RootAdded, "root bytes"),
                       restored(Fragment, FragmentAdded, nullptr)}));
    rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "undo"}});
    rig.answer(rig.last().id, committed({{Root, RootA}, {Fragment, nullptr}}));
    CHECK(rig.history.line() == "Undid \"Add pass tint\".");
    REQUIRE(rig.history.redoStack().size() == 1);
    CHECK_FALSE(rig.history.redoStack().back().written.at(Fragment).has_value());

    // Redo: the fragment is not there to read; it is written again (base
    // null) with the content Undo read, and the root under Undo's digest.
    REQUIRE(rig.history.redo());
    REQUIRE(rig.last().method == "get_render_documents");
    CHECK(rig.last().params == Json{{"references", Json::array({Root})}});
    rig.answer(rig.last().id, Json{{"documents", rootBefore()}});
    REQUIRE(rig.last().method == "restore_render_documents");
    CHECK(rig.last().params.at("documents") ==
          Json::array({restored(Root, RootA, "root with the pass"),
                       restored(Fragment, nullptr, "fragment bytes")}));
    const std::string RootAgain(64, 'd');
    rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "redo"}});
    rig.answer(rig.last().id, committed({{Fragment, FragmentAdded}, {Root, RootAgain}}));
    CHECK(rig.history.line() == "Redid \"Add pass tint\".");

    // And undone again, under what the Redo wrote.
    const auto again = rig.restore(
        true,
        Json::array({document(Root, "root", RootAgain, "root with the pass"),
                     document(Fragment, "feature", FragmentAdded, "fragment bytes")}),
        committed({{Root, RootA}, {Fragment, nullptr}}));
    CHECK(again == Json::array({restored(Root, RootAgain, "root bytes"),
                                restored(Fragment, FragmentAdded, nullptr)}));
}

TEST_CASE("RU-1b Remove pass is undone by writing the fragment again from what was read before "
          "it",
          "[devstudio][render-history][ru1b][negative-contrast]") {
    Rig rig;
    const std::string RootRemoved(64, 'e');
    const std::string FragmentDigest(64, 'c');
    const Json remove{{"base_source_digest", RootA}, {"fragment_reference", Fragment}};
    const auto fragment = document(Fragment, "feature", FragmentDigest, "fragment bytes");

    SECTION("the documents the composition writes list the fragment") {
        rig.edit("remove_authored_pass", remove,
                 Json::array({document(Root, "root", RootA, "root bytes"), fragment}),
                 committed({{Fragment, nullptr}, {Root, RootRemoved}}));
    }
    SECTION("only the read by name has it") {
        rig.edit("remove_authored_pass", remove, rootBefore(),
                 committed({{Fragment, nullptr}, {Root, RootRemoved}}), Json(),
                 Json{{"documents", Json::array({fragment})}});
    }
    REQUIRE(rig.history.undoStack().size() == 1);
    CHECK(rig.history.undoLabel() == "Remove pass " + Fragment);
    CHECK_FALSE(rig.history.undoStack().back().barrier);
    // The fragment is not there now: only the root is read.
    const auto documents = rig.restore(
        true, Json::array({document(Root, "root", RootRemoved, "root without the pass")}),
        committed({{Fragment, FragmentDigest}, {Root, RootA}}));
    CHECK(documents == Json::array({restored(Root, RootRemoved, "root bytes"),
                                    restored(Fragment, nullptr, "fragment bytes")}));
    // Redo deletes it again, under the digest Undo wrote.
    const auto redone = rig.restore(
        false,
        Json::array({document(Root, "root", RootA, "root bytes"), fragment}),
        committed({{Fragment, nullptr}, {Root, RootRemoved}}));
    CHECK(redone == Json::array({restored(Root, RootA, "root without the pass"),
                                 restored(Fragment, FragmentDigest, nullptr)}));
}

TEST_CASE("RU-1b fork, Make editable and a first render target are undone by deleting what they "
          "created",
          "[devstudio][render-history][ru1b]") {
    Rig rig;
    const std::string RootForked(64, 'f');
    const std::string Created(64, '9');
    std::string reference;
    SECTION("fork_render_pipeline writes the project preset") {
        reference = Preset;
        rig.edit("fork_render_pipeline", Json{{"base_source_digest", RootA}}, rootBefore(),
                 committed({{Root, RootForked}, {Preset, Created}}));
        CHECK(rig.history.undoLabel() == "Fork render pipeline");
    }
    SECTION("Make editable writes the feature") {
        reference = Feature;
        rig.edit("edit_render_features",
                 Json{{"ops", Json::array({{{"op", "materialize"}, {"scope", "root"}}})}},
                 rootBefore(), committed({{Root, RootForked}, {Feature, Created}}));
        CHECK(rig.history.undoLabel() == "Make feature editable");
    }
    SECTION("import_render_feature writes the feature") {
        reference = Feature;
        rig.edit("import_render_feature", Json{{"reference", "engine://features/bloom"}},
                 rootBefore(), committed({{Root, RootForked}, {Feature, Created}}));
        CHECK(rig.history.undoLabel() == "Import feature");
    }
    SECTION("an edit whose commit wrote a document that was not there before") {
        reference = Feature;
        rig.edit("edit_render_targets",
                 Json{{"ops", Json::array({{{"op", "add"}, {"target", {{"name", "hdr"}}}}})}},
                 rootBefore(), committed({{Root, RootForked}, {Feature, Created}}));
        CHECK(rig.history.undoLabel() == "Add render target hdr");
    }
    REQUIRE(rig.history.undoStack().size() == 1);
    CHECK_FALSE(rig.history.undoStack().back().barrier);
    auto current = Json::array({document(Root, "root", RootForked, "root after"),
                                document(reference, "preset", Created, "created bytes")});
    const auto documents =
        rig.restore(true, current, committed({{Root, RootA}, {reference, nullptr}}));
    // In reference order.
    auto expected = Json::array({restored(Root, RootForked, "root bytes"),
                                 restored(reference, Created, nullptr)});
    if (reference < Root) std::swap(expected[0], expected[1]);
    CHECK(documents == expected);
    CHECK(rig.history.redoStack().size() == 1);
}

TEST_CASE("RU-1b an edit whose commit erased a document is undone by writing it again",
          "[devstudio][render-history][ru1b]") {
    Rig rig;
    rig.edit("edit_render_features",
             Json{{"ops", Json::array({{{"op", "remove"}, {"feature", "bloom"}}})}}, bothBefore(),
             committed({{Preset, nullptr}}));
    CHECK_FALSE(rig.history.undoStack().back().barrier);
    const auto documents = rig.restore(true, Json::array(), committed({{Preset, PresetBefore}}));
    CHECK(documents == Json::array({restored(Preset, nullptr, "preset before")}));
}

TEST_CASE("RU-1b Undo stops where the documents could not be read, or where the engine does not "
          "create or delete them",
          "[devstudio][render-history][ru1b]") {
    Rig rig;
    recordPresetEdit(rig);
    SECTION("the read before the edit was not answered") {
        rig.edit("add_authored_pass", AddPass, Json(),
                 committed({{Fragment, std::string(64, 'c')}, {Root, std::string(64, 'b')}}));
        REQUIRE(rig.history.undoStack().size() == 2);
        CHECK(rig.history.undoStack().back().barrier);
        const auto before = rig.sent.size();
        CHECK_FALSE(rig.history.undo());
        CHECK(rig.sent.size() == before);
        CHECK(rig.history.line() ==
              "Cannot undo past \"Add pass tint\": its documents could not be read before it.");
    }
    SECTION("what it removed was read by neither") {
        rig.edit("remove_authored_pass",
                 Json{{"base_source_digest", RootA}, {"fragment_reference", Fragment}},
                 rootBefore(), committed({{Fragment, nullptr}, {Root, std::string(64, 'e')}}));
        CHECK(rig.history.undoStack().back().barrier);
        CHECK_FALSE(rig.history.undo());
        CHECK(rig.history.line() == "Cannot undo past \"Remove pass " + Fragment +
                                        "\": what it removed could not be read before it.");
    }
    SECTION("reserved_location: one line, and the entry stays where Undo stops") {
        const std::string Created(64, '9');
        const std::string Reserved = "project://assets/tint.json";
        rig.edit("add_authored_pass", AddPass, rootBefore(),
                 committed({{Root, std::string(64, 'b')}, {Reserved, Created}}));
        REQUIRE(rig.history.undo());
        rig.answer(rig.last().id,
                   Json{{"documents",
                         Json::array({document(Root, "root", std::string(64, 'b'), "r"),
                                      document(Reserved, "feature", Created, "f")})}});
        REQUIRE(rig.last().method == "restore_render_documents");
        rig.answer(rig.last().id,
                   Json{{"status", "rejected"},
                        {"committed", false},
                        {"error",
                         {{"code", "render_document_not_writable"},
                          {"message", "Render document is under assets/"},
                          {"method", "restore_render_documents"},
                          {"reference", Reserved},
                          {"reason", "reserved_location"},
                          {"location", "assets/"}}}});
        const std::string reason = "it would create or delete " + Reserved +
                                   " under assets/, where the project keeps documents that are "
                                   "not render documents";
        CHECK(rig.history.line() == "\"Add pass tint\" cannot be undone: " + reason + ".");
        REQUIRE(rig.history.undoStack().size() == 2);
        CHECK(rig.history.undoStack().back().barrier);
        CHECK(rig.history.undoStack().back().barrier_reason == reason);
        const auto before = rig.sent.size();
        CHECK_FALSE(rig.history.undo());
        CHECK(rig.sent.size() == before);
        CHECK(rig.history.line() == "Cannot undo past \"Add pass tint\": " + reason + ".");
    }
    SECTION("root_document") {
        rig.edit("add_authored_pass", AddPass, rootBefore(),
                 committed({{Root, std::string(64, 'b')}}));
        REQUIRE(rig.history.undo());
        rig.answer(rig.last().id, Json{{"documents", rootBefore()}});
        rig.answer(rig.last().id, Json{{"status", "rejected"},
                                       {"committed", false},
                                       {"error",
                                        {{"code", "render_document_not_writable"},
                                         {"reference", Root},
                                         {"reason", "root_document"}}}});
        CHECK(rig.history.line() ==
              "\"Add pass tint\" cannot be undone: it would delete the root " + Root + ".");
        CHECK(rig.history.undoStack().back().barrier);
    }
}

namespace {

// Decision 3's roots. The edit changed a node; the Frame Plan then moved a
// pass (layout.frame_plan) and the Graph tab a definition's node. The
// rendering section's odd spacing shows the restored bytes are the earlier
// root's, not a rewrite of them.
const std::string RootEarlier = R"({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {
    "paint": {"nodes": [], "outputs": {}, "layout": {"nodes": {}}}
  },
  "nodes": [],
  "outputs": {},
  "layout": {"nodes": {}, "frame_plan": {"main_render": {"nodes": {"present": {"x": 0, "y": 0}}}}},
  "rendering": {"features": [],   "material_route_default":   "forward"}
}
)";
const std::string RootEdited = R"({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {
    "paint": {"nodes": [], "outputs": {}, "layout": {"nodes": {}}}
  },
  "nodes": [{"id": "tint", "op": "render.fullscreen"}],
  "outputs": {},
  "layout": {"nodes": {"tint": {"x": 5, "y": 5}}, "frame_plan": {"main_render": {"nodes": {"present": {"x": 0, "y": 0}}}}},
  "rendering": {"features": [], "material_route_default": "forward"}
}
)";
const std::string RootMoved = R"({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {
    "paint": {"nodes": [], "outputs": {}, "layout": {"nodes": {"q": {"x": 1, "y": 2}}}}
  },
  "nodes": [{"id": "tint", "op": "render.fullscreen"}],
  "outputs": {},
  "layout": {"nodes": {"tint": {"x": 5, "y": 5}}, "frame_plan": {"main_render": {"nodes": {"present": {"x": 40, "y": 60}}}}},
  "rendering": {"features": [], "material_route_default": "forward"}
}
)";
const std::string DigestEarlier(64, '4');
const std::string DigestEdited(64, '5');
const std::string DigestMoved(64, '6');

void recordRootEdit(Rig &rig, const Json &after_documents) {
    rig.edit("decompose_material_pass", Json{{"pass", "forward_opaque"}},
             Json::array({document(Root, "root", DigestEarlier, RootEarlier)}),
             committed({{Root, DigestEdited}}), after_documents);
    REQUIRE(rig.history.undoStack().size() == 1);
}

} // namespace

TEST_CASE("RU-1b a root that changed since only in placement is undone on its digest now, "
          "with the placement it has now",
          "[devstudio][render-history][ru1b][layout][negative-contrast]") {
    CHECK(RenderEditHistory::differsOnlyInLayout(RootEdited, RootMoved));
    CHECK_FALSE(RenderEditHistory::differsOnlyInLayout(RootEarlier, RootEdited));
    CHECK_FALSE(RenderEditHistory::differsOnlyInLayout(RootEdited, "not json"));

    Rig rig;
    recordRootEdit(rig, Json::array({document(Root, "root", DigestEdited, RootEdited)}));
    REQUIRE(rig.history.undoStack().back().written.at(Root)->content == RootEdited);
    // The Frame Plan moved a pass: the root's digest is not the one the edit
    // wrote.
    const auto documents = rig.restore(
        true, Json::array({document(Root, "root", DigestMoved, RootMoved)}),
        committed({{Root, std::string(64, '7')}}));
    REQUIRE(documents.size() == 1);
    // On the digest the root has now, so CAS still holds the whole document.
    CHECK(documents[0].at("base_digest") == DigestMoved);
    const auto content = documents[0].at("content").get<std::string>();
    // The earlier root, with the placement moved since: the frame plan and
    // the definition's. The root's node placement the edit itself made (and
    // nothing moved since) is the earlier one, so the tint node it drew is
    // not left placed after it is gone.
    auto expected = Json::parse(RootEarlier);
    expected["layout"]["frame_plan"] = Json::parse(RootMoved).at("layout").at("frame_plan");
    expected["definitions"]["paint"]["layout"] =
        Json::parse(RootMoved).at("definitions").at("paint").at("layout");
    CHECK(Json::parse(content) == expected);
    CHECK(RenderEditHistory::differsOnlyInLayout(content, RootEarlier));
    CHECK(content.find(R"("rendering": {"features": [],   "material_route_default":   "forward"})") !=
          std::string::npos);
    CHECK(rig.history.line() == "Undid \"Decompose forward_opaque\".");

    // Redo the same way: the content Undo sent is what it wrote; moved again
    // since, Redo puts the edited root back with that placement.
    const std::string DigestUndone(64, '7');
    auto moved_again = Json::parse(content);
    moved_again["layout"]["frame_plan"]["main_render"]["nodes"]["present"] = {{"x", 90},
                                                                              {"y", 10}};
    const auto redone = rig.restore(
        false,
        Json::array({document(Root, "root", std::string(64, '8'), moved_again.dump(2))}),
        committed({{Root, DigestMoved}}));
    REQUIRE(redone.size() == 1);
    CHECK(redone[0].at("base_digest") == std::string(64, '8'));
    auto expected_redo = Json::parse(RootMoved);
    expected_redo["layout"]["frame_plan"] = moved_again.at("layout").at("frame_plan");
    CHECK(Json::parse(redone[0].at("content").get<std::string>()) == expected_redo);
}

TEST_CASE("RU-1b a root that changed since in more than placement is refused as before",
          "[devstudio][render-history][ru1b][layout][negative-contrast]") {
    Rig rig;
    SECTION("a node changed as well") {
        recordRootEdit(rig, Json::array({document(Root, "root", DigestEdited, RootEdited)}));
        auto changed = Json::parse(RootMoved);
        changed["nodes"][0]["label"] = "renamed";
        REQUIRE(rig.history.undo());
        rig.answer(rig.last().id,
                   Json{{"documents",
                         Json::array({document(Root, "root", DigestMoved, changed.dump(2))})}});
        REQUIRE(rig.last().method == "restore_render_documents");
        // The earlier root, on the digest the edit wrote: CAS refuses it.
        CHECK(rig.last().params.at("documents") ==
              Json::array({restored(Root, DigestEdited, RootEarlier)}));
    }
    SECTION("what the commit wrote was not read (another edit landed first)") {
        recordRootEdit(rig, Json::array({document(Root, "root", DigestMoved, RootMoved)}));
        CHECK_FALSE(rig.history.undoStack().back().written.at(Root)->content.has_value());
        REQUIRE(rig.history.undo());
        rig.answer(rig.last().id,
                   Json{{"documents", Json::array({document(Root, "root", DigestMoved,
                                                            RootMoved)})}});
        CHECK(rig.last().params.at("documents") ==
              Json::array({restored(Root, DigestEdited, RootEarlier)}));
    }
    SECTION("a preset changed in the same way is not the root") {
        rig.edit("edit_preset_pass", PresetEdit, bothBefore(), committed({{Preset, PresetAfter}}),
                 Json::array({document(Preset, "preset", PresetAfter, RootEdited)}));
        REQUIRE(rig.history.undo());
        rig.answer(rig.last().id,
                   Json{{"documents", Json::array({document(Preset, "preset", PresetAgain,
                                                            RootMoved)})}});
        CHECK(rig.last().params.at("documents") ==
              Json::array({restored(Preset, PresetAfter, "preset before")}));
    }
    rig.answer(rig.last().id, Json{{"status", "rejected"},
                                   {"committed", false},
                                   {"error",
                                    {{"code", "external_modification"},
                                     {"reference", rig.last().params.at("documents")[0].at(
                                                       "reference")}}}});
    CHECK(rig.history.line().find("changed since. Undo the later edit first") !=
          std::string::npos);
    CHECK(rig.history.undoStack().size() == 1);
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
        CHECK_FALSE(rig.history.undoStack().back().barrier);
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

// --- LP-4f2: apply_graph_template (GT-1) is a render edit like the others.

TEST_CASE("LP-4f2 apply_graph_template is recorded as Replace <feature> with <title>, and Undo "
          "restores the documents it wrote",
          "[devstudio][render-history][lp4f2][negative-contrast]") {
    Rig rig;
    const std::string Bloom = "engine://graphs/bloom_loop.json";
    const std::string RootAfter(64, 'e');
    CHECK(RenderEditHistory::recordsMethod("apply_graph_template"));
    const auto before = rig.sent.size();
    const auto panel = rig.panelSends(
        "apply_graph_template", Json{{"base_source_digest", RootA}, {"template", Bloom}});
    // The documents are read before it is on the wire, as for every render edit.
    REQUIRE(rig.sent.size() == before + 1);
    CHECK(rig.sent[before].method == "get_render_documents");
    CHECK(rig.sent[before].params == Json::object());
    rig.answer(rig.sent[before].id, Json{{"documents", bothBefore()}});
    // Accepted, with what it applied: the feature is named there.
    rig.answer(panel, Json{{"status", "accepted"},
                           {"ticket", "t-gt"},
                           {"applied",
                            {{"template", Bloom},
                             {"feature", "engine://features/bloom.json"},
                             {"nodes", Json::array({"bloom_down"})}}}});
    REQUIRE(rig.last().method == "get_edit_result");
    rig.answer(rig.last().id, committed({{Root, RootAfter}}));
    REQUIRE(rig.last().method == "get_render_documents");
    rig.answer(rig.last().id,
               Json{{"documents", Json::array({document(Root, "root", RootAfter, "root after")})}});
    REQUIRE(rig.history.undoStack().size() == 1);
    CHECK(rig.history.undoLabel() == "Replace bloom with Bloom loop");

    // Undo: the root before it goes back, under the digest the commit named.
    REQUIRE(rig.history.undo());
    REQUIRE(rig.last().method == "get_render_documents");
    rig.answer(rig.last().id,
               Json{{"documents", Json::array({document(Root, "root", RootAfter, "root after")})}});
    REQUIRE(rig.last().method == "restore_render_documents");
    CHECK(rig.last().params.at("documents") ==
          Json::array({restored(Root, RootAfter, "root bytes")}));
    rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "undo-gt"}});
    rig.answer(rig.last().id, committed({{Root, RootA}}));
    CHECK(rig.restores_committed == 1);
    CHECK(rig.history.redoLabel() == "Replace bloom with Bloom loop");
}

// UX-15b, decision 3: the Frame Plan's pin and frozen plan (EL-2) are
// render edits like any other: recorded, named by what they did, undone.
TEST_CASE("UX-15b a backend pin and a frozen physical plan are recorded, named, and undone",
          "[devstudio][render-history][ux15b]") {
    Rig rig;
    const std::string RootAfter(64, 'f');
    const Json pin{{"schema", "pelican.vulkan_target_plan_pins"},
                   {"version", 1},
                   {"graph", "main_render"},
                   {"logical_graph_fingerprint", "fnv1a64:0000000000000001"},
                   {"pins", {{"backend_candidate", "pelican.vulkan.transient_plan@1"}}}};
    const auto before = rig.sent.size();
    const auto panel = rig.panelSends(
        "edit_vulkan_plan_pins", Json{{"base_source_digest", RootA}, {"family", "flat"}, {"pin", pin}});
    REQUIRE(rig.sent.size() == before + 1);
    CHECK(rig.sent[before].method == "get_render_documents");
    rig.answer(rig.sent[before].id, Json{{"documents", bothBefore()}});
    rig.answer(panel, Json{{"status", "accepted"}, {"ticket", "t-pin"}});
    REQUIRE(rig.last().method == "get_edit_result");
    rig.answer(rig.last().id, committed({{Root, RootAfter}}));
    REQUIRE(rig.last().method == "get_render_documents");
    rig.answer(rig.last().id,
               Json{{"documents", Json::array({document(Root, "root", RootAfter, "root after")})}});
    REQUIRE(rig.history.undoStack().size() == 1);
    CHECK(rig.history.undoLabel() == "Pin backend pelican.vulkan.transient_plan@1");
    REQUIRE(rig.history.undo());
    REQUIRE(rig.last().method == "get_render_documents");
    rig.answer(rig.last().id,
               Json{{"documents", Json::array({document(Root, "root", RootAfter, "root after")})}});
    REQUIRE(rig.last().method == "restore_render_documents");
    CHECK(rig.last().params.at("documents") ==
          Json::array({restored(Root, RootAfter, "root bytes")}));

    // The other three gestures, by the package they carry.
    CHECK(RenderEditHistory::labelFor("edit_vulkan_plan_pins",
                                      Json{{"pin", nullptr}, {"graph", "main_render"}}) ==
          "Unpin backend");
    CHECK(RenderEditHistory::recordsMethod("put_vulkan_physical_fragment"));
    CHECK(RenderEditHistory::labelFor("put_vulkan_physical_fragment",
                                      Json{{"fragment", {{"graph", "main_render"}}}}) ==
          "Freeze physical plan");
    CHECK(RenderEditHistory::labelFor("put_vulkan_physical_fragment",
                                      Json{{"fragment", nullptr}, {"graph", "main_render"}}) ==
          "Remove frozen physical plan");
}

TEST_CASE("LP-4f2 the entry is named for the template until the engine names the feature",
          "[devstudio][render-history][lp4f2]") {
    CHECK(RenderEditHistory::labelFor(
              "apply_graph_template",
              Json{{"template", "engine://graphs/shadow_cascade_loop.json"}}) ==
          "Replace with Shadow cascade loop");
    CHECK(RenderEditHistory::graphTemplateLabel(
              Json{{"applied",
                    {{"template", "engine://graphs/shadow_cascade_loop.json"},
                     {"feature", "engine://features/shadow_directional.json"}}}}) ==
          "Replace shadow_directional with Shadow cascade loop");
    CHECK(RenderEditHistory::graphTemplateLabel(Json{{"status", "accepted"}}).empty());
    CHECK(RenderEditHistory::graphTemplateTitle("engine://graphs/planar_mip_chain_loop.json") ==
          "Planar mip chain loop");
}

// RTL-2s-c: a ray type renamed in the Ray layout section rewrites the
// shaders that name it (PELICAN_RAY_TYPE_<name>) beside the documents. The
// files ride on the edit's entry: Undo writes them back before the restore
// is sent (the restore's commit compiles them), Redo writes them again, and
// a request the history does not record drops them.
TEST_CASE("RTL-2s-c a rename's shaders ride on its history entry: Undo writes them back before the "
          "restore, Redo writes them again",
          "[devstudio][render-history][rtl2s]") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_rtl2sc_h_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto raygen = root / "trace.rgen";
    const auto read = [&] {
        std::ifstream file{raygen, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    };
    const auto put = [&](const std::string &text) { std::ofstream{raygen, std::ios::binary} << text; };
    const auto made = root / "made.rchit";
    put("PELICAN_RAY_TYPE_sun");
    std::ofstream{made, std::ios::binary} << "new";
    Rig rig;
    // A request the history does not record drops what was handed over.
    rig.history.attachFiles({{raygen.string(), "dropped", "dropped"}});
    rig.history.beforeRequest("get_ray_layout", Json::object());
    rig.history.attachFiles({{raygen.string(), std::string{"PELICAN_RAY_TYPE_shadow"}, "PELICAN_RAY_TYPE_sun"},
                             {made.string(), std::nullopt, "new"}});
    rig.edit("edit_batch", Json{{"base_source_digest", RootA}, {"edits", Json::array()}}, rootBefore(),
             committed({{Root, std::string(64, 'e')}}));
    REQUIRE(rig.history.undoStack().size() == 1);
    REQUIRE(rig.history.undoStack().back().files.size() == 2);
    // Undo: nothing written while the documents are read ...
    REQUIRE(rig.history.undo());
    CHECK(read() == "PELICAN_RAY_TYPE_sun");
    rig.answer(rig.last().id, Json{{"documents", Json::array({document(Root, "root", std::string(64, 'e'), "now")})}});
    // ... and the shader back (the new file gone) as the restore is sent.
    REQUIRE(rig.last().method == "restore_render_documents");
    CHECK(read() == "PELICAN_RAY_TYPE_shadow");
    CHECK_FALSE(std::filesystem::exists(made));
    rig.answer(rig.last().id, Json{{"status", "accepted"}, {"ticket", "undo"}});
    rig.answer(rig.last().id, committed({{Root, RootA}}));
    // Redo writes them again.
    REQUIRE(rig.history.redo());
    rig.answer(rig.last().id, Json{{"documents", Json::array({document(Root, "root", RootA, "root bytes")})}});
    REQUIRE(rig.last().method == "restore_render_documents");
    CHECK(read() == "PELICAN_RAY_TYPE_sun");
    CHECK(std::filesystem::exists(made));
    // A refused restore puts the files back as the documents still are.
    rig.answer(rig.last().id, Json{{"status", "rejected"}, {"error", {{"code", "external_modification"}}}});
    CHECK(read() == "PELICAN_RAY_TYPE_shadow");
    CHECK_FALSE(std::filesystem::exists(made));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("RTL-3s decision 3: an edit that rewrote procedural records is recorded -- Undo restores its asset data "
          "by digest with the layout, Redo takes it forward, and a restore the engine cannot make after a restart "
          "stops there",
          "[devstudio][render-history][rtl3s]") {
    Rig rig;
    const std::string Layout = "project://passes/ray_layout.json";
    const std::string Assets = "project://assets/asset_data.json";
    const std::string L1(64, '1'), L2(64, '2'), A1(64, 'b'), A2(64, 'c'), A3(64, 'd');
    const Json params{{"base_source_digest", RootA},
                      {"layout", Json::object()},
                      {"procedural_records", {{"ball", {{"value", 1.0}}}}}};
    const auto before = rig.sent.size();
    const auto panel = rig.panelSends("edit_ray_layout", params);
    REQUIRE(rig.sent.size() == before + 1);
    rig.answer(rig.sent[before].id,
               Json{{"documents", Json::array({document(Root, "root", RootA, "root bytes"),
                                               document(Layout, "layout", L1, "layout before")})}});
    rig.answer(panel, committed({{Layout, L2}, {Assets, A2}}));
    // The asset data is no render document: not read after the commit.
    REQUIRE(rig.last().method == "get_render_documents");
    CHECK(rig.last().params == Json{{"references", Json::array({Layout})}});
    rig.answer(rig.last().id, Json{{"documents", Json::array({document(Layout, "layout", L2, "layout after")})}});
    REQUIRE(rig.history.undoStack().size() == 1);
    CHECK(rig.history.undoStack().back().records_only == std::set<std::string>{Assets});
    // Undo: the asset data by the digest the commit wrote, content null.
    const auto undone = rig.restore(true, Json::array({document(Layout, "layout", L2, "layout after")}),
                                    committed({{Layout, L1}, {Assets, A1}}));
    CHECK(undone == Json::array({restored(Assets, A2, nullptr), restored(Layout, L2, "layout before")}));
    // Redo: forward again from the digest the Undo answered.
    const auto redone = rig.restore(false, Json::array({document(Layout, "layout", L1, "layout before")}),
                                    committed({{Layout, L2}, {Assets, A3}}));
    CHECK(redone == Json::array({restored(Assets, A1, nullptr), restored(Layout, L1, "layout after")}));
    // After the engine started again it no longer knows the digest: one
    // sentence, and Undo stops at the entry.
    REQUIRE(rig.history.undo());
    rig.answer(rig.last().id, Json{{"documents", Json::array({document(Layout, "layout", L2, "layout after")})}});
    REQUIRE(rig.last().method == "restore_render_documents");
    rig.answer(rig.last().id, Json{{"status", "rejected"},
                                   {"error", {{"code", "render_document_not_writable"},
                                              {"reference", Assets},
                                              {"reason", "procedural_records_only"}}}});
    CHECK(rig.history.line().find("keeps the procedural records an edit replaced only while it runs") !=
          std::string::npos);
    CHECK(rig.history.undoStack().back().barrier);
}

TEST_CASE("Procedural record Undo retains each inverse across repeated document digests",
          "[devstudio][render-history][procedural-history]") {
    Rig rig;
    const std::string assets = "project://assets/asset_data.json";
    const std::string a0(64, '0'), a1(64, '1'), a2(64, '2');
    const auto records = [](int value) { return Json{{"ball", {{"value", value}}}}; };
    const auto commit = [&](const std::string &digest, int previous) {
        auto result = committed({{assets, digest}});
        result["procedural_records_before"] = {{assets, records(previous).dump()}};
        return result;
    };
    const auto edit = [&](const std::string &digest, int previous, int next) {
        const auto before = rig.sent.size();
        const auto panel = rig.panelSends("edit_ray_layout",
                                         {{"procedural_records", records(next)}});
        rig.answer(rig.sent[before].id, {{"documents", Json::array()}});
        rig.answer(panel, commit(digest, previous));
    };
    edit(a1, 0, 1);
    edit(a2, 1, 2);
    const auto restore = [&](bool undo, const std::string &base, int target,
                             const std::string &written, int previous) {
        auto expected = restored(assets, base, nullptr);
        expected["procedural_records"] = records(target).dump();
        CHECK(rig.restore(undo, Json::array(), commit(written, previous)) == Json::array({expected}));
    };
    restore(true, a2, 1, a1, 2);
    // The engine just produced a1 again; the earlier entry still restores 0.
    rig.history.abandonInFlight(); // a same-project engine restart preserves the inverse
    restore(true, a1, 0, a0, 1);
    restore(false, a0, 1, a1, 0);
    restore(false, a1, 2, a2, 1);
}

TEST_CASE("RTL-3s decision 4: a raygen switch Studio wrote is an entry of its own -- Undo writes the file back "
          "and Redo writes it again, with no restore sent",
          "[devstudio][render-history][rtl3s]") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_rtl3s_h_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto raygen = root / "start.rgen";
    const auto read = [&] {
        std::ifstream file{raygen, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    };
    std::ofstream{raygen, std::ios::binary} << "#define TRACE_SECOND_RAY_TYPE 1";
    Rig rig;
    rig.history.recordFiles("TRACE_SECOND_RAY_TYPE on in start.rgen",
                            {{raygen.string(), std::string{"#define TRACE_SECOND_RAY_TYPE 0"},
                              "#define TRACE_SECOND_RAY_TYPE 1"}});
    REQUIRE(rig.history.canUndo());
    CHECK(rig.history.undoLabel() == "TRACE_SECOND_RAY_TYPE on in start.rgen");
    const auto sent = rig.sent.size();
    REQUIRE(rig.history.undo());
    CHECK(read() == "#define TRACE_SECOND_RAY_TYPE 0");
    CHECK(rig.sent.size() == sent);
    CHECK(rig.history.canRedo());
    REQUIRE(rig.history.redo());
    CHECK(read() == "#define TRACE_SECOND_RAY_TYPE 1");
    CHECK(rig.sent.size() == sent);
    CHECK(rig.history.line() == "Redid \"TRACE_SECOND_RAY_TYPE on in start.rgen\".");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace PelicanStudio
