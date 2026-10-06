#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

namespace PelicanStudio {

// RSX, decision 1. The one reading of a render config ticket's last answer
// (get_edit_result, or a submission the engine answered committed at once).
// Every panel that sends a render edit -- the Graph tab, the Frame Plan, the
// Pass Editor, Render Features and the dialogs it opens, Rendering settings,
// the Inspector's graph attachments -- and the render edit history's Undo /
// Redo read it here, so "the engine has to start again to run this" is heard
// the same way everywhere.
//
// Since RS-1 every render config ticket the runtime takes says, when it
// committed, whether it could be run live: `restart_required: true` with
// `restart_reason {code, ...}` when it could not (a material's physical
// contract, G-4's material_route_default). The edit is written either way;
// the engine that comes back after a restart runs it.
struct RenderEditCompletion {
    // The answer's `status` ("committed", "rejected", "pending", ...).
    std::string status;
    // status "committed" and `committed: true`.
    bool committed = false;
    // Committed, and nothing in the documents changed.
    bool no_change = false;
    // Committed, and the engine runs it only after a restart.
    bool restart_required = false;
    // restart_reason.code; empty when the engine named none. RS-1's `detail`
    // is a list (color_formats, depth_format, ...) and is not read: the code
    // says enough for one line.
    std::string restart_code;
    std::uint64_t published_generation = 0;
    // source_digest.hex; empty when the answer has none.
    std::string source_digest;
    // What the panel that sent the edit says while the engine restarts:
    // "Restarting the engine to apply this change (<code>)...". Empty when
    // no restart is asked for.
    std::string restart_line;
    // RT-C: committed, and applying it in the running engine failed after the
    // commit (the renderer's post-publication cleanup threw). The documents
    // are written and the generation published either way. Empty when the
    // answer has none, and for an answer that did not commit. A present
    // non-string is reported as a response format error.
    std::string post_commit_error;
    // The answer's `ticket`, whatever its status; empty when it has none.
    std::string ticket;
};

// The panels' line for a restart, with the reason's code when there is one.
[[nodiscard]] std::string renderEditRestartLine(std::string_view code);

// Reads one answer; anything that is not an object reads as nothing
// committed.
[[nodiscard]] RenderEditCompletion readRenderEditCompletion(const nlohmann::json &response);

// Reads one answer and, when it committed with restart_required, hands its
// restart_line to `restart` -- the main window's requestEngineRestart, the
// same restart the Graph tab and the Frame Plan always asked for (it asks
// about unsaved scene edits first). Nothing is called otherwise.
RenderEditCompletion completeRenderEdit(
    const nlohmann::json &response,
    const std::function<void(const std::string &line)> &restart);

// RT-C: the main window's one line for a commit with a post_commit_error:
// "Saved. Applying it in the running engine failed after the commit:
// <error>", and the restart line after it when the commit also asks for a
// restart. Empty when there is no post_commit_error.
[[nodiscard]] std::string renderEditPostCommitErrorLine(const RenderEditCompletion &completion);

// RT-C: post_commit_error said once per ticket. A ticket's last answer comes
// to the main window's RPC observer once for each reader that polls it (the
// panel that sent the edit and the render edit history), so the second one
// for the same commit says nothing.
class RenderEditPostCommitErrors {
  public:
    // The line to show for `response` (renderEditPostCommitErrorLine), or
    // empty: it did not commit, has no post_commit_error, or its commit's
    // line was given already.
    [[nodiscard]] std::string take(const nlohmann::json &response);
    // Ticket / generation / root digest can all repeat in another engine.
    // Forget the old session's notifications when its RPC connection goes.
    void engineGone() { said_.clear(); }

  private:
    // Within one engine session a commit is its ticket, generation and
    // digest. The last few are enough: the repeats come together.
    std::deque<std::string> said_;
};

} // namespace PelicanStudio
