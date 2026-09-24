#pragma once

#include "project.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace PelicanStudio {

// OL-1: what the Outliner's Duplicate, Delete and drag send, and what it does
// with the answer. No Qt here: the rows are the project's objects, and every
// operation names its object by authoring id, the key a duplicate, a delete or
// an undo does not move.

// {"op":"duplicate","object_id":N}. The engine copies the subtree and places it
// right after the original's; the copy is named <name>_copy<N>.
[[nodiscard]] nlohmann::json duplicateObjectOperation(const OutlinerObject &object);

// {"op":"destroy","object_id":N}. The object's descendants go with it.
[[nodiscard]] nlohmann::json destroyObjectOperation(const OutlinerObject &object);

enum class ReparentPreserve : std::uint8_t {
    // The object stays where it is on screen: its transform is recomputed
    // under the new parent. What a plain drag does.
    World,
    // The object keeps the transform it has written down, so it moves with
    // the new parent. What an Alt-drag does.
    Local,
};

// {"op":"reparent","object_id":N,"new_parent_id":M|null,"preserve":...};
// a null parent is the scene's root.
[[nodiscard]] nlohmann::json reparentObjectOperation(const OutlinerObject &object,
                                                     const OutlinerObject *new_parent,
                                                     ReparentPreserve preserve);

// Where the selection goes when an object is deleted: its parent, else the
// sibling declared before it, else nowhere.
[[nodiscard]] std::optional<OutlinerObjectKey>
selectionAfterDestroy(const ProjectOutlinerModel &project, const OutlinerObject &object);

enum class OutlinerDropKind : std::uint8_t {
    // Already there: dropped on its own parent, or on the root while at it.
    Unchanged,
    // One reparent, in `operation`.
    Reparent,
    // Studio will not send it; `refusal` says why in one line.
    Refused,
};

struct OutlinerDropPlan {
    OutlinerDropKind kind = OutlinerDropKind::Unchanged;
    nlohmann::json operation;
    std::string refusal;
};

// A row dropped onto another row (`onto`) becomes its child; dropped between
// rows (`onto` empty) it goes to its scene's root. Onto itself or anything
// under it is refused here, before the engine would answer cycle_detected;
// so are a drop into another scene and an object with no name, which no parent
// reference can name.
[[nodiscard]] OutlinerDropPlan planOutlinerDrop(const ProjectOutlinerModel &project,
                                                const OutlinerObjectKey &dragged,
                                                const std::optional<OutlinerObjectKey> &onto,
                                                ReparentPreserve preserve);

// The object to select from a committed edit's `created_objects`: the copy
// made of `source_object_id` when one is given (a duplicate creates the whole
// subtree; its root is the one whose source is the object duplicated), else
// the first one listed. Empty when the edit created nothing.
[[nodiscard]] std::optional<OutlinerObjectKey>
createdObjectToSelect(const nlohmann::json &created_objects,
                      std::uint64_t source_object_id = 0);

} // namespace PelicanStudio
