#pragma once

#include "editorjournal.hpp"

#include <memory>

namespace Pelican {

class EditorCommandService;

struct EditorRuntimeServiceOptions {
    // The config editor is intentionally exposed only by the RPC composition
    // used by Studio.  The existing ImGui composition keeps its current
    // surface while sharing all scene-edit implementation.
    bool render_config_editing = false;
    // Optional deterministic transaction seam used by production-composition
    // regression tests. Normal editor compositions leave this null.
    EditorProjectionFaultInjector *projection_fault_injector = nullptr;
};

namespace internal {

// Centralizes the BEH2 G2 policy so acceptance and execution observe the
// same stable reject while a behavior callback or game-logic reload owns the
// lifecycle boundary.
EditorGateObservation applyBehaviorEditConcurrencyGate(
    EditorGateObservation observation, bool behavior_callback_active,
    bool game_logic_reload_active, bool reload_reconciling) noexcept;

// Exact production isolation oracle used by lifecycle regression tests. It
// shares the eval-preview snapshot implementation, covering ECS, camera,
// light, physics, and behavior attachment state.
nlohmann::ordered_json editorRuntimeStateSnapshotForTesting();

} // namespace internal

// Creates the single production editor composition used by either the RPC
// endpoint or the interactive ImGui runtime. Deterministic drivers never
// create the interactive runtime, so the editable surface remains absent.
std::unique_ptr<EditorCommandService> makeEditorRuntimeService(
    EditorRuntimeServiceOptions options = {});

} // namespace Pelican
