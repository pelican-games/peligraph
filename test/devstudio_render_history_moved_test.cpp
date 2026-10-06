#include "renderedithistory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

namespace {
using Json = nlohmann::json;
using History = PelicanStudio::RenderEditHistory;
using Event = std::tuple<std::uint64_t, History::Move, bool>;
const std::string Root = "project://passes/main.json";

struct HistoryRig {
    struct Request {
        std::int64_t id;
        std::string method;
        Json params;
    };
    std::vector<Request> requests;
    std::vector<Event> events;
    int restored = 0;
    std::string bytes = R"({"fold_groups":{"g":{"folded":{}}}})";
    std::string digest = std::string(64, 'a');
    History history{History::Transport{
        .request = [this](const std::string &method, const Json &params, std::string *) {
            const auto id = static_cast<std::int64_t>(requests.size() + 1);
            requests.push_back({id, method, params});
            return id;
        },
        .schedule = [](int, std::function<void()>) {},
    }};

    HistoryRig() {
        history.historyMoved = [this](std::uint64_t serial, History::Move move, bool ok) {
            // The notification follows the move, never just the send.
            const auto &stack = ok && move == History::Move::Undo ? history.redoStack()
                                : move == History::Move::Redo && !ok ? history.redoStack()
                                : history.undoStack();
            REQUIRE_FALSE(stack.empty());
            CHECK(stack.back().serial == serial);
            events.emplace_back(serial, move, ok);
        };
        history.restored = [this](const Json &, bool) { ++restored; };
    }
    Json snapshot() const {
        return {{"documents", Json::array({{{"reference", Root}, {"kind", "root"},
                                            {"digest", digest}, {"content", bytes}}})}};
    }
    Json commit() const {
        return {{"status", "committed"}, {"committed", true},
                {"source_digest", {{"hex", digest}}}, {"documents", {{Root, digest}}}};
    }
    void answer(const Json &reply) {
        REQUIRE_FALSE(requests.empty());
        const auto id = requests.back().id;
        REQUIRE(history.receiveResult(id, reply));
    }
    std::uint64_t record(const std::string &method = "edit_preset_pass", bool snapshot_ok = true) {
        history.beforeRequest(method, {{"group", "g"}});
        const auto snapshot_id = requests.back().id;
        history.requestSent(100000, method, {});
        if (snapshot_ok) REQUIRE(history.receiveResult(snapshot_id, snapshot()));
        else REQUIRE(history.receiveFailure(snapshot_id, "snapshot unavailable"));
        REQUIRE(history.receiveResult(100000, {{"status", "accepted"}, {"ticket", "edit"}}));
        bytes = R"({"fold_groups":{"g":{"folded":{"verified":{"digest":"fold"}}}}})";
        digest = std::string(64, 'b');
        answer(commit());
        if (snapshot_ok) answer(snapshot());
        REQUIRE(history.undoStack().size() == 1);
        return history.undoStack().back().serial;
    }
    Json restoreRequest(bool undoing) {
        const auto before = events.size();
        REQUIRE((undoing ? history.undo() : history.redo()));
        CHECK(events.size() == before);
        REQUIRE(requests.back().method == "get_render_documents");
        answer(snapshot());
        REQUIRE(requests.back().method == "restore_render_documents");
        return requests.back().params.at("documents");
    }
    void finishRestore() {
        const auto content = requests.back().params.at("documents")[0].at("content").get<std::string>();
        answer({{"status", "accepted"}, {"ticket", "restore"}});
        bytes = content;
        digest = std::string(64, digest[0] == 'b' ? 'c' : 'd');
        answer(commit());
    }
};
} // namespace

TEST_CASE("RT-D history movement reports RPC commits and refusals with a stable serial",
          "[devstudio][render-history][rtd]") {
    HistoryRig rig;
    const auto serial = rig.record();
    REQUIRE(rig.events == std::vector<Event>{{serial, History::Move::Push, true}});
    const auto before = rig.history.undoStack().back().before.at(Root)->content;
    auto undo = rig.restoreRequest(true);
    CHECK(undo[0].at("content") == before);
    CHECK(undo[0].at("base_digest") == std::string(64, 'b'));
    rig.answer({{"status", "rejected"}, {"error", {{"code", "external_modification"}, {"reference", Root}}}});
    CHECK(rig.events.back() == Event{serial, History::Move::Undo, false});
    CHECK(rig.history.redoStack().empty());
    CHECK(rig.restored == 0);
    // Retry, Redo, then Undo again all refer to the same entry.
    CHECK(rig.restoreRequest(true) == undo);
    rig.finishRestore();
    CHECK(rig.events.back() == Event{serial, History::Move::Undo, true});
    CHECK(rig.restored == 1);
    const auto redo = rig.restoreRequest(false);
    CHECK(redo[0].at("content") != before);
    rig.finishRestore();
    CHECK(rig.events.back() == Event{serial, History::Move::Redo, true});
    CHECK(rig.restored == 2);
    CHECK(rig.restoreRequest(true)[0].at("content") == before);
    rig.finishRestore();
    CHECK(rig.events.back() == Event{serial, History::Move::Undo, true});
    CHECK(rig.restored == 3);
}

TEST_CASE("RT-D history movement preserves a barrier and a failed restore",
          "[devstudio][render-history][rtd]") {
    HistoryRig rig;
    SECTION("barrier refuses synchronously") {
        const auto serial = rig.record("edit_preset_pass", false);
        const auto requests = rig.requests.size();
        REQUIRE_FALSE(rig.history.undo());
        CHECK(rig.requests.size() == requests);
        CHECK(rig.history.undoStack().back().serial == serial);
        CHECK(rig.events.back() == Event{serial, History::Move::Undo, false});
    }
    SECTION("a lost restore reply reports failure without moving its entry") {
        const auto serial = rig.record();
        rig.restoreRequest(true);
        REQUIRE(rig.history.receiveFailure(rig.requests.back().id, "connection lost"));
        CHECK(rig.history.undoStack().back().serial == serial);
        CHECK(rig.events.back() == Event{serial, History::Move::Undo, false});
        CHECK(rig.history.redoStack().empty());
        CHECK(rig.restored == 0);
    }
}

TEST_CASE("RT-D history movement covers file-only Undo and Redo without changing restored",
          "[devstudio][render-history][rtd]") {
    HistoryRig rig;
    const auto path = std::filesystem::temp_directory_path() /
                      ("pelican-rtd-history-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    { std::ofstream file{path, std::ios::binary}; file << "after"; }
    const auto read = [&] {
        std::ifstream file{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{file}, {}};
    };
    rig.history.recordFiles("Raygen switch", {{path.string(), "before", "after"}});
    REQUIRE(rig.history.undoStack().size() == 1);
    const auto serial = rig.history.undoStack().back().serial;
    REQUIRE(rig.events == std::vector<Event>{{serial, History::Move::Push, true}});
    REQUIRE(rig.history.undo());
    CHECK(read() == "before");
    CHECK(rig.events.back() == Event{serial, History::Move::Undo, true});
    REQUIRE(rig.history.redo());
    CHECK(read() == "after");
    CHECK(rig.events.back() == Event{serial, History::Move::Redo, true});
    REQUIRE(rig.history.undo());
    CHECK(read() == "before");
    CHECK(rig.events.back() == Event{serial, History::Move::Undo, true});
    CHECK(rig.requests.empty());
    CHECK(rig.restored == 0);
    rig.history.recordFiles("New switch", {{path.string(), "before", "new"}});
    CHECK(rig.history.redoStack().empty());
    CHECK(rig.history.undoStack().back().serial > serial);
}

TEST_CASE("RT-D UNMET1 verify_fold writes a recorded render entry",
          "[devstudio][render-history][rtd]") {
    HistoryRig rig;
    const auto before = rig.bytes;
    REQUIRE(History::recordsMethod("verify_fold"));
    const auto serial = rig.record("verify_fold");
    CHECK(rig.history.undoStack().back().label == "Verify g");
    CHECK(rig.history.undoStack().back().before.at(Root)->content == before);
    CHECK(rig.history.undoStack().back().written.at(Root)->content == rig.bytes);
    CHECK(rig.bytes != before);
    CHECK(rig.restoreRequest(true)[0].at("content") == before);
    rig.finishRestore();
    CHECK(rig.history.redoStack().back().serial == serial);
}
