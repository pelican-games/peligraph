// HUB-1a: the shared selection model itself. The window, the Graph's press
// observer and the Inspector page build on these semantics; each of them has
// a test of its own.

#include "../src/devstudio/model/selectionhub.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using PelicanStudio::GraphNodeRef;
using PelicanStudio::SelectionFront;
using PelicanStudio::SelectionHub;
using PelicanStudio::graphNodeScopeId;

} // namespace

TEST_CASE("HUB-1a every user intent rises, state changes alone rise revision",
          "[devstudio][selection-hub][hub1a]") {
    SelectionHub hub;
    int changed = 0;
    hub.setChanged([&] { ++changed; });

    CHECK(hub.front() == SelectionFront::scene);
    CHECK_FALSE(hub.graphNode().has_value());
    CHECK(hub.intent() == 0);
    CHECK(hub.revision() == 0);

    hub.pickedGraphNode(GraphNodeRef{.scope = "root", .id = "blur"});
    CHECK(hub.front() == SelectionFront::graph_node);
    REQUIRE(hub.graphNode().has_value());
    // The engine's "root" is the canvas's empty scope.
    CHECK(hub.graphNode()->scope.empty());
    CHECK(hub.graphNode()->id == "blur");
    CHECK(hub.intent() == 1);
    CHECK(hub.revision() == 1);
    CHECK(changed == 1);

    // The same node again is still a user intent, but it does not move the
    // state the page depends on.
    hub.pickedGraphNode(GraphNodeRef{.scope = "", .id = "blur"});
    CHECK(hub.intent() == 2);
    CHECK(hub.revision() == 1);
    CHECK(changed == 2);

    // The instance is how the body was reached, not the node's identity.
    hub.pickedGraphNode(
        GraphNodeRef{.scope = "", .id = "blur", .instance = "render/body"});
    CHECK(hub.intent() == 3);
    CHECK(hub.revision() == 1);
    REQUIRE(hub.graphNode().has_value());
    CHECK(hub.graphNode()->instance == "render/body");
    CHECK(changed == 3);

    // A different node in the same scope moves both.
    hub.pickedGraphNode(GraphNodeRef{.scope = "", .id = "sum"});
    CHECK(hub.intent() == 4);
    CHECK(hub.revision() == 2);
    CHECK(hub.graphNode()->id == "sum");

    // Back to the scene: the graph node is gone and the front moves.
    hub.pickedSceneObject();
    CHECK(hub.front() == SelectionFront::scene);
    CHECK_FALSE(hub.graphNode().has_value());
    CHECK(hub.intent() == 5);
    CHECK(hub.revision() == 3);

    // Picking the scene it already shows is still an intent.
    hub.pickedSceneObject();
    CHECK(hub.intent() == 6);
    CHECK(hub.revision() == 3);
    CHECK(changed == 6);
}

TEST_CASE("HUB-1a reset is an intent: the next project starts on the scene",
          "[devstudio][selection-hub][hub1a]") {
    SelectionHub hub;
    int changed = 0;
    hub.setChanged([&] { ++changed; });
    hub.pickedGraphNode(GraphNodeRef{.scope = "main", .id = "blur"});
    const std::uint64_t before = hub.intent();

    hub.reset();
    CHECK(hub.front() == SelectionFront::scene);
    CHECK_FALSE(hub.graphNode().has_value());
    CHECK(hub.intent() == before + 1);
    CHECK(hub.revision() == 2);
    CHECK(changed == 2);

    // Already on the scene: reset still rises the intent, so a viewport pick
    // begun before the switch cannot land.
    const std::uint64_t again = hub.intent();
    const std::uint64_t revision = hub.revision();
    hub.reset();
    CHECK(hub.intent() == again + 1);
    CHECK(hub.revision() == revision);
    CHECK(changed == 3);
}

TEST_CASE("HUB-1a scope normalization keeps root in one spelling",
          "[devstudio][selection-hub][hub1a]") {
    CHECK(graphNodeScopeId("root").empty());
    CHECK(graphNodeScopeId("").empty());
    CHECK(graphNodeScopeId("main") == "main");
    CHECK(graphNodeScopeId("render.body") == "render.body");
}
