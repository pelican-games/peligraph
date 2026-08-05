#include "selection.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

SelectionUpdate failed(std::string message) {
    return {
        .kind = SelectionUpdateKind::failed,
        .message = std::move(message),
    };
}

std::optional<std::uint64_t> unsignedInteger(const Json &value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            return static_cast<std::uint64_t>(signed_value);
        }
    }
    return std::nullopt;
}

} // namespace

void SelectionModel::advanceRevision() {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("selection revision exhausted");
    }
    ++revision_;
}

SelectionUpdate SelectionModel::applySelection(
    std::optional<OutlinerObjectKey> selection) {
    if (selection &&
        (project_ == nullptr || project_->findObject(*selection) == nullptr)) {
        return failed(
            "selection does not identify an object in the opened project");
    }

    if (selected_ == selection) {
        return {.kind = SelectionUpdateKind::unchanged};
    }
    selected_ = std::move(selection);
    return {.kind = SelectionUpdateKind::changed};
}

void SelectionModel::bindProject(const ProjectOutlinerModel *project) {
    advanceRevision();
    project_ = project;
    selected_.reset();
}

SelectionUpdate SelectionModel::selectFromOutliner(
    std::optional<OutlinerObjectKey> selection) {
    advanceRevision();
    return applySelection(std::move(selection));
}

ViewportPickToken SelectionModel::beginViewportPick() {
    advanceRevision();
    return {.value = revision_};
}

SelectionUpdate SelectionModel::completeViewportPick(
    ViewportPickToken token, std::string_view result_json) {
    if (token.value == 0 || token.value != revision_) {
        return {.kind = SelectionUpdateKind::stale};
    }
    advanceRevision();

    Json result;
    try {
        result = Json::parse(result_json);
    } catch (const Json::exception &error) {
        return failed("pick_object returned invalid JSON: " +
                      std::string{error.what()});
    }
    if (!result.is_object()) {
        return failed("pick_object result must be an object");
    }

    const auto contract = result.find("contract");
    if (contract == result.end() || unsignedInteger(*contract) != 1) {
        return failed("pick_object result requires contract 1");
    }

    const auto hit = result.find("hit");
    if (hit == result.end()) {
        return failed("pick_object result requires field 'hit'");
    }
    if (hit->is_null()) {
        // A successful background pick is an explicit selection clear. RPC
        // failure is handled separately and preserves the previous selection.
        return applySelection(std::nullopt);
    }
    if (!hit->is_object()) {
        return failed("pick_object result field 'hit' must be an object or null");
    }

    const auto scene_id = hit->find("scene_id");
    const auto declaration_index = hit->find("declaration_index");
    if (scene_id == hit->end() || !scene_id->is_string() ||
        scene_id->get_ref<const std::string &>().empty()) {
        return failed(
            "picked object has no scene_id and cannot be matched to the outliner");
    }
    if (declaration_index == hit->end()) {
        return failed(
            "picked object has no declaration_index and cannot be matched to the outliner");
    }
    const auto index = unsignedInteger(*declaration_index);
    if (!index || *index > std::numeric_limits<std::size_t>::max()) {
        return failed("picked object declaration_index is not a valid unsigned index");
    }

    return applySelection(OutlinerObjectKey{
        .scene_id = scene_id->get<std::string>(),
        .declaration_index = static_cast<std::size_t>(*index),
    });
}

SelectionUpdate SelectionModel::failViewportPick(ViewportPickToken token,
                                                 std::string message) {
    if (token.value == 0 || token.value != revision_) {
        return {.kind = SelectionUpdateKind::stale};
    }
    advanceRevision();
    if (message.empty()) {
        message = "pick_object failed without an error message";
    }
    return failed(std::move(message));
}

} // namespace PelicanStudio
