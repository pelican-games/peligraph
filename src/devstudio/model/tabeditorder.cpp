#include "tabeditorder.hpp"

#include <algorithm>
#include <utility>

namespace PelicanStudio {

TabEditOrder::TabEditOrder(GraphCanvasModel &graph) : graph_{graph} {
    const std::weak_ptr<void> alive = lifetime_;
    graph_.historySubmitted = [this, alive] {
        if (!alive.expired()) graph_pending_ = submitted(History::Graph);
    };
    graph_.historyCleared = [this, alive] {
        if (!alive.expired()) forgetGraph();
    };
    graph_.historyMoved = [this, alive](std::uint64_t serial, GraphCanvasModel::HistoryMove direction, bool ok) {
        if (alive.expired()) return;
        if (direction == GraphCanvasModel::HistoryMove::Push)
            complete(std::exchange(graph_pending_, 0), ok ? serial : 0);
        else
            moved(History::Graph, serial,
                  direction == GraphCanvasModel::HistoryMove::Undo ? Move::Undo : Move::Redo, ok);
    };
}

void TabEditOrder::setRenderHistory(RenderEditHistory *history) {
    if (render_ == history) return;
    render_connection_.reset();
    forget(History::Render);
    render_ = history;
    if (render_)
        render_connection_ = render_->observeMoves([this](std::uint64_t serial, Move direction, bool ok) {
            moved(History::Render, serial, direction, ok);
        });
}

std::uint64_t TabEditOrder::submitted(History history) {
    const auto sent = next_sent_++;
    edits_.push_back({history, sent, 0});
    constexpr std::size_t capacity = 256;
    if (edits_.size() > capacity) edits_.erase(edits_.begin());
    return sent;
}

void TabEditOrder::complete(std::uint64_t sent, std::uint64_t serial) {
    const auto found = std::ranges::find(edits_, sent, &Record::sent);
    if (found == edits_.end()) return; // Cleared or disconnected while in flight.
    if (serial == 0) {
        edits_.erase(found);
        return;
    }
    found->serial = serial;
    // Only a committed new edit consumes Redo, across both histories.
    undone_.clear();
}

RenderEditHistory::Completion TabEditOrder::completion(std::uint64_t sent) {
    const std::weak_ptr<void> alive = lifetime_;
    return [this, alive, sent](std::uint64_t serial) {
        if (!alive.expired()) complete(sent, serial);
    };
}

void TabEditOrder::renderRequestSent(std::int64_t id) {
    if (!render_ || id == 0) return;
    const auto sent = submitted(History::Render);
    if (!render_->followRequest(id, completion(sent))) complete(sent, 0);
}

void TabEditOrder::recordFiles(std::string label, std::vector<RenderEditHistory::FileChange> files) {
    if (!render_) return;
    const auto sent = submitted(History::Render);
    render_->recordFiles(std::move(label), std::move(files), completion(sent));
}

void TabEditOrder::moved(History history, std::uint64_t serial, Move direction, bool ok) {
    if (!ok) return;
    if (direction == Move::Push) {
        // A foreign push discards this history's Redo as well. Own pushes
        // have already bound their placeholder and cleared all tab Redo.
        std::erase_if(undone_, [history](const Record &record) { return record.history == history; });
        return;
    }
    auto &from = direction == Move::Undo ? edits_ : undone_;
    auto &to = direction == Move::Undo ? undone_ : edits_;
    const auto found = std::ranges::find_if(from, [=](const Record &record) {
        return record.history == history && record.serial == serial && serial != 0;
    });
    if (found == from.end()) return; // Another panel's entry.
    to.push_back(*found);
    from.erase(found);
    // An external action may move an owned entry too. Keep the original
    // submission key, even then; Redo visits the oldest undone entry first.
    std::ranges::sort(to, [direction](const Record &a, const Record &b) {
        return direction == Move::Undo ? a.sent > b.sent : a.sent < b.sent;
    });
}

bool TabEditOrder::available(bool redo) const {
    const auto &records = redo ? undone_ : edits_;
    if (records.empty() || records.back().serial == 0) return false;
    const auto &record = records.back();
    if (record.history == History::Graph)
        return (redo ? graph_.canRedo() : graph_.canUndo()) &&
               (redo ? graph_.redoSerial() : graph_.undoSerial()) == record.serial;
    const auto &enabled = redo ? renderRedoAvailable : renderUndoAvailable;
    // Keep a mismatching top actionable so the tab can explain the block.
    return render_ && (!enabled || enabled()) && (redo ? render_->canRedo() : render_->canUndo());
}

bool TabEditOrder::move(bool redo) {
    if (!available(redo)) return false;
    const auto &record = (redo ? undone_ : edits_).back();
    if (record.history == History::Graph) return redo ? graph_.redo() : graph_.undo();
    const auto &stack = redo ? render_->redoStack() : render_->undoStack();
    const auto &records = redo ? undone_ : edits_;
    // Independent documents can be observed in a different order from the
    // tab's submissions. Restore the shared top if this tab owns it, even
    // when its record is not last. Digest-linked edits are already ordered
    // by the shared history; moved() transfers the matching record on success.
    const bool owned = !stack.empty() && std::ranges::any_of(records, [&](const Record &candidate) {
        return candidate.history == History::Render && candidate.serial != 0 &&
               candidate.serial == stack.back().serial;
    });
    if (!owned) {
        graph_.setEditNotice({InspectorNoticeKind::Information,
            redo ? "A later render edit from another panel is on top. Redo it there first, or refresh."
                 : "A later render edit from another panel is on top. Undo it there first, or refresh."});
        return false;
    }
    graph_.keepHistoryThroughNextDigest();
    const bool started = redo ? render_->redo() : render_->undo();
    if (!started && !render_->line().empty())
        graph_.setEditNotice({InspectorNoticeKind::Information, render_->line()});
    return started;
}

bool TabEditOrder::undo() { return move(false); }
bool TabEditOrder::redo() { return move(true); }
bool TabEditOrder::canUndo() const { return available(false); }
bool TabEditOrder::canRedo() const { return available(true); }

void TabEditOrder::forget(History history) {
    for (auto *records : {&edits_, &undone_})
        std::erase_if(*records, [history](const Record &record) { return record.history == history; });
}

void TabEditOrder::forgetGraph() {
    graph_pending_ = 0;
    forget(History::Graph);
}

void TabEditOrder::clear() {
    graph_pending_ = 0;
    edits_.clear();
    undone_.clear();
}

} // namespace PelicanStudio
