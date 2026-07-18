#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace Pelican {

enum class FramePhase : std::uint8_t {
    freeze_events,
    freeze_input,
    freeze_actions,
    deliver_events,
    update_game,
};

inline constexpr std::array frame_phase_order{
    FramePhase::freeze_events,
    FramePhase::freeze_input,
    FramePhase::freeze_actions,
    FramePhase::deliver_events,
    FramePhase::update_game,
};

template <class Operation> void forEachFramePhase(Operation &&operation) {
    for (const auto phase : frame_phase_order) {
        std::forward<Operation>(operation)(phase);
    }
}

// Shared by the windowed, fixed-step headless, and RPC step_frame loops.
// Called once by the composition root before freezing module creation.
void prepareFrameStateModules();

// Single-owner hook for the editor transaction queue. It runs after reload
// publication and immediately before freeze_events in every loop surface.
// Installation/removal happen outside frame execution; an absent hook is a
// zero-state no-op and does not create a module or alter the module graph.
using EditorCommitQueueHook = void (*)(void *context) noexcept;
bool installEditorCommitQueueHook(void *context,
                                  EditorCommitQueueHook hook) noexcept;
void removeEditorCommitQueueHook(void *context) noexcept;
void invokeEditorCommitQueueHook() noexcept;

void updateFrameState();

} // namespace Pelican
