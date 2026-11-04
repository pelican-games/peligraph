#include "selectionhub.hpp"

namespace PelicanStudio {

std::string graphNodeScopeId(std::string_view scope) {
    return scope == "root" ? std::string{} : std::string{scope};
}

namespace {

bool sameNode(const std::optional<GraphNodeRef> &left,
              const std::optional<GraphNodeRef> &right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left) return true;
    // The instance is how a body was reached, not which node was picked.
    return left->scope == right->scope && left->id == right->id;
}

} // namespace

void SelectionHub::pickedSceneObject() {
    const bool changed = front_ != SelectionFront::scene || graph_node_.has_value();
    front_ = SelectionFront::scene;
    graph_node_.reset();
    advance(changed);
}

void SelectionHub::pickedGraphNode(GraphNodeRef node) {
    node.scope = graphNodeScopeId(node.scope);
    const std::optional<GraphNodeRef> next{std::move(node)};
    const bool changed = front_ != SelectionFront::graph_node || !sameNode(graph_node_, next);
    front_ = SelectionFront::graph_node;
    graph_node_ = next;
    advance(changed);
}

void SelectionHub::reset() {
    const bool changed = front_ != SelectionFront::scene || graph_node_.has_value();
    front_ = SelectionFront::scene;
    graph_node_.reset();
    advance(changed);
}

void SelectionHub::advance(bool state_changed) {
    ++intent_;
    if (state_changed) ++revision_;
    notify();
}

void SelectionHub::notify() {
    if (changed_) changed_();
}

} // namespace PelicanStudio
