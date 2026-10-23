// RT-C: a committed render edit's post_commit_error (the renderer's
// post-publication cleanup threw after the documents were written and the
// generation published). readRenderEditCompletion reads it from a committed
// answer only; the main window's line says it once per commit, with the
// restart line when the commit also asks for a restart. No Qt, no engine.

#include "rendereditcompletion.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr const char *CleanupError = "render target history reset failed";

Json committed(std::string ticket = "render-config-edit-1") {
    return Json{{"ticket", std::move(ticket)},
                {"status", "committed"},
                {"committed", true},
                {"published_generation", 7u},
                {"source_digest", {{"algorithm", "sha256"}, {"hex", "abc"}}}};
}

Json withError(Json answer) {
    answer["post_commit_error"] = CleanupError;
    return answer;
}

Json rejectedWithError() {
    return Json{{"ticket", "render-config-edit-2"},
                {"status", "rejected"},
                {"committed", false},
                {"post_commit_error", CleanupError}};
}

TEST_CASE("readRenderEditCompletion reads post_commit_error from a committed answer",
          "[devstudio][rendereditcompletion]") {
    const auto reading = readRenderEditCompletion(withError(committed()));
    CHECK(reading.committed);
    CHECK(reading.post_commit_error == CleanupError);
    CHECK(reading.ticket == "render-config-edit-1");
    CHECK_FALSE(reading.restart_required);

    // With a restart too: both are read.
    auto restart = withError(committed());
    restart["restart_required"] = true;
    restart["restart_reason"] = {{"code", "material_route_default_changed"}};
    const auto both = readRenderEditCompletion(restart);
    CHECK(both.post_commit_error == CleanupError);
    CHECK(both.restart_required);
    CHECK(both.restart_code == "material_route_default_changed");
}

TEST_CASE("readRenderEditCompletion does not read post_commit_error from a rejected answer",
          "[devstudio][rendereditcompletion]") {
    const auto rejected = readRenderEditCompletion(rejectedWithError());
    CHECK_FALSE(rejected.committed);
    CHECK(rejected.post_commit_error.empty());
    CHECK(renderEditPostCommitErrorLine(rejected).empty());

    RenderEditPostCommitErrors errors;
    CHECK(errors.take(rejectedWithError()).empty());
}

TEST_CASE("readRenderEditCompletion reads no post_commit_error as empty", "[devstudio][rendereditcompletion]") {
    const auto reading = readRenderEditCompletion(committed());
    CHECK(reading.committed);
    CHECK(reading.post_commit_error.empty());
    CHECK(renderEditPostCommitErrorLine(reading).empty());
    RenderEditPostCommitErrors errors;
    CHECK(errors.take(committed()).empty());
}

TEST_CASE("readRenderEditCompletion reports a non-string post_commit_error",
          "[devstudio][rendereditcompletion]") {
    auto answer = committed();
    answer["post_commit_error"] = {{"message", "cleanup failed"}};
    const auto reading = readRenderEditCompletion(answer);
    REQUIRE(reading.committed);
    CHECK(reading.post_commit_error == "post_commit_error is not a string");
    RenderEditPostCommitErrors errors;
    CHECK(errors.take(answer) ==
          "Saved. Applying it in the running engine failed after the commit: post_commit_error is not a string");
}

TEST_CASE("The post-commit error line says the edit is saved, and the restart with it",
          "[devstudio][rendereditcompletion]") {
    const auto line = renderEditPostCommitErrorLine(readRenderEditCompletion(withError(committed())));
    CHECK(line == std::string{"Saved. Applying it in the running engine failed after the commit: "} + CleanupError);

    auto restart = withError(committed());
    restart["restart_required"] = true;
    restart["restart_reason"] = {{"code", "material_route_default_changed"}};
    const auto both = renderEditPostCommitErrorLine(readRenderEditCompletion(restart));
    CHECK(both.starts_with("Saved."));
    CHECK(both.find(CleanupError) != std::string::npos);
    CHECK(both.find(renderEditRestartLine("material_route_default_changed")) != std::string::npos);
}

TEST_CASE("A ticket's post_commit_error is said once however many readers poll it",
          "[devstudio][rendereditcompletion]") {
    RenderEditPostCommitErrors errors;
    // The panel that sent it and the render edit history each poll the
    // ticket; the observer sees its last answer twice.
    CHECK_FALSE(errors.take(withError(committed())).empty());
    CHECK(errors.take(withError(committed())).empty());
    // Another ticket's is said.
    CHECK_FALSE(errors.take(withError(committed("render-config-edit-3"))).empty());
    CHECK(errors.take(withError(committed("render-config-edit-3"))).empty());
    // The same ticket name from an engine that started again publishes
    // another generation: said again.
    auto again = withError(committed());
    again["published_generation"] = 2u;
    CHECK_FALSE(errors.take(again).empty());
}

} // namespace
} // namespace PelicanStudio
