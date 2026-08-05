#pragma once

#include "project.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace PelicanStudio {

struct ViewportPickToken {
    std::uint64_t value = 0;

    bool operator==(const ViewportPickToken &) const = default;
};

enum class SelectionUpdateKind {
    changed,
    unchanged,
    stale,
    failed,
};

struct SelectionUpdate {
    SelectionUpdateKind kind = SelectionUpdateKind::unchanged;
    std::string message;

    bool applied() const noexcept {
        return kind == SelectionUpdateKind::changed ||
               kind == SelectionUpdateKind::unchanged;
    }
};

// View-independent, client-session selection state. The only object identity
// stored here is the public (scene_id, declaration_index) contract shared by
// scene_tree, get_components, pick_object, and ProjectOutlinerModel. Selection
// is intentionally not engine-authoritative state: any public RPC client can
// maintain the same local focus without a Studio-only engine entry point.
class SelectionModel {
    const ProjectOutlinerModel *project_ = nullptr;
    std::optional<OutlinerObjectKey> selected_;
    std::uint64_t revision_ = 0;

    void advanceRevision();
    SelectionUpdate applySelection(
        std::optional<OutlinerObjectKey> selection);

  public:
    void bindProject(const ProjectOutlinerModel *project);

    const std::optional<OutlinerObjectKey> &selected() const noexcept {
        return selected_;
    }

    SelectionUpdate selectFromOutliner(
        std::optional<OutlinerObjectKey> selection);

    ViewportPickToken beginViewportPick();
    SelectionUpdate completeViewportPick(ViewportPickToken token,
                                          std::string_view result_json);
    SelectionUpdate failViewportPick(ViewportPickToken token,
                                      std::string message);
};

} // namespace PelicanStudio
