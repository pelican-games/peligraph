#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace PelicanStudio {

// One node of the common graph. `scope` is the document scope the canvas
// files the node under; the root document is the empty string ("root" is the
// engine's spelling of it). `instance` is the instance path that reached a
// body scope, when one did; it is context for reopening the node, not part of
// its identity.
struct GraphNodeRef {
    std::string scope;
    std::string id;
    std::string instance;

    friend bool operator==(const GraphNodeRef &, const GraphNodeRef &) = default;
};

// The engine and the canvas spell the root differently; the hub keeps the
// canvas's spelling everywhere.
[[nodiscard]] std::string graphNodeScopeId(std::string_view scope);

enum class SelectionFront { scene, graph_node };

// HUB-1a: the one place the two entrances -- the Outliner / the viewport and
// the Graph tab -- put what the user picked. It is deliberately small: a
// front, a graph node and a counter for the user intents the window has seen,
// so a viewport pick that answers after a later intent can tell it is stale.
//
// `intent()` rises for every picked*() call, even one that names what the hub
// already shows. `revision()` rises only when the front, the node or the
// instance context actually changes. `changed()` runs on every picked*() and
// reset; the window coalesces those calls into one Inspector refresh.
class SelectionHub {
  public:
    using Changed = std::function<void()>;

    [[nodiscard]] SelectionFront front() const noexcept { return front_; }
    [[nodiscard]] const std::optional<GraphNodeRef> &graphNode() const noexcept {
        return graph_node_;
    }
    [[nodiscard]] std::uint64_t intent() const noexcept { return intent_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    void setChanged(Changed changed) { changed_ = std::move(changed); }

    // The user picked a scene object (Outliner, keyboard move, viewport pick,
    // Back to scene). The object itself stays in SelectionModel; the hub only
    // says which front the Inspector follows.
    void pickedSceneObject();
    // The user pressed a Graph node. The Graph reports an empty canvas, an
    // edge and a control point as nullopt; the window turns that into
    // pickedSceneObject().
    void pickedGraphNode(GraphNodeRef node);
    // A project switch: scene front, no node. This is an intent like any
    // other, so an answer to a pick begun in the old project cannot move it.
    void reset();

  private:
    void advance(bool state_changed);
    void notify();

    SelectionFront front_ = SelectionFront::scene;
    std::optional<GraphNodeRef> graph_node_;
    std::uint64_t intent_ = 0;
    std::uint64_t revision_ = 0;
    Changed changed_;
};

} // namespace PelicanStudio
