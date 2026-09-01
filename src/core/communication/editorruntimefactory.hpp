#pragma once

#include "editorjournal.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Pelican {

class BehaviorAttachmentArena;
class EditorCommandService;
enum class BehaviorRegistryAvailability;
struct EditorProjectionRuntimeObjectBinding;

struct EditorRuntimeServiceOptions {
    // The config editor is intentionally exposed only by the RPC composition
    // used by Studio.  The existing ImGui composition keeps its current
    // surface while sharing all scene-edit implementation.
    bool render_config_editing = false;
};

namespace internal {

struct EditorPrefabProjectionRoute {
    std::string scene_id;
    bool full_component_projection = false;
    OriginTransitionMap origin_transition;
    BehaviorRegistryAvailability behavior_availability;
};

// Shared production route planner. Runtime execution and CPU acceptance use
// this same typed decision, so an unpack operation cannot silently omit the
// behavior scene reconciliation adapter.
std::vector<EditorPrefabProjectionRoute> planEditorPrefabProjectionRoutes(
    std::span<const nlohmann::ordered_json> operations,
    const ResolvedScene &resolved, std::string_view current_scene,
    BehaviorRegistryAvailability behavior_availability);

// Shared final hop for prefab behavior projection. Runtime execution and CPU
// acceptance both use this function so the per-scene transition/availability
// table, its missing-entry defaults, and the adapter construction cannot drift.
std::vector<std::unique_ptr<EditorProjectionAdapter>>
makeEditorPrefabBehaviorProjectionAdapters(
    BehaviorAttachmentArena &arena,
    const std::vector<EditorProjectionRuntimeObjectBinding> &runtime_bindings,
    std::span<const EditorPrefabProjectionRoute> routes,
    BehaviorRegistryAvailability fallback_availability);

// Centralizes the BEH2 G2 policy so acceptance and execution observe the
// same stable reject while a behavior callback or game-logic reload owns the
// lifecycle boundary.
EditorGateObservation applyBehaviorEditConcurrencyGate(
    EditorGateObservation observation, bool behavior_callback_active,
    bool game_logic_reload_active, bool reload_reconciling) noexcept;

} // namespace internal

// Creates the single production editor composition used by either the RPC
// endpoint or the interactive ImGui runtime. Deterministic drivers never
// create the interactive runtime, so the editable surface remains absent.
std::unique_ptr<EditorCommandService> makeEditorRuntimeService(
    EditorRuntimeServiceOptions options = {});

} // namespace Pelican
