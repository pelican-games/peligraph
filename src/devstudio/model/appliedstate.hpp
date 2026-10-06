#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// RT-C. The one reading of what a synchronous project-document write did in
// the running engine: the Materials dock's edits and binds (engine MT-1 /
// MT-3 / EB-6), register_model / unregister_model (MI-1) and
// edit_project_settings (HW-2 / AV-1). The engine answers
//   applied: "live" | "rebuilt" | "restart_required" | "unusable"
//   restart_required: null | "<reason>" | {reason}
//   problem (unusable), rebuilt_models (rebuilt).
// edit_project_settings' `applied` is an object (its hot_reload and
// asset_stores), which reads here as Live; its caller reads the object itself.
// What each caller says for each state stays with the caller.
struct AppliedState {
    enum class Kind { Live, Rebuilt, RestartRequired, Unusable };
    Kind kind = Kind::Live;
    // RestartRequired: the engine's reason, else the caller's default.
    std::string restart_reason;
    // Unusable: the answer's `problem` (an empty object when it has none).
    nlohmann::json problem = nlohmann::json::object();
    // Rebuilt: the models drawn again in place, one entry per model the
    // engine listed (an entry that is not a string reads as "").
    std::vector<std::string> rebuilt_models;
};

// In this order: `applied` "unusable" -> Unusable; else `restart_required`
// present and not null -> RestartRequired (a string is the reason, an object
// its `reason`; empty -> `default_restart_reason`); else `applied` "rebuilt"
// -> Rebuilt; else Live ("live", no `applied`, anything else). Anything that
// is not an object reads as Live.
[[nodiscard]] AppliedState readAppliedState(const nlohmann::json &result,
                                            std::string_view default_restart_reason);

} // namespace PelicanStudio
