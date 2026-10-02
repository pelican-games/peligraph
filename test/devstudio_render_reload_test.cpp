// RL-1s: Studio picks up a render document rewritten outside it, through
// RL-1's reload_render_documents. No Qt and no engine: the transport is a list
// of what was sent, the files are a map of digests, and the test answers each
// request as the engine does.
//
// One case per decision of the spec: what is watched (1), a write of Studio's
// own asks nothing (2), anything else asks once and waits out reload_busy (3),
// what each answer says and whom it tells (4), and an edit refused while the
// file stays broken reads as the disk's sentence (5).

#include "inspectormodel.hpp"
#include "renderdocumentreload.hpp"
#include "renderedithistory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using Notice = RenderDocumentReload::Notice;

const std::filesystem::path Project{"project_root"};
const std::string Root = "passes/main_rendering_config.json";
const std::string Fragment = "project://passes/authoring/pass-0123.json";
const std::string RootA(64, 'a');
const std::string RootB(64, 'b');
const std::string RootC(64, 'c');
const std::string FragmentA(64, '1');

struct Sent {
    std::int64_t id = 0;
    std::string method;
    Json params;
};

struct Scheduled {
    int milliseconds = 0;
    std::function<void()> task;
};

struct Rig {
    std::vector<Sent> sent;
    std::vector<Scheduled> scheduled;
    std::map<std::filesystem::path, std::string> disk;
    std::vector<Notice> notices;
    std::vector<Json> reloads;
    int watch_changes = 0;
    std::int64_t next_id = 1;
    RenderDocumentReload reload{RenderDocumentReload::Transport{
        .request = [this](const std::string &method, const Json &params, std::string *) {
            sent.push_back(Sent{next_id, method, params});
            return next_id++;
        },
        .schedule = [this](int milliseconds, std::function<void()> task) {
            scheduled.push_back(Scheduled{milliseconds, std::move(task)});
        },
        .file_digest = [this](const std::filesystem::path &path) -> std::optional<std::string> {
            const auto found = disk.find(path);
            if (found == disk.end()) return std::nullopt;
            return found->second;
        },
    }};

    Rig() {
        reload.watchChanged = [this] { ++watch_changes; };
        reload.notice = [this](const Notice &notice) { notices.push_back(notice); };
        reload.reloaded = [this](const Json &commit) { reloads.push_back(commit); };
        setRenderDocumentsRefusedOnDisk(false);
    }

    std::filesystem::path rootPath() const { return (Project / Root).lexically_normal(); }
    std::filesystem::path fragmentPath() const {
        return (Project / "passes/authoring/pass-0123.json").lexically_normal();
    }

    const Sent &last() const {
        REQUIRE_FALSE(sent.empty());
        return sent.back();
    }

    void answer(std::int64_t id, const Json &result) { CHECK(reload.receiveResult(id, result)); }

    void runScheduled() {
        auto tasks = std::move(scheduled);
        scheduled.clear();
        for (auto &task : tasks) task.task();
    }

    // A project open, and the engine's listing of what it composes.
    void opened() {
        disk[rootPath()] = RootA;
        disk[fragmentPath()] = FragmentA;
        reload.open(Project);
        reload.engineReady();
        REQUIRE(last().method == "get_render_documents");
        answer(last().id,
               Json{{"documents",
                     Json::array({
                         {{"reference", Root}, {"kind", "root"}, {"digest", RootA}, {"content", "{}"}},
                         {{"reference", Fragment},
                          {"kind", "feature"},
                          {"digest", FragmentA},
                          {"content", "{}"}},
                     })}});
    }

    // An editor's save: the watcher says so, and the debounce runs out.
    void saved(const std::filesystem::path &path, const std::string &digest) {
        disk[path] = digest;
        reload.fileChanged();
        REQUIRE(scheduled.size() == 1);
        CHECK(scheduled.front().milliseconds == RenderDocumentReload::ReloadDebounceMs);
        runScheduled();
    }

    // The reload's ticket, accepted, polled, and answered with `result`.
    void ticket(const Json &result) {
        REQUIRE(last().method == "reload_render_documents");
        CHECK(last().params == Json::object());
        answer(last().id, Json{{"ticket", "render-config-reload-1"},
                               {"status", "accepted"},
                               {"base_source_digest", {{"algorithm", "sha256"}, {"hex", RootA}}}});
        REQUIRE(scheduled.size() == 1);
        runScheduled();
        REQUIRE(last().method == "get_edit_result");
        CHECK(last().params == Json{{"ticket", "render-config-reload-1"}});
        answer(last().id, result);
    }

    static Json committed(const Json &documents, const Json &changed) {
        return Json{{"ticket", "render-config-reload-1"},
                    {"status", "committed"},
                    {"committed", true},
                    {"operation", "reload_render_documents"},
                    {"published_generation", 7},
                    {"source_commit_called", true},
                    {"source_digest", {{"algorithm", "sha256"}, {"hex", RootB}}},
                    {"documents", documents},
                    {"changed", changed}};
    }
};

TEST_CASE("RL-1s decision 1: the project's render documents are watched, engine:// is not",
          "[devstudio][render_reload]") {
    CHECK(RenderDocumentReload::documentPath(Project, "engine://features/bloom.json") ==
          std::nullopt);
    CHECK(RenderDocumentReload::documentPath(Project, "project://a.json#node") == std::nullopt);
    CHECK(RenderDocumentReload::documentPath(Project, "project://passes/a.json") ==
          (Project / "passes/a.json").lexically_normal());
    CHECK(RenderDocumentReload::documentPath(Project, "passes/main.json") ==
          (Project / "passes/main.json").lexically_normal());

    Rig rig;
    rig.opened();
    const auto files = rig.reload.watchedFiles();
    REQUIRE(files.size() == 2);
    CHECK(std::find(files.begin(), files.end(), rig.rootPath()) != files.end());
    CHECK(std::find(files.begin(), files.end(), rig.fragmentPath()) != files.end());
    CHECK(rig.watch_changes > 0);

    // Writes in a burst are one check, after the last of them.
    rig.disk[rig.rootPath()] = RootB;
    rig.reload.fileChanged();
    rig.reload.fileChanged();
    REQUIRE(rig.scheduled.size() == 2);
    const auto before = rig.sent.size();
    rig.runScheduled();
    CHECK(rig.sent.size() == before + 1);
    CHECK(rig.last().method == "reload_render_documents");
}

TEST_CASE("RL-1s decision 2: a file holding what Studio last committed asks nothing",
          "[devstudio][render_reload]") {
    Rig rig;
    rig.opened();
    // A Graph edit commits; its documents are what it wrote.
    rig.reload.observeReply(Json{{"ticket", "render-config-1"},
                                 {"status", "committed"},
                                 {"committed", true},
                                 {"documents", {{Root, RootC}}}});
    CHECK(rig.reload.documents().at(Root).digest == RootC);
    const auto before = rig.sent.size();
    rig.saved(rig.rootPath(), RootC);
    CHECK(rig.sent.size() == before);
    CHECK(rig.notices.empty());

    // A document an edit created is watched from its commit on.
    const std::string Created = "project://passes/authoring/pass-new.json";
    rig.reload.observeReply(Json{{"status", "committed"},
                                 {"committed", true},
                                 {"documents", {{Created, RootA}}}});
    CHECK(rig.reload.documents().contains(Created));
    rig.reload.observeReply(Json{{"status", "committed"},
                                 {"committed", true},
                                 {"documents", {{Created, nullptr}}}});
    CHECK_FALSE(rig.reload.documents().contains(Created));
}

TEST_CASE("RL-1s decision 3: a change asks once, and reload_busy asks again later",
          "[devstudio][render_reload]") {
    Rig rig;
    rig.opened();
    rig.saved(rig.rootPath(), RootB);
    REQUIRE(rig.last().method == "reload_render_documents");

    // Studio's own ticket is still pending: half a second later, again.
    rig.answer(rig.last().id,
               Json{{"status", "rejected"},
                    {"committed", false},
                    {"error",
                     {{"code", "reload_busy"},
                      {"reason", "ticket_pending"},
                      {"tickets", Json::array({"render-config-4"})},
                      {"message", "render documents cannot be reloaded while ..."}}}});
    CHECK(rig.notices.empty());
    REQUIRE(rig.scheduled.size() == 1);
    CHECK(rig.scheduled.front().milliseconds == RenderDocumentReload::BusyRetryMs);
    // A write meanwhile does not send a second request.
    rig.reload.fileChanged();
    rig.scheduled.back().task();
    rig.scheduled.pop_back();
    const auto before = rig.sent.size();
    rig.runScheduled();
    CHECK(rig.sent.size() == before + 1);
    REQUIRE(rig.last().method == "reload_render_documents");

    // A scene preview open: the notice says what to do, and it asks again.
    rig.answer(rig.last().id, Json{{"status", "rejected"},
                                   {"error", {{"code", "reload_busy"}, {"reason", "preview_open"}}}});
    REQUIRE(rig.notices.size() == 1);
    CHECK(rig.notices.back().kind == Notice::Kind::Information);
    CHECK(rig.notices.back().line ==
          "Close the scene preview to pick up the change to main_rendering_config.json.");
    REQUIRE(rig.scheduled.size() == 1);

    // The ticket that was pending committed what is on disk now: nothing to ask.
    rig.reload.observeReply(Json{{"status", "committed"},
                                 {"committed", true},
                                 {"documents", {{Root, RootB}}}});
    const auto settled = rig.sent.size();
    rig.runScheduled();
    CHECK(rig.sent.size() == settled);
    CHECK(rig.notices.back().kind == Notice::Kind::None);
}

TEST_CASE("RL-1s decision 4: committed, no_change, refused and restart_required",
          "[devstudio][render_reload]") {
    Rig rig;
    rig.opened();

    SECTION("committed: Picked up, and the owner reads the panels again") {
        rig.saved(rig.rootPath(), RootB);
        rig.ticket(Rig::committed({{Root, RootB}, {Fragment, FragmentA}}, Json::array({Root})));
        REQUIRE(rig.reloads.size() == 1);
        CHECK(rig.reloads.front().at("changed") == Json::array({Root}));
        REQUIRE(rig.notices.size() == 1);
        CHECK(rig.notices.back().kind == Notice::Kind::Success);
        CHECK(rig.notices.back().line == "Picked up the change to main_rendering_config.json.");
        CHECK(rig.reload.documents().at(Root).digest == RootB);
        CHECK_FALSE(rig.reload.reloadInFlight());
    }
    SECTION("no_change: nothing said, nobody told") {
        rig.reload.reloadNow();
        auto commit = Rig::committed({{Root, RootA}, {Fragment, FragmentA}}, Json::array());
        commit["no_change"] = true;
        commit["source_commit_called"] = false;
        rig.ticket(commit);
        CHECK(rig.reloads.empty());
        CHECK(rig.notices.empty());
    }
    SECTION("refused: which file, why, and that the engine keeps the previous one") {
        rig.disk[rig.rootPath()] = RootB;
        rig.reload.fileChanged();
        rig.runScheduled();
        rig.answer(rig.last().id,
                   Json{{"status", "rejected"},
                        {"committed", false},
                        {"error",
                         {{"code", "render_pipeline_preflight_failed"},
                          {"message", "[json.exception.parse_error.101] syntax error"},
                          {"participant", "pelican.render_pipeline"},
                          {"method", "reload_render_documents"},
                          {"reference", Root}}}});
        REQUIRE(rig.notices.size() == 1);
        const auto &said = rig.notices.back();
        CHECK(said.kind == Notice::Kind::Error);
        CHECK(said.line == "Could not load main_rendering_config.json: fix the file and save again.");
        CHECK(said.detail.find("The engine keeps the previous version") != std::string::npos);
        CHECK(said.detail.find("render_pipeline_preflight_failed (pelican.render_pipeline, " +
                               Root + "): [json.exception.parse_error.101]") !=
              std::string::npos);
        CHECK(rig.reload.refused());
        CHECK(rig.reloads.empty());
        // The digest it knew stays: the fixed file is asked about again.
        CHECK(rig.reload.documents().at(Root).digest == RootA);
        rig.saved(rig.rootPath(), RootC);
        rig.ticket(Rig::committed({{Root, RootC}, {Fragment, FragmentA}}, Json::array({Root})));
        CHECK_FALSE(rig.reload.refused());
        CHECK(rig.notices.back().line == "Picked up the change to main_rendering_config.json.");
    }
    SECTION("refused at run time: the ticket's own refusal") {
        rig.saved(rig.rootPath(), RootB);
        rig.ticket(Json{{"ticket", "render-config-reload-1"},
                        {"status", "rejected"},
                        {"error",
                         {{"code", "render_config_runtime_rejected"},
                          {"message", "shader project://shaders/missing was not found"}}}});
        REQUIRE_FALSE(rig.notices.empty());
        CHECK(rig.notices.back().line ==
              "Could not load main_rendering_config.json: fix the file and save again.");
        CHECK(rig.notices.back().detail.find("shader project://shaders/missing was not found") !=
              std::string::npos);
    }
    SECTION("restart_required: the owner hears the commit and its restart") {
        rig.saved(rig.rootPath(), RootB);
        auto commit = Rig::committed({{Root, RootB}}, Json::array({Root}));
        commit["restart_required"] = true;
        commit["restart_reason"] = {{"code", "material_physical_contract_changed"}};
        rig.ticket(commit);
        REQUIRE(rig.reloads.size() == 1);
        CHECK(rig.reloads.front().at("restart_reason").at("code") ==
              "material_physical_contract_changed");
    }
}

TEST_CASE("RL-1s decision 4: the render undo history is cut at a reload",
          "[devstudio][render_reload]") {
    std::vector<Sent> sent;
    std::int64_t next = 1;
    RenderEditHistory history{RenderEditHistory::Transport{
        .request = [&](const std::string &method, const Json &params, std::string *) {
            sent.push_back(Sent{next, method, params});
            return next++;
        },
        .schedule = [](int, std::function<void()>) {},
    }};
    // One edit the whole way, as RU-1's test drives it: the documents read
    // before it, accepted, committed, and what it wrote read after.
    const Json edit{{"base_source_digest", RootA}, {"graph", "main_render"}, {"pass_name", "present"}};
    history.beforeRequest("edit_preset_pass", edit);
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].method == "get_render_documents");
    history.requestSent(500, "edit_preset_pass", edit);
    CHECK(history.receiveResult(sent[0].id,
                                Json{{"documents",
                                      Json::array({{{"reference", Root},
                                                    {"kind", "root"},
                                                    {"digest", RootA},
                                                    {"content", "{\"a\":1}"}}})}}));
    CHECK(history.receiveResult(500, Json{{"ticket", "t1"}, {"status", "accepted"}}));
    REQUIRE(sent.back().method == "get_edit_result");
    CHECK(history.receiveResult(sent.back().id,
                                Json{{"ticket", "t1"},
                                     {"status", "committed"},
                                     {"committed", true},
                                     {"source_digest", {{"algorithm", "sha256"}, {"hex", RootB}}},
                                     {"documents", {{Root, RootB}}}}));
    if (sent.back().method == "get_render_documents")
        CHECK(history.receiveResult(sent.back().id,
                                    Json{{"documents",
                                          Json::array({{{"reference", Root},
                                                        {"kind", "root"},
                                                        {"digest", RootB},
                                                        {"content", "{\"a\":2}"}}})}}));
    REQUIRE(history.canUndo());

    history.cut("\xE2\x80\x94 reloaded from disk \xE2\x80\x94");
    CHECK_FALSE(history.canUndo());
    CHECK_FALSE(history.canRedo());
    CHECK(history.line() == "\xE2\x80\x94 reloaded from disk \xE2\x80\x94");
}

TEST_CASE("RL-1s decision 5: while the file stays refused, external_modification reads as "
          "the disk's sentence",
          "[devstudio][render_reload]") {
    const std::string engine = "render config source changed outside the editor";
    setRenderDocumentsRefusedOnDisk(false);
    // A panel that read before another's commit is told the same code; that
    // is not the disk's doing, and keeps the engine's words.
    CHECK(knownRefusalDescription("external_modification", engine).empty());

    Rig rig;
    rig.opened();
    rig.saved(rig.rootPath(), RootB);
    rig.answer(rig.last().id,
               Json{{"status", "rejected"},
                    {"error",
                     {{"code", "render_pipeline_preflight_failed"},
                      {"message", "syntax error"},
                      {"reference", Root}}}});
    REQUIRE(renderDocumentsRefusedOnDisk());
    const auto said = knownRefusalDescription("external_modification", engine);
    CHECK(said.starts_with(std::string{renderDiskRefusalSentence}));
    CHECK(said.find("external_modification: " + engine) != std::string::npos);

    // Reload from disk asks whatever the digests say.
    const auto before = rig.sent.size();
    rig.reload.reloadNow();
    CHECK(rig.sent.size() == before + 1);
    CHECK(rig.last().method == "reload_render_documents");
    rig.ticket(Rig::committed({{Root, RootB}, {Fragment, FragmentA}}, Json::array({Root})));
    CHECK_FALSE(renderDocumentsRefusedOnDisk());
    CHECK(knownRefusalDescription("external_modification", engine).empty());
}


TEST_CASE("IA-1s decision 5: the images' initial assets are watched; changed_assets says it was "
          "picked up, an engine without it that it is read on the next start",
          "[devstudio][render_reload][ia1s]") {
    Rig rig;
    rig.opened();
    const std::string Lut = "assets/images/ltc_1.ktx2";
    const auto lut = (Project / Lut).lexically_normal();
    const std::string LutA(64, 'd');
    const std::string LutB(64, 'e');
    rig.disk[lut] = LutA;
    const auto watched_before = rig.watch_changes;
    rig.reload.setInitialAssets({Lut});
    CHECK(rig.watch_changes > watched_before);
    auto files = rig.reload.watchedFiles();
    CHECK(files.size() == 3);
    CHECK(std::find(files.begin(), files.end(), lut) != files.end());
    // Nothing changed yet: a check asks nothing.
    const auto quiet = rig.sent.size();
    rig.saved(lut, LutA);
    CHECK(rig.sent.size() == quiet);
    const Json documents{{Root, RootA}, {Fragment, FragmentA}};

    SECTION("an engine before IA-2: no_change, and the notice says when it is read") {
        rig.saved(lut, LutB);
        auto commit = Rig::committed(documents, Json::array());
        commit["no_change"] = true;
        rig.ticket(commit);
        CHECK(rig.reloads.empty());
        REQUIRE_FALSE(rig.notices.empty());
        CHECK(rig.notices.back().kind == Notice::Kind::Information);
        CHECK(rig.notices.back().line ==
              "assets/images/ltc_1.ktx2 changed \xE2\x80\x94 the engine reads it on its next start.");
        // Seen: the same bytes again ask nothing.
        const auto before = rig.sent.size();
        rig.saved(lut, LutB);
        CHECK(rig.sent.size() == before);
    }
    SECTION("IA-2: the engine read it again") {
        rig.saved(lut, LutB);
        // As IA-2 answers an asset-only change: changed empty, no no_change.
        auto commit = Rig::committed(documents, Json::array());
        commit["changed_assets"] = Json::array({Lut});
        rig.ticket(commit);
        CHECK(rig.reloads.size() == 1);
        REQUIRE_FALSE(rig.notices.empty());
        CHECK(rig.notices.back().kind == Notice::Kind::Success);
        CHECK(rig.notices.back().line == "Picked up the change to assets/images/ltc_1.ktx2.");
        // Nothing changed since: IA-2 says no_change and an empty list.
        rig.reload.reloadNow();
        auto unchanged = Rig::committed(documents, Json::array());
        unchanged["no_change"] = true;
        unchanged["changed_assets"] = Json::array();
        rig.ticket(unchanged);
        CHECK(rig.reloads.size() == 1);
    }
    SECTION("IA-2: an asset that no longer fits its image is refused by name") {
        rig.saved(lut, LutB);
        rig.ticket(Json{{"ticket", "render-config-reload-1"},
                        {"status", "rejected"},
                        {"error",
                         {{"code", "graph.image_initial_mismatch@1"},
                          {"message", "graph.image_initial_mismatch@1"},
                          {"node", "render_image"},
                          {"asset", Lut},
                          {"what", "extent"},
                          {"expected", {{"width", 64}, {"height", 64}, {"format", nullptr}}},
                          {"actual", {{"width", 32}, {"height", 32}, {"format", "R32G32B32A32_SFLOAT"}}},
                          {"document", "project://passes/main_rendering_config.json"}}}});
        REQUIRE_FALSE(rig.notices.empty());
        CHECK(rig.notices.back().kind == Notice::Kind::Error);
        CHECK(rig.notices.back().line == "Could not load ltc_1.ktx2: fix the file and save again.");
        CHECK(rig.notices.back().detail.starts_with(
            "assets/images/ltc_1.ktx2 is 32\xC3\x97"
            "32 but render_image is 64\xC3\x97"
            "64"));
        CHECK(rig.reloads.empty());
    }
    // An image that no longer starts from it: no longer watched.
    rig.reload.setInitialAssets({});
    files = rig.reload.watchedFiles();
    CHECK(std::find(files.begin(), files.end(), lut) == files.end());
}
} // namespace
} // namespace PelicanStudio
