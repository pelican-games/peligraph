#include "appliedstate.hpp"

#include <utility>

namespace PelicanStudio {
namespace {

std::string text(const nlohmann::json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

} // namespace

AppliedState readAppliedState(const nlohmann::json &result, std::string_view default_restart_reason) {
    AppliedState state;
    if (!result.is_object()) return state;
    const auto applied = text(result, "applied");
    if (applied == "unusable") {
        state.kind = AppliedState::Kind::Unusable;
        if (const auto problem = result.find("problem"); problem != result.end() && !problem->is_null())
            state.problem = *problem;
        return state;
    }
    if (const auto restart = result.find("restart_required"); restart != result.end() && !restart->is_null()) {
        state.kind = AppliedState::Kind::RestartRequired;
        state.restart_reason = restart->is_string() ? restart->get<std::string>() : text(*restart, "reason");
        if (state.restart_reason.empty()) state.restart_reason = std::string{default_restart_reason};
        return state;
    }
    if (applied == "rebuilt") {
        state.kind = AppliedState::Kind::Rebuilt;
        if (const auto rebuilt = result.find("rebuilt_models"); rebuilt != result.end() && rebuilt->is_array())
            for (const auto &model : *rebuilt)
                state.rebuilt_models.push_back(model.is_string() ? model.get<std::string>() : std::string{});
    }
    return state;
}

} // namespace PelicanStudio
