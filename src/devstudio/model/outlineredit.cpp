#include "outlineredit.hpp"

#include <limits>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::optional<std::uint64_t> unsignedField(const Json &object, const char *field) {
    const auto found = object.find(field);
    if (found == object.end()) return std::nullopt;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>();
    if (found->is_number_integer() && found->get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(found->get<std::int64_t>());
    return std::nullopt;
}

} // namespace

Json duplicateObjectOperation(const OutlinerObject &object) {
    return {{"op", "duplicate"}, {"object_id", object.key.authoring_object_id}};
}

Json destroyObjectOperation(const OutlinerObject &object) {
    return {{"op", "destroy"}, {"object_id", object.key.authoring_object_id}};
}

Json reparentObjectOperation(const OutlinerObject &object, const OutlinerObject *new_parent,
                             ReparentPreserve preserve) {
    return {{"op", "reparent"},
            {"object_id", object.key.authoring_object_id},
            {"new_parent_id",
             new_parent != nullptr ? Json(new_parent->key.authoring_object_id) : Json(nullptr)},
            {"preserve", preserve == ReparentPreserve::Local ? "local" : "world"}};
}

std::optional<OutlinerObjectKey> selectionAfterDestroy(const ProjectOutlinerModel &project,
                                                       const OutlinerObject &object) {
    if (const OutlinerObject *parent = project.parentOf(object)) return parent->key;
    const OutlinerScene *scene = project.findScene(object.key.scene_id);
    if (scene == nullptr) return std::nullopt;
    const OutlinerObject *previous = nullptr;
    for (const auto &candidate : scene->objects) {
        if (candidate.key.declaration_index >= object.key.declaration_index) break;
        if (candidate.parent_declaration_index == object.parent_declaration_index)
            previous = &candidate;
    }
    if (previous == nullptr) return std::nullopt;
    return previous->key;
}

OutlinerDropPlan planOutlinerDrop(const ProjectOutlinerModel &project,
                                  const OutlinerObjectKey &dragged,
                                  const std::optional<OutlinerObjectKey> &onto,
                                  ReparentPreserve preserve) {
    const OutlinerObject *child = project.findObject(dragged);
    if (child == nullptr) return {.kind = OutlinerDropKind::Unchanged};
    const OutlinerObject *current_parent = project.parentOf(*child);
    if (!onto) {
        if (current_parent == nullptr) return {.kind = OutlinerDropKind::Unchanged};
        if (child->name.empty())
            return {.kind = OutlinerDropKind::Refused,
                    .refusal = child->display_name +
                               " has no name, so it cannot be moved to another parent."};
        return {.kind = OutlinerDropKind::Reparent,
                .operation = reparentObjectOperation(*child, nullptr, preserve)};
    }
    const OutlinerObject *parent = project.findObject(*onto);
    if (parent == nullptr || parent == child || parent == current_parent)
        return {.kind = OutlinerDropKind::Unchanged};
    if (parent->key.scene_id != child->key.scene_id)
        return {.kind = OutlinerDropKind::Refused,
                .refusal = child->display_name + " cannot be moved into another scene."};
    if (project.isSelfOrDescendant(*child, *parent))
        return {.kind = OutlinerDropKind::Refused,
                .refusal = child->display_name + " cannot go under " + parent->display_name +
                           ": " + parent->display_name + " is inside " +
                           child->display_name + "."};
    if (child->name.empty())
        return {.kind = OutlinerDropKind::Refused,
                .refusal = child->display_name +
                           " has no name, so it cannot be moved to another parent."};
    if (parent->name.empty())
        return {.kind = OutlinerDropKind::Refused,
                .refusal = parent->display_name +
                           " has no name, so nothing can be placed under it."};
    return {.kind = OutlinerDropKind::Reparent,
            .operation = reparentObjectOperation(*child, parent, preserve)};
}

std::optional<OutlinerObjectKey> createdObjectToSelect(const Json &created_objects,
                                                       std::uint64_t source_object_id) {
    if (!created_objects.is_array()) return std::nullopt;
    for (const auto &entry : created_objects) {
        if (!entry.is_object()) continue;
        const auto object_id = unsignedField(entry, "object_id");
        const auto index = unsignedField(entry, "declaration_index");
        const auto scene = entry.find("scene_id");
        if (!object_id || *object_id == 0 || !index ||
            *index > std::numeric_limits<std::size_t>::max() || scene == entry.end() ||
            !scene->is_string())
            continue;
        if (source_object_id != 0 && unsignedField(entry, "source_object_id") != source_object_id)
            continue;
        return OutlinerObjectKey{.scene_id = scene->get<std::string>(),
                                 .declaration_index = static_cast<std::size_t>(*index),
                                 .authoring_object_id = *object_id};
    }
    return std::nullopt;
}

} // namespace PelicanStudio
