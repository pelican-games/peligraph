#include "../src/devstudio/model/graphfiltersedit.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {
using PelicanStudio::GraphFiltersEdit;
using Json = nlohmann::json;
using Kind = GraphFiltersEdit::RequestKind;

Json json(const Pelican::GraphFilters &document) {
    return Json::parse(Pelican::renderGraphFilters(document));
}

struct Fixture {
    GraphFiltersEdit edit;
    std::vector<Pelican::GraphFilters> accepted;

    Fixture() {
        edit.committed = [&](const auto &document) { accepted.push_back(document); };
        edit.activateProject(1);
        edit.engineReady(10);
        REQUIRE(edit.requestLoad());
        auto request = one();
        REQUIRE(edit.applyReply(request.id, request.project_generation, request.engine_generation, Kind::load,
                                false, {{"digest", nullptr}, {"document", nullptr}, {"error", nullptr}}));
    }
    GraphFiltersEdit::Request one() {
        auto requests = edit.takeRequests();
        REQUIRE(requests.size() == 1);
        return requests.front();
    }
    void load(const Pelican::GraphFilters &document, std::string digest = "digest-1") {
        REQUIRE(edit.requestLoad());
        const auto request = one();
        REQUIRE(edit.applyReply(request.id, request.project_generation, request.engine_generation, Kind::load,
                                false, {{"digest", digest}, {"document", json(document)}, {"error", nullptr}}));
    }
    void commit(const GraphFiltersEdit::Request &request, bool changed = true, Json previous = nullptr) {
        const auto candidate = Pelican::parseGraphFilters(request.params.at("document")).document;
        REQUIRE(candidate);
        REQUIRE(edit.applyReply(request.id, request.project_generation, request.engine_generation, request.kind,
                                true, {{"committed", true}, {"changed", changed}, {"digest", "next-digest"},
                                       {"previous_digest", request.params.at("base_digest")},
                                       {"previous", std::move(previous)}, {"document", request.params.at("document")}}));
    }
};

TEST_CASE("GF1b edit ids are bounded ASCII and unique through the filter tree", "[gf1b]") {
    Fixture fixture;
    REQUIRE(fixture.edit.addFilter("Left only!"));
    CHECK(fixture.edit.draft().filters.back().id == "left_only");
    REQUIRE(fixture.edit.addFilter("Left only"));
    CHECK(fixture.edit.draft().filters.back().id == "left_only_2");
    REQUIRE(fixture.edit.addFilter("12 clocks"));
    CHECK(fixture.edit.draft().filters.back().id == "f_12_clocks");
    REQUIRE(fixture.edit.addFilter("日本語"));
    CHECK(fixture.edit.draft().filters.back().id == "filter");
    REQUIRE(fixture.edit.addFilter(""));
    CHECK(fixture.edit.draft().filters.back().id == "filter_2");
    REQUIRE(fixture.edit.addFilter(std::string(100, 'a')));
    CHECK(fixture.edit.draft().filters.back().id == std::string(64, 'a'));
    REQUIRE(fixture.edit.addFilter(std::string(100, 'a')));
    CHECK(fixture.edit.draft().filters.back().id == std::string(62, 'a') + "_2");
    fixture.edit.save();
    fixture.commit(fixture.one());
    fixture.load([] { Pelican::GraphFilters document;
        document.filters.push_back({"parent", "Parent", {}, {{"child", "Child", {}, {}}}});
        return document;
    }());
    REQUIRE(fixture.edit.addFilter("child"));
    CHECK(fixture.edit.draft().filters.back().id == "child_2");
    REQUIRE(fixture.edit.addFilter("all"));
    CHECK(fixture.edit.draft().filters.back().id == "all");
}

TEST_CASE("GF1b category removal and feature updates preserve nested filters", "[gf1b]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.categories = {{"hero", "Hero"}, {"other", "Other"}};
    document.feature_categories = {{"folded", {"hero"}}, {"typed_feature", {"other", "hero"}}};
    document.filters = {{"parent", "Parent", {}, {}}};
    document.filters[0].match.feature_categories = {"hero"};
    document.filters[0].children = {{"child", "Child", {}, {}}};
    document.filters[0].children[0].match.feature_categories = {"hero", "other"};
    fixture.load(document);
    REQUIRE(fixture.edit.removeCategory("hero"));
    CHECK(fixture.edit.draft().feature_categories.size() == 1);
    CHECK(fixture.edit.draft().feature_categories[0] == std::pair<std::string, std::vector<std::string>>{"typed_feature", {"other"}});
    CHECK(fixture.edit.draft().filters[0].match.feature_categories.empty());
    CHECK(fixture.edit.draft().filters[0].children[0].match.feature_categories == std::vector<std::string>{"other"});
    CHECK(fixture.edit.categoryReferenceCounts("other") == std::pair<std::size_t, std::size_t>{1, 1});
    CHECK(fixture.edit.descendantCount("parent") == 1);
    REQUIRE_FALSE(fixture.edit.setFeatureCategories("typed_feature", {"hero"}));
    REQUIRE(fixture.edit.setFeatureCategories("typed_feature", {}));
    CHECK(fixture.edit.draft().feature_categories.empty());
    REQUIRE(fixture.edit.setFeatureCategories("typed_feature", {"other"}));
    REQUIRE(fixture.edit.setCategoryLabel("other", "Renamed"));
    CHECK(fixture.edit.draft().categories[0].id == "other");
    CHECK(fixture.edit.draft().feature_categories[0].second == std::vector<std::string>{"other"});
    fixture.edit.save();
    const auto request = fixture.one();
    fixture.commit(request, true, json(document));
    CHECK(fixture.edit.saved().filters[0].children.size() == 1);
    CHECK(fixture.accepted.back().filters[0].children.size() == 1);
}

TEST_CASE("GF1b save Undo discard and refusal keep their document transitions", "[gf1b]") {
    Fixture fixture;
    REQUIRE_FALSE(fixture.edit.undo());
    REQUIRE(fixture.edit.addFilter("First"));
    fixture.edit.save();
    auto pending = fixture.one();
    REQUIRE_FALSE(fixture.edit.addFilter("Blocked during save"));
    REQUIRE_FALSE(fixture.edit.undo());
    fixture.commit(pending, true, nullptr);
    CHECK(fixture.edit.undoCount() == 1);
    CHECK(fixture.edit.digest() == std::optional<std::string>{"next-digest"});
    CHECK(fixture.accepted.size() == 1);
    REQUIRE(fixture.edit.addFilter("Unsaved"));
    fixture.edit.save();
    pending = fixture.one();
    fixture.commit(pending, false, nullptr);
    CHECK(fixture.edit.undoCount() == 1);
    REQUIRE(fixture.edit.undo());
    pending = fixture.one();
    CHECK(pending.params.at("base_digest") == "next-digest");
    CHECK(pending.params.at("document").at("filters").empty());
    fixture.commit(pending, true, json(fixture.edit.saved()));
    CHECK(fixture.edit.undoCount() == 0);
    CHECK(fixture.edit.saved().filters.empty());
    CHECK(fixture.accepted.size() == 3);

    Pelican::GraphFilters document;
    document.filters.push_back({"saved", "Saved", {}, {}});
    fixture.load(document);
    REQUIRE(fixture.edit.addFilter("Unsaved"));
    REQUIRE(fixture.edit.discard());
    CHECK(fixture.edit.draft() == fixture.edit.saved());
    REQUIRE(fixture.edit.setLabel("saved", "Changed"));
    fixture.edit.save();
    pending = fixture.one();
    REQUIRE(fixture.edit.applyRefusal(pending.id, pending.project_generation, pending.engine_generation,
                                      Kind::save, "external_modification", "stale"));
    CHECK(fixture.edit.state().external_modification);
    CHECK(fixture.edit.state().save_enabled);
    CHECK(fixture.edit.draft().filters.size() == 1);
    CHECK(fixture.edit.draft().filters[0].label == "Changed");
    CHECK(fixture.edit.undoCount() == 0);
    CHECK_FALSE(fixture.edit.loadReplacesDirtyDraft());
    CHECK(fixture.edit.state().confirm_load);
    CHECK(fixture.edit.loadReplacesDirtyDraft());
    const auto confirmed_load = fixture.one();
    REQUIRE(fixture.edit.applyReply(confirmed_load.id, confirmed_load.project_generation,
                                    confirmed_load.engine_generation, Kind::load, false,
                                    {{"digest", "disk-digest"}, {"document", json(Pelican::GraphFilters{})},
                                     {"error", nullptr}}));
}

TEST_CASE("GF1b stale load save and restart replies cannot change current state", "[gf1b]") {
    Fixture fixture;
    REQUIRE(fixture.edit.addFilter("Draft"));
    const auto draft = fixture.edit.draft();
    fixture.edit.save();
    const auto pending = fixture.one();
    REQUIRE_FALSE(fixture.edit.applyReply(pending.id, 2, pending.engine_generation, Kind::save, true,
                                          {{"document", json(Pelican::GraphFilters{})}, {"digest", "bad"},
                                           {"changed", true}, {"previous", nullptr}}));
    REQUIRE(fixture.edit.applyFailure(pending.id, pending.project_generation, pending.engine_generation,
                                      Kind::save, "retain draft"));
    CHECK(fixture.edit.draft() == draft);
    fixture.edit.requestLoad();
    const auto old_load = fixture.one();
    fixture.edit.activateProject(2);
    fixture.edit.engineReady(10);
    REQUIRE_FALSE(fixture.edit.applyReply(old_load.id, old_load.project_generation, old_load.engine_generation,
                                          Kind::load, false,
                                          {{"digest", nullptr}, {"document", nullptr}, {"error", nullptr}}));
    CHECK(fixture.edit.draft() == draft);
    CHECK(fixture.edit.state().dirty);
    CHECK(fixture.edit.takeRequests().empty());
    fixture.edit.requestLoad();
    const auto before_restart = fixture.one();
    fixture.edit.engineGone();
    fixture.edit.engineReady(11);
    REQUIRE_FALSE(fixture.edit.applyReply(before_restart.id, before_restart.project_generation,
                                          before_restart.engine_generation, Kind::load, false,
                                          {{"digest", "replacement"}, {"document", json(Pelican::GraphFilters{})},
                                           {"error", nullptr}}));
    CHECK(fixture.edit.draft() == draft);
}

TEST_CASE("GF1b a delayed load cannot overwrite a completed save", "[gf1b]") {
    Fixture fixture;
    REQUIRE(fixture.edit.requestLoad());
    const auto delayed = fixture.one();
    REQUIRE(fixture.edit.applyFailure(delayed.id, delayed.project_generation, delayed.engine_generation,
                                      Kind::load, "timed out"));
    REQUIRE(fixture.edit.addFilter("Committed draft"));
    fixture.edit.save();
    const auto save = fixture.one();
    fixture.commit(save, true, nullptr);
    const auto saved = fixture.edit.saved();
    const auto draft = fixture.edit.draft();
    REQUIRE_FALSE(fixture.edit.applyReply(delayed.id, delayed.project_generation, delayed.engine_generation,
                                          Kind::load, false,
                                          {{"digest", nullptr}, {"document", nullptr}, {"error", nullptr}}));
    CHECK(fixture.edit.saved() == saved);
    CHECK(fixture.edit.draft() == draft);
    CHECK(fixture.edit.digest() == std::optional<std::string>{"next-digest"});
    CHECK(fixture.edit.undoCount() == 1);
    CHECK(fixture.accepted.size() == 1);
}

TEST_CASE("GF1b a lost commit is reconciled by its fresh get without resending", "[gf1b]") {
    Fixture fixture;
    REQUIRE(fixture.edit.addFilter("First"));
    fixture.edit.save();
    const auto pending = fixture.one();
    fixture.edit.engineGone();
    CHECK(fixture.edit.takeRequests().empty());
    CHECK_FALSE(fixture.edit.state().text == "Saved");
    CHECK(fixture.edit.draft().filters.size() == 1);
    CHECK(fixture.edit.undoCount() == 0);
    fixture.edit.engineReady(11);
    fixture.edit.requestLoad();
    const auto load = fixture.one();
    REQUIRE(fixture.edit.applyReply(load.id, load.project_generation, load.engine_generation, Kind::load,
                                    false, {{"digest", "committed-digest"}, {"document", pending.params.at("document")},
                                            {"error", nullptr}}));
    CHECK(fixture.edit.saved().filters.size() == 1);
    CHECK(fixture.edit.digest() == std::optional<std::string>{"committed-digest"});
    CHECK_FALSE(fixture.edit.state().dirty);
    CHECK(fixture.edit.undoCount() == 1);

    REQUIRE(fixture.edit.undo());
    const auto undo_request = fixture.one();
    fixture.edit.save();
    CHECK(fixture.edit.takeRequests().empty());
    REQUIRE(fixture.edit.applyRefusal(undo_request.id, undo_request.project_generation,
                                      undo_request.engine_generation, Kind::undo, "busy", "busy"));
    CHECK(fixture.edit.undoCount() == 1);
    CHECK(fixture.edit.saved().filters.size() == 1);
}

TEST_CASE("GF1b invalid external loads preserve a dirty draft", "[gf1b]") {
    Fixture fixture;
    REQUIRE(fixture.edit.addCategory("Original"));
    REQUIRE_FALSE(fixture.edit.loadReplacesDirtyDraft());
    REQUIRE(fixture.edit.loadReplacesDirtyDraft());
    const auto request = fixture.one();
    REQUIRE(fixture.edit.applyReply(request.id, request.project_generation, request.engine_generation, Kind::load,
                                    false, {{"digest", "broken"}, {"document", nullptr}, {"error", "invalid JSON"}}));
    CHECK(fixture.edit.draft().categories.size() == 1);
    CHECK(fixture.edit.saved().categories.empty());
    CHECK(fixture.edit.state().dirty);
    CHECK(fixture.edit.state().text == "invalid JSON");
}

} // namespace
