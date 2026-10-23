// RT-C: the one reading of a synchronous project-document write's answer
// (Materials edits and binds, register_model / unregister_model,
// edit_project_settings): applied's four values, restart_required's three
// forms, the caller's default reason, an answer with no `applied`, and the
// order when unusable and a restart come together. No Qt, no engine.

#include "appliedstate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using Kind = AppliedState::Kind;

constexpr const char *Default = "the engine reads it when it starts";

TEST_CASE("readAppliedState reads applied's four values", "[devstudio][appliedstate]") {
    SECTION("live") {
        const auto state = readAppliedState(Json{{"applied", "live"}, {"restart_required", nullptr}}, Default);
        CHECK(state.kind == Kind::Live);
        CHECK(state.restart_reason.empty());
        CHECK(state.rebuilt_models.empty());
    }
    SECTION("rebuilt, with the models it lists") {
        const auto state = readAppliedState(
            Json{{"applied", "rebuilt"}, {"restart_required", nullptr}, {"rebuilt_models", {"cube", "sphere"}}},
            Default);
        CHECK(state.kind == Kind::Rebuilt);
        CHECK(state.rebuilt_models == std::vector<std::string>{"cube", "sphere"});
    }
    SECTION("rebuilt with no list") {
        const auto state = readAppliedState(Json{{"applied", "rebuilt"}}, Default);
        CHECK(state.kind == Kind::Rebuilt);
        CHECK(state.rebuilt_models.empty());
    }
    SECTION("restart_required, as the engine writes it") {
        const auto state = readAppliedState(
            Json{{"applied", "restart_required"}, {"restart_required", {{"reason", "the pass changed"}}}}, Default);
        CHECK(state.kind == Kind::RestartRequired);
        CHECK(state.restart_reason == "the pass changed");
    }
    SECTION("unusable, with its problem") {
        const Json problem{{"code", "material.pass_contract_unmet@1"}, {"message", "the pass does not declare it"}};
        const auto state =
            readAppliedState(Json{{"applied", "unusable"}, {"restart_required", nullptr}, {"problem", problem}},
                             Default);
        CHECK(state.kind == Kind::Unusable);
        CHECK(state.problem == problem);
        CHECK(state.restart_reason.empty());
    }
    SECTION("unusable with no problem reads an empty object") {
        const auto state = readAppliedState(Json{{"applied", "unusable"}}, Default);
        CHECK(state.kind == Kind::Unusable);
        CHECK(state.problem == Json::object());
    }
}

TEST_CASE("readAppliedState reads restart_required's three forms", "[devstudio][appliedstate]") {
    SECTION("null is no restart") {
        CHECK(readAppliedState(Json{{"restart_required", nullptr}}, Default).kind == Kind::Live);
    }
    SECTION("a string is the reason") {
        const auto state = readAppliedState(Json{{"restart_required", "hot_reload changed"}}, Default);
        CHECK(state.kind == Kind::RestartRequired);
        CHECK(state.restart_reason == "hot_reload changed");
    }
    SECTION("an object's reason is the reason") {
        const auto state = readAppliedState(Json{{"restart_required", {{"reason", "asset_stores changed"}}}}, Default);
        CHECK(state.kind == Kind::RestartRequired);
        CHECK(state.restart_reason == "asset_stores changed");
    }
}

TEST_CASE("readAppliedState says the caller's default when the engine names no reason",
          "[devstudio][appliedstate]") {
    CHECK(readAppliedState(Json{{"restart_required", ""}}, Default).restart_reason == Default);
    CHECK(readAppliedState(Json{{"restart_required", Json::object()}}, "project.json changed").restart_reason ==
          "project.json changed");
    CHECK(readAppliedState(Json{{"applied", "restart_required"}, {"restart_required", {{"reason", ""}}}},
                           "the engine reads it when it starts again")
              .restart_reason == "the engine reads it when it starts again");
}

TEST_CASE("readAppliedState reads an answer with no applied as live", "[devstudio][appliedstate]") {
    CHECK(readAppliedState(Json::object(), Default).kind == Kind::Live);
    CHECK(readAppliedState(Json{{"changed", true}}, Default).kind == Kind::Live);
    // edit_project_settings: `applied` is the settings object.
    CHECK(readAppliedState(Json{{"applied", {{"hot_reload", Json::array()}}}, {"restart_required", nullptr}},
                           Default)
              .kind == Kind::Live);
    // Not an object at all.
    CHECK(readAppliedState(Json{}, Default).kind == Kind::Live);
    CHECK(readAppliedState(Json::array(), Default).kind == Kind::Live);
}

TEST_CASE("readAppliedState puts unusable before a restart, and a restart before rebuilt",
          "[devstudio][appliedstate]") {
    const auto unusable = readAppliedState(
        Json{{"applied", "unusable"}, {"restart_required", {{"reason", "r"}}}, {"problem", {{"message", "m"}}}},
        Default);
    CHECK(unusable.kind == Kind::Unusable);
    CHECK(unusable.restart_reason.empty());
    CHECK(unusable.problem == Json{{"message", "m"}});

    const auto restart =
        readAppliedState(Json{{"applied", "rebuilt"}, {"restart_required", "r"}, {"rebuilt_models", {"cube"}}},
                         Default);
    CHECK(restart.kind == Kind::RestartRequired);
    CHECK(restart.restart_reason == "r");
    CHECK(restart.rebuilt_models.empty());
}

} // namespace
} // namespace PelicanStudio
