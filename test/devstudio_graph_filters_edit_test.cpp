#include "../src/devstudio/model/graphfilters.hpp"
#include "../src/devstudio/model/graphfiltersedit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <utility>

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

// --- GF-1e: the tree operations. Every refusal compares the whole draft.
Pelican::GraphFilter gf1eFilter(const std::string &id, std::vector<std::string> node_ids = {},
                                std::vector<Pelican::GraphFilter> children = {}) {
    Pelican::GraphFilter filter;
    filter.id = id;
    filter.label = "Label " + id;
    filter.match.node_ids = std::move(node_ids);
    filter.children = std::move(children);
    return filter;
}

// l1 > l2 > ... > l<levels>, plus a category and a feature that a refusal must keep.
Pelican::GraphFilters gf1eChain(int levels) {
    Pelican::GraphFilter filter = gf1eFilter("l" + std::to_string(levels), {"deep"});
    for (int level = levels - 1; level >= 1; --level)
        filter = gf1eFilter("l" + std::to_string(level), {"n" + std::to_string(level)}, {std::move(filter)});
    Pelican::GraphFilters document;
    document.categories = {{"hero", "Hero"}};
    document.feature_categories = {{"zeta", {"hero"}}, {"alpha", {"hero"}}};
    document.filters = {std::move(filter)};
    return document;
}

const Pelican::GraphFilter &gf1eAt(const Pelican::GraphFilters &document, const std::string &id) {
    const auto *found = PelicanStudio::findGraphFilter(document, id);
    REQUIRE(found != nullptr);
    return *found;
}

std::vector<std::string> gf1eIds(const std::vector<Pelican::GraphFilter> &filters) {
    std::vector<std::string> ids;
    for (const auto &filter : filters) ids.push_back(filter.id);
    return ids;
}

void gf1eRoundTrips(const GraphFiltersEdit &edit) {
    const auto parsed = Pelican::parseGraphFilters(Json::parse(Pelican::renderGraphFilters(edit.draft())));
    REQUIRE(parsed.document);
    // Only filters: the parser's nlohmann::json sorts feature_categories.
    CHECK(parsed.document->filters == edit.draft().filters);
}

// A refused edit leaves everything: the same value and the same bytes.
struct GF1eUnchanged {
    const GraphFiltersEdit &edit;
    Pelican::GraphFilters before = edit.draft();
    std::string bytes = Pelican::renderGraphFilters(edit.draft());
    void check() const {
        CHECK(edit.draft() == before);
        CHECK(Pelican::renderGraphFilters(edit.draft()) == bytes);
    }
};

bool gf1eTextHas(const GraphFiltersEdit &edit, const std::string &text) {
    return edit.state().text.find(text) != std::string::npos;
}

TEST_CASE("GF1e addChildFilter nests with an id unique across levels and reaches children", "[gf1e]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.filters = {gf1eFilter("filter", {"own"})};
    fixture.load(document);
    REQUIRE(fixture.edit.addChildFilter("filter", "Filter"));
    const auto &parent = gf1eAt(fixture.edit.draft(), "filter");
    REQUIRE(parent.children.size() == 1);
    CHECK(parent.children[0].id == "filter_2");
    CHECK(parent.children[0].label == "Filter");
    CHECK(parent.match.node_ids == std::vector<std::string>{"own"});
    gf1eRoundTrips(fixture.edit);
    REQUIRE(fixture.edit.addChildFilter("filter_2", "Grand"));
    gf1eRoundTrips(fixture.edit);
    CHECK_FALSE(fixture.edit.addChildFilter("missing", "Nobody"));

    REQUIRE(fixture.edit.setLabel("grand", "Grand label"));
    CHECK(gf1eAt(fixture.edit.draft(), "grand").label == "Grand label");
    Pelican::GraphFilterMatch match;
    match.ops = {"value."};
    REQUIRE(fixture.edit.setMatch("grand", match));
    CHECK(gf1eAt(fixture.edit.draft(), "grand").match == match);
    CHECK(fixture.edit.descendantCount("filter") == 2);
    CHECK(fixture.edit.descendantCount("filter_2") == 1);
    CHECK(fixture.edit.descendantCount("grand") == 0);
    const auto place = fixture.edit.filterPlace("grand");
    REQUIRE(place);
    CHECK(place->depth == 3);
    CHECK(place->index == 0);
    CHECK(place->siblings == 1);
    CHECK(place->parent == "filter_2");
    CHECK(fixture.edit.filterPlace("filter")->parent.empty());
    CHECK_FALSE(fixture.edit.filterPlace("missing"));
}

TEST_CASE("GF1e moveFilter moves among siblings at any level and never crosses a parent", "[gf1e]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.filters = {gf1eFilter("p", {}, {gf1eFilter("a"), gf1eFilter("b"), gf1eFilter("c")}),
                        gf1eFilter("q", {}, {gf1eFilter("z")})};
    fixture.load(document);
    REQUIRE(fixture.edit.moveFilter("b", -1));
    CHECK(gf1eIds(gf1eAt(fixture.edit.draft(), "p").children) == std::vector<std::string>{"b", "a", "c"});
    gf1eRoundTrips(fixture.edit);
    {
        GF1eUnchanged unchanged{fixture.edit};
        CHECK_FALSE(fixture.edit.moveFilter("b", -1));
        unchanged.check();
    }
    REQUIRE(fixture.edit.moveFilter("a", 1));
    CHECK(gf1eIds(gf1eAt(fixture.edit.draft(), "p").children) == std::vector<std::string>{"b", "c", "a"});
    {
        GF1eUnchanged unchanged{fixture.edit};
        CHECK_FALSE(fixture.edit.moveFilter("a", 1));
        CHECK_FALSE(fixture.edit.moveFilter("z", 1));
        CHECK_FALSE(fixture.edit.moveFilter("missing", 1));
        unchanged.check();
    }
    REQUIRE(fixture.edit.moveFilter("q", -1));
    CHECK(gf1eIds(fixture.edit.draft().filters) == std::vector<std::string>{"q", "p"});
    gf1eRoundTrips(fixture.edit);
}

TEST_CASE("GF1e indent and outdent keep a parent's own match and the sibling order", "[gf1e]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.filters = {gf1eFilter("a", {"a_own"}, {gf1eFilter("a1")}), gf1eFilter("b", {"b_own"}),
                        gf1eFilter("c", {"c_own"})};
    fixture.load(document);
    {
        GF1eUnchanged unchanged{fixture.edit};
        CHECK_FALSE(fixture.edit.indentFilter("a"));
        CHECK_FALSE(fixture.edit.indentFilter("a1"));
        CHECK_FALSE(fixture.edit.outdentFilter("a"));
        CHECK_FALSE(fixture.edit.outdentFilter("missing"));
        unchanged.check();
    }
    REQUIRE(fixture.edit.indentFilter("b"));
    gf1eRoundTrips(fixture.edit);
    CHECK(gf1eIds(fixture.edit.draft().filters) == std::vector<std::string>{"a", "c"});
    CHECK(gf1eIds(gf1eAt(fixture.edit.draft(), "a").children) == std::vector<std::string>{"a1", "b"});
    CHECK(gf1eAt(fixture.edit.draft(), "a").match.node_ids == std::vector<std::string>{"a_own"});
    CHECK(gf1eAt(fixture.edit.draft(), "b").match.node_ids == std::vector<std::string>{"b_own"});

    REQUIRE(fixture.edit.outdentFilter("a1"));
    gf1eRoundTrips(fixture.edit);
    CHECK(gf1eIds(fixture.edit.draft().filters) == std::vector<std::string>{"a", "a1", "c"});
    CHECK(gf1eIds(gf1eAt(fixture.edit.draft(), "a").children) == std::vector<std::string>{"b"});
    REQUIRE(fixture.edit.outdentFilter("b"));
    CHECK(gf1eIds(fixture.edit.draft().filters) == std::vector<std::string>{"a", "b", "a1", "c"});
    const auto &emptied = gf1eAt(fixture.edit.draft(), "a");
    CHECK(emptied.children.empty());
    CHECK(emptied.match.node_ids == std::vector<std::string>{"a_own"});
    gf1eRoundTrips(fixture.edit);
}

TEST_CASE("GF1e the parser's depth limit refuses edits and leaves the whole draft", "[gf1e]") {
    Fixture fixture;
    fixture.load(gf1eChain(8));
    CHECK(fixture.edit.filterPlace("l8")->depth == 8);
    {
        GF1eUnchanged unchanged{fixture.edit};
        CHECK_FALSE(fixture.edit.addChildFilter("l8", "Too deep"));
        unchanged.check();
        CHECK(gf1eTextHas(fixture.edit, "nest deeper than 8"));
    }
    CHECK_FALSE(fixture.edit.state().dirty);

    // A subtree whose root would reach level 8 and whose child would reach 9.
    Pelican::GraphFilters subtree = gf1eChain(6);
    auto *level6 = const_cast<Pelican::GraphFilter *>(PelicanStudio::findGraphFilter(subtree, "l6"));
    REQUIRE(level6 != nullptr);
    level6->children = {gf1eFilter("first", {"first_own"}),
                        gf1eFilter("second", {"second_own"}, {gf1eFilter("second_child", {"kept"})})};
    fixture.load(subtree);
    CHECK(fixture.edit.filterPlace("second")->depth == 7);
    {
        GF1eUnchanged unchanged{fixture.edit};
        CHECK_FALSE(fixture.edit.indentFilter("second"));
        unchanged.check();
        CHECK(gf1eTextHas(fixture.edit, "nest deeper than 8"));
    }
    // Landing exactly on level 8 is accepted.
    REQUIRE(fixture.edit.outdentFilter("second_child"));
    REQUIRE(fixture.edit.indentFilter("second"));
    CHECK(fixture.edit.filterPlace("second")->depth == 8);
    gf1eRoundTrips(fixture.edit);
    REQUIRE(fixture.edit.addChildFilter("second_child", "Room left"));
    gf1eRoundTrips(fixture.edit);
}

TEST_CASE("GF1e setFilterId refuses a repeated or invalid id and leaves the whole draft", "[gf1e]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.categories = {{"hero", "Hero"}};
    document.feature_categories = {{"zeta", {"hero"}}, {"alpha", {"hero"}}};
    document.filters = {gf1eFilter("a", {"a_own"}, {gf1eFilter("deep", {"deep_own"})}), gf1eFilter("b")};
    fixture.load(document);
    const auto refused = [&](const std::string &id, const std::string &to, const std::string &reason) {
        GF1eUnchanged unchanged{fixture.edit};
        CHECK_FALSE(fixture.edit.setFilterId(id, to));
        unchanged.check();
        CHECK(gf1eTextHas(fixture.edit, reason));
    };
    refused("b", "deep", "is repeated");
    refused("deep", "a", "is repeated");
    refused("b", "Bad-Id", "is not");
    refused("b", "", "is empty");
    const auto text = fixture.edit.state().text;
    CHECK_FALSE(fixture.edit.setFilterId("b", "b"));
    CHECK(fixture.edit.state().text == text);
    CHECK_FALSE(fixture.edit.setFilterId("missing", "free"));

    REQUIRE(fixture.edit.setFilterId("deep", "free"));
    CHECK(std::as_const(fixture.edit).findFilter("deep") == nullptr);
    REQUIRE(std::as_const(fixture.edit).findFilter("free") != nullptr);
    CHECK(std::as_const(fixture.edit).findFilter("free")->match.node_ids == std::vector<std::string>{"deep_own"});
    gf1eRoundTrips(fixture.edit);
}

TEST_CASE("GF1e removing a filter with descendants takes two presses on the same draft", "[gf1e]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.filters = {gf1eFilter("parent", {}, {gf1eFilter("kid", {}, {gf1eFilter("grand")}), gf1eFilter("kid2")}),
                        gf1eFilter("other", {}, {gf1eFilter("other_kid")}), gf1eFilter("leaf")};
    fixture.load(document);
    int fired = 0;
    fixture.edit.changed = [&] { ++fired; };

    // First press: armed, named, nothing lost.
    {
        GF1eUnchanged unchanged{fixture.edit};
        REQUIRE_FALSE(fixture.edit.removeFilter("parent"));
        unchanged.check();
    }
    CHECK(fixture.edit.state().confirm_remove);
    CHECK(gf1eTextHas(fixture.edit, "and 3 more"));
    CHECK(gf1eTextHas(fixture.edit, "Label parent"));

    // Cancelling restores the text without firing `changed`.
    const int before_cancel = fired;
    CHECK(fixture.edit.cancelRemoveConfirmation());
    CHECK(fired == before_cancel);
    CHECK_FALSE(fixture.edit.state().confirm_remove);
    CHECK(fixture.edit.state().text == "Saved");
    CHECK_FALSE(fixture.edit.cancelRemoveConfirmation());

    // An edit between the presses cancels: the next press only arms again.
    REQUIRE_FALSE(fixture.edit.removeFilter("parent"));
    REQUIRE(fixture.edit.setLabel("leaf", "Changed"));
    CHECK_FALSE(fixture.edit.state().confirm_remove);
    REQUIRE_FALSE(fixture.edit.removeFilter("parent"));
    CHECK(fixture.edit.state().confirm_remove);
    CHECK(std::as_const(fixture.edit).findFilter("parent") != nullptr);

    // A press on another id arms that id and removes nothing.
    REQUIRE_FALSE(fixture.edit.removeFilter("other"));
    CHECK(std::as_const(fixture.edit).findFilter("parent") != nullptr);
    CHECK(std::as_const(fixture.edit).findFilter("other") != nullptr);
    CHECK(gf1eTextHas(fixture.edit, "and 1 more"));

    // Discard and Load cancel too.
    REQUIRE(fixture.edit.discard());
    CHECK_FALSE(fixture.edit.state().confirm_remove);
    REQUIRE_FALSE(fixture.edit.removeFilter("parent"));
    REQUIRE(fixture.edit.loadReplacesDirtyDraft());
    CHECK_FALSE(fixture.edit.state().confirm_remove);
    const auto pending_load = fixture.one();
    REQUIRE(fixture.edit.applyReply(pending_load.id, pending_load.project_generation, pending_load.engine_generation,
                                    Kind::load, false,
                                    {{"digest", "digest-1"}, {"document", json(document)}, {"error", nullptr}}));

    // Second press removes the subtree, at any level.
    REQUIRE_FALSE(fixture.edit.removeFilter("kid"));
    CHECK(gf1eTextHas(fixture.edit, "and 1 more"));
    REQUIRE(fixture.edit.removeFilter("kid"));
    CHECK(std::as_const(fixture.edit).findFilter("kid") == nullptr);
    CHECK(std::as_const(fixture.edit).findFilter("grand") == nullptr);
    CHECK(gf1eIds(gf1eAt(fixture.edit.draft(), "parent").children) == std::vector<std::string>{"kid2"});
    gf1eRoundTrips(fixture.edit);

    // A leaf goes at once, at any level.
    REQUIRE(fixture.edit.removeFilter("kid2"));
    REQUIRE(fixture.edit.removeFilter("leaf"));
    gf1eRoundTrips(fixture.edit);
    CHECK_FALSE(fixture.edit.removeFilter("missing"));
    REQUIRE_FALSE(fixture.edit.removeFilter("other"));
    fixture.edit.save();
    CHECK_FALSE(fixture.edit.state().confirm_remove);
}

TEST_CASE("GF1e Discard and Undo after a subtree removal are separate paths", "[gf1e]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.categories = {{"hero", "Hero"}};
    document.feature_categories = {{"alpha", {"hero"}}};
    document.filters = {gf1eFilter("parent", {"own"}, {gf1eFilter("kid", {"kid_own"})}), gf1eFilter("leaf")};
    fixture.load(document);
    const auto saved_bytes = Pelican::renderGraphFilters(fixture.edit.saved());

    // Discard restores the saved draft and sends nothing.
    REQUIRE_FALSE(fixture.edit.removeFilter("parent"));
    REQUIRE(fixture.edit.removeFilter("parent"));
    CHECK(fixture.edit.state().dirty);
    CHECK_FALSE(fixture.edit.undo());
    REQUIRE(fixture.edit.discard());
    CHECK(Pelican::renderGraphFilters(fixture.edit.draft()) == saved_bytes);
    CHECK(fixture.edit.takeRequests().empty());

    // Undo is one request per Save, restoring the committed document with its nesting.
    REQUIRE_FALSE(fixture.edit.removeFilter("parent"));
    REQUIRE(fixture.edit.removeFilter("parent"));
    REQUIRE(fixture.edit.save());
    auto request = fixture.one();
    CHECK(request.kind == Kind::save);
    CHECK(request.params.at("document").at("filters").size() == 1);
    fixture.commit(request, true, json(document));
    CHECK(fixture.edit.undoCount() == 1);
    CHECK(std::as_const(fixture.edit).findFilter("parent") == nullptr);
    REQUIRE(fixture.edit.undo());
    request = fixture.one();
    CHECK(request.kind == Kind::undo);
    CHECK(request.params.at("base_digest") == "next-digest");
    const auto restored = Pelican::parseGraphFilters(request.params.at("document")).document;
    REQUIRE(restored);
    CHECK(Pelican::renderGraphFilters(*restored) == saved_bytes);
    fixture.commit(request, true, json(fixture.edit.saved()));
    CHECK(fixture.edit.undoCount() == 0);
    CHECK(gf1eAt(fixture.edit.saved(), "kid").match.node_ids == std::vector<std::string>{"kid_own"});
}

TEST_CASE("GF1e a nested Save sends children, and Undo isn't available over an unsaved tree edit", "[gf1e]") {
    Fixture fixture;
    REQUIRE(fixture.edit.addFilter("Parent"));
    REQUIRE(fixture.edit.addChildFilter("parent", "Child"));
    REQUIRE(fixture.edit.save());
    auto request = fixture.one();
    CHECK(request.params.at("document").at("filters").at(0).at("children").at(0).at("id") == "child");
    fixture.commit(request, true, nullptr);
    CHECK(fixture.edit.undoCount() == 1);
    REQUIRE(fixture.edit.indentFilter("parent") == false);
    REQUIRE(fixture.edit.outdentFilter("child"));
    CHECK_FALSE(fixture.edit.undo());
    CHECK(fixture.edit.state().save_enabled);
}

TEST_CASE("GF1e the disk conflict keeps a tree edit, and a pending request blocks every operation", "[gf1e]") {
    Fixture fixture;
    Pelican::GraphFilters document;
    document.filters = {gf1eFilter("a"), gf1eFilter("b", {}, {gf1eFilter("b1")})};
    fixture.load(document);
    REQUIRE(fixture.edit.indentFilter("b"));
    const auto draft = fixture.edit.draft();
    REQUIRE(fixture.edit.save());

    // While the request is out, nothing changes the draft or the text.
    CHECK(fixture.edit.state().text == "Saving...");
    CHECK_FALSE(fixture.edit.addChildFilter("a", "x"));
    CHECK_FALSE(fixture.edit.moveFilter("b1", -1));
    CHECK_FALSE(fixture.edit.indentFilter("b1"));
    CHECK_FALSE(fixture.edit.outdentFilter("b"));
    CHECK_FALSE(fixture.edit.setFilterId("a", "free"));
    CHECK_FALSE(fixture.edit.removeFilter("b1"));
    CHECK_FALSE(fixture.edit.state().confirm_remove);
    CHECK(fixture.edit.draft() == draft);
    CHECK(fixture.edit.state().text == "Saving...");

    const auto request = fixture.one();
    REQUIRE(fixture.edit.applyRefusal(request.id, request.project_generation, request.engine_generation, Kind::save,
                                      "external_modification", "stale"));
    CHECK(fixture.edit.state().external_modification);
    CHECK(fixture.edit.draft() == draft);
    CHECK(fixture.edit.undoCount() == 0);
    CHECK_FALSE(fixture.edit.loadReplacesDirtyDraft());
    CHECK(fixture.edit.state().confirm_load);
    REQUIRE(fixture.edit.loadReplacesDirtyDraft());
    const auto load = fixture.one();
    REQUIRE(fixture.edit.applyReply(load.id, load.project_generation, load.engine_generation, Kind::load, false,
                                    {{"digest", "disk"}, {"document", json(document)}, {"error", nullptr}}));
    CHECK(gf1eIds(fixture.edit.draft().filters) == std::vector<std::string>{"a", "b"});
}

} // namespace
