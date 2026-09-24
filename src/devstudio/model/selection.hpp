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

// View-independent, client-session selection state. The object identity stored
// here is the public contract shared by scene_tree, get_components,
// pick_object, and ProjectOutlinerModel: the scene id and the authoring object
// id, with the declaration index the object has now (OL-1). Selection
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
    // OL-1: after the project read a new scene tree. The selected object is
    // looked up by its authoring id: still there, the selection keeps it at
    // whatever declaration index it has now (changed when that moved); gone,
    // the selection is cleared (changed). Not a new selection, so a pick in
    // flight stays current.
    SelectionUpdate followProject();

    ViewportPickToken beginViewportPick();
    SelectionUpdate completeViewportPick(ViewportPickToken token,
                                          std::string_view result_json);
    SelectionUpdate failViewportPick(ViewportPickToken token,
                                      std::string message);
};

} // namespace PelicanStudio
