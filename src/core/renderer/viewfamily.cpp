#include "viewfamily.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

std::set<std::string, std::less<>> validatedViewIds(
    const RenderViewFamily &family) {
    validateRenderViewFamilyId(
        family.family_id,
        "render view family");
    if (family.views.empty()) {
        throw std::runtime_error(
            "render view family '" + family.family_id +
            "' requires at least one view");
    }
    if (family.views.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "render view family '" + family.family_id +
            "' exceeds the public view index range");
    }

    std::set<std::string, std::less<>> ids;
    for (const auto &view : family.views) {
        if (view.view_id.empty()) {
            throw std::runtime_error(
                "render view family '" + family.family_id +
                "' contains a view without a stable view_id");
        }
        if (!ids.insert(view.view_id).second) {
            throw std::runtime_error(
                "render view family '" + family.family_id +
                "' contains duplicate view_id '" + view.view_id + "'");
        }
    }
    return ids;
}

} // namespace

RenderViewFamily makeMainRenderViewFamily(RenderViewParameters view) {
    if (view.view_id.empty()) {
        view.view_id = monoRenderViewId;
    }
    RenderViewFamily result{
        .family_id = std::string{mainRenderViewFamilyId},
        .views = {},
    };
    result.views.push_back(std::move(view));
    return result;
}

const RenderViewFamily *RenderViewFamilies::find(
    std::string_view family_id) const noexcept {
    const auto found = std::find_if(
        families.begin(), families.end(),
        [family_id](const RenderViewFamily &family) {
            return family.family_id == family_id;
        });
    return found != families.end()
               ? &*found
               : nullptr;
}

const RenderViewFamily &RenderViewFamilies::require(
    std::string_view family_id) const {
    if (const auto *family = find(family_id)) {
        return *family;
    }
    throw std::runtime_error(
        "render view families do not provide family '" +
        std::string{family_id} + "'");
}

RenderViewFamilies makeMainRenderViewFamilies(
    RenderViewFamily main_family) {
    if (main_family.family_id !=
        mainRenderViewFamilyId) {
        throw std::runtime_error(
            "main render view family must use family_id '$main'");
    }
    RenderViewFamilies result;
    result.families.push_back(
        std::move(main_family));
    return result;
}

void validateRenderViewFamilies(
    const RenderViewFamilies &families,
    const CompiledGraphVariantPolicy &policy) {
    if (families.families.empty()) {
        throw std::runtime_error(
            "render view families require '$main'");
    }
    std::set<std::string, std::less<>>
        family_ids;
    for (const auto &family : families.families) {
        (void)validatedViewIds(family);
        if (!family_ids.insert(
                 family.family_id)
                 .second) {
            throw std::runtime_error(
                "render view families contain duplicate family_id '" +
                family.family_id + "'");
        }
    }
    validateRenderViewFamily(
        families.require(
            mainRenderViewFamilyId),
        policy);
}

void validateRenderViewFamily(
    const RenderViewFamily &family,
    const CompiledGraphVariantPolicy &policy) {
    (void)validatedViewIds(family);
    const auto view_count =
        static_cast<std::uint32_t>(family.views.size());
    if (policy.view_count != 0 &&
        view_count != policy.view_count) {
        throw std::runtime_error(
            "render graph variant '" +
            std::string{renderPipelineGraphVariantName(policy.variant)} +
            "' requires " + std::to_string(policy.view_count) +
            " views in family '" + family.family_id + "'");
    }
    if (policy.view_family == GraphVariantViewFamily::mono &&
        view_count != 1) {
        throw std::runtime_error(
            "render graph variant requires a mono view family");
    }
    if (policy.view_family == GraphVariantViewFamily::stereo &&
        view_count != 2) {
        throw std::runtime_error(
            "render graph variant requires a stereo view family");
    }
}

bool TemporalViewFamilyHistory::hasValidView() const noexcept {
    return std::any_of(
        views.begin(), views.end(),
        [](const auto &entry) { return entry.second.valid; });
}

TemporalViewFamilyChange synchronizeTemporalViewFamilyHistory(
    TemporalViewFamilyHistory &history,
    const RenderViewFamily &family) {
    const auto view_ids = validatedViewIds(family);
    const bool same_membership =
        history.family_id == family.family_id &&
        history.views.size() == view_ids.size() &&
        std::equal(
            history.views.begin(), history.views.end(),
            view_ids.begin(),
            [](const auto &history_entry,
               const std::string &view_id) {
                return history_entry.first == view_id;
            });
    if (same_membership) {
        std::vector<std::string> execution_order;
        execution_order.reserve(family.views.size());
        for (const auto &view : family.views) {
            execution_order.push_back(view.view_id);
        }
        if (history.execution_order ==
            execution_order) {
            return TemporalViewFamilyChange::none;
        }
        history.execution_order =
            std::move(execution_order);
        return TemporalViewFamilyChange::
            execution_order;
    }

    history.family_id = family.family_id;
    history.views.clear();
    history.execution_order.clear();
    history.execution_order.reserve(
        family.views.size());
    for (const auto &view : family.views) {
        history.execution_order.push_back(
            view.view_id);
    }
    for (const auto &view_id : view_ids) {
        history.views.emplace(view_id, TemporalFrameHistory{});
    }
    return TemporalViewFamilyChange::topology;
}

std::vector<RenderFrameSnapshot> buildRenderViewFamilySnapshots(
    const TemporalViewFamilyHistory &history,
    const RenderViewFamily &family,
    const RenderViewFamilyProjectionModifiers &modifiers,
    const CompiledGraphVariantPolicy &policy,
    std::uint64_t frame_index,
    std::uint32_t render_width,
    std::uint32_t render_height,
    bool reset_requested) {
    (void)validatedViewIds(family);
    if (family.family_id ==
        mainRenderViewFamilyId) {
        validateRenderViewFamily(
            family, policy);
    }
    if (history.family_id != family.family_id) {
        throw std::runtime_error(
            "render view family history was not synchronized for family '" +
            family.family_id + "'");
    }
    if (modifiers.projection_jitter &&
        policy.projection_jitter ==
            GraphVariantProjectionJitterPolicy::forbid) {
        throw std::runtime_error(
            "render graph variant '" +
            std::string{renderPipelineGraphVariantName(policy.variant)} +
            "' forbids projection jitter for family '" +
            family.family_id + "'");
    }

    glm::vec2 jitter_ndc{0.0f};
    if (modifiers.projection_jitter) {
        jitter_ndc = projectionJitterSample(
                         *modifiers.projection_jitter,
                         frame_index, render_width,
                         render_height)
                         .jitter_ndc;
    }

    std::vector<RenderFrameSnapshot> result;
    result.reserve(family.views.size());
    for (const auto &view : family.views) {
        const auto history_entry =
            history.views.find(view.view_id);
        if (history_entry == history.views.end()) {
            throw std::runtime_error(
                "render view family history is missing view_id '" +
                view.view_id + "'");
        }
        result.push_back(buildRenderFrameSnapshot(
            history_entry->second, view.projection, view.view,
            view.camera_position, jitter_ndc, reset_requested));
    }
    return result;
}

void commitRenderViewFamilySnapshots(
    TemporalViewFamilyHistory &history,
    const RenderViewFamily &family,
    const std::vector<RenderFrameSnapshot> &snapshots) {
    if (snapshots.size() != family.views.size()) {
        throw std::runtime_error(
            "render view family snapshot count does not match its views");
    }
    for (std::size_t index = 0; index < family.views.size();
         ++index) {
        const auto &view = family.views[index];
        const auto history_entry =
            history.views.find(view.view_id);
        if (history_entry == history.views.end()) {
            throw std::runtime_error(
                "render view family history is missing view_id '" +
                view.view_id + "'");
        }
        commitRenderFrameSnapshot(
            history_entry->second, snapshots[index]);
    }
}

} // namespace Pelican
