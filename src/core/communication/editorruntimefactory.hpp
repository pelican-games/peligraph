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
};

namespace internal {

// Centralizes the BEH2 G2 policy so acceptance and execution observe the
// same stable reject while a behavior callback or game-logic reload owns the
// lifecycle boundary.
EditorGateObservation applyBehaviorEditConcurrencyGate(
    EditorGateObservation observation, bool behavior_callback_active,
    bool game_logic_reload_active, bool reload_reconciling) noexcept;

// Exact production state oracle used by lifecycle regression tests. It is the
// same snapshot provider used to enforce eval_preview isolation, so camera,
// light, physics, ECS, and behavior attachment state cannot silently diverge.
nlohmann::ordered_json editorRuntimeStateSnapshotForTesting();

} // namespace internal

// Creates the single production editor composition used by either the RPC
// endpoint or the interactive ImGui runtime. Deterministic drivers never
// create the interactive runtime, so the editable surface remains absent.
std::unique_ptr<EditorCommandService> makeEditorRuntimeService(
    EditorRuntimeServiceOptions options = {});

} // namespace Pelican
