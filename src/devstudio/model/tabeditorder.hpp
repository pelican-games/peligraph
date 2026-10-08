#pragma once

#include "graphcanvasmodel.hpp"
#include "renderedithistory.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace PelicanStudio {

// One Graph tab's submissions, independently of the two histories' polling
// order. A zero serial reserves a position until that submission terminates.
class TabEditOrder {
  public:
    enum class History { Graph, Render };
    struct Record {
        History history;
        std::uint64_t sent;
        std::uint64_t serial = 0;
        // Render submissions at the time this Graph edit was sent. Retained
        // across Undo/Redo, independently of commit notification order.
        std::uint64_t render_submission = 0;
        // Entries already undone when this Graph edit was submitted are on
        // an older branch, not prerequisites for replaying this edit.
        std::vector<std::uint64_t> render_already_undone;
        bool operator==(const Record &) const = default;
    };

    explicit TabEditOrder(GraphCanvasModel &graph);
    TabEditOrder(const TabEditOrder &) = delete;
    TabEditOrder &operator=(const TabEditOrder &) = delete;
    void setRenderHistory(RenderEditHistory *history);
    void renderRequestSent(std::int64_t id);
    void recordFiles(std::string label, std::vector<RenderEditHistory::FileChange> files);

    // The view can further disable the actions (or disconnect them).
    std::function<bool()> renderUndoAvailable;
    std::function<bool()> renderRedoAvailable;
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    void clear();
    void forgetGraph();
    [[nodiscard]] const std::vector<Record> &undoRecords() const { return edits_; }
    [[nodiscard]] const std::vector<Record> &redoRecords() const { return undone_; }

  private:
    using Move = RenderEditHistory::Move;
    GraphCanvasModel &graph_;
    RenderEditHistory *render_ = nullptr;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::shared_ptr<RenderEditHistory::Movement> render_connection_;
    std::uint64_t next_sent_ = 1;
    std::uint64_t graph_pending_ = 0;
    std::vector<Record> edits_;
    std::vector<Record> undone_;

    std::uint64_t submitted(History history);
    void complete(std::uint64_t sent, std::uint64_t serial);
    RenderEditHistory::Completion completion(std::uint64_t sent);
    void moved(History history, std::uint64_t serial, Move direction, bool ok);
    void forget(History history);
    [[nodiscard]] bool available(bool redo) const;
    bool move(bool redo);
};

} // namespace PelicanStudio
