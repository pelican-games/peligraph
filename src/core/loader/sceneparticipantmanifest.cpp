#include "sceneparticipantmanifest.hpp"
#include "sceneload.hpp"

#include <array>

namespace Pelican {
namespace {

#if PELICAN_WITH_PHYSICS
constexpr auto physics_presence = SceneParticipantPresence::real;
#else
constexpr auto physics_presence = SceneParticipantPresence::stub;
#endif

#if PELICAN_WITH_SEQPLAYER
constexpr auto seq_presence = SceneParticipantPresence::real;
#else
constexpr auto seq_presence = SceneParticipantPresence::stub;
#endif

#if PELICAN_WITH_VAT
constexpr auto vat_presence = SceneParticipantPresence::real;
#else
constexpr auto vat_presence = SceneParticipantPresence::stub;
#endif

#if PELICAN_WITH_OPENXR
constexpr auto xr_presence = SceneParticipantPresence::real;
#else
constexpr auto xr_presence = SceneParticipantPresence::absent;
#endif

#if PELICAN_WITH_AUDIO
constexpr auto audio_presence = SceneParticipantPresence::real;
#else
constexpr auto audio_presence = SceneParticipantPresence::absent;
#endif

#if PELICAN_WITH_RPC
constexpr auto rpc_presence = SceneParticipantPresence::real;
#else
constexpr auto rpc_presence = SceneParticipantPresence::absent;
#endif

constexpr auto participant = SceneManifestRole::participant;
constexpr auto real = SceneParticipantPresence::real;
constexpr std::string_view swap_publish = "publish_candidates:noexcept-swap";
constexpr std::string_view inverse_rollback = "inverse-token:noexcept";
constexpr std::string_view swap_rollback = "candidate-swap:noexcept";
constexpr std::string_view no_retire = "none";

// This is intentionally verbose.  A grouped row such as "renderer state"
// would let a newly added counter escape both snapshots and mutation tests.
constexpr std::array manifest{
    SceneParticipantManifestEntry{"metadata", "document", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "authoring-semantic-v1", scene_load_mutation::metadata_document},
    SceneParticipantManifestEntry{"metadata", "projection", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "resolved-scene-v1", scene_load_mutation::metadata_projection},
    SceneParticipantManifestEntry{"metadata", "current_scene_id", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "scene-id-v1", {}},
    SceneParticipantManifestEntry{"metadata", "epoch", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "u64", {}},
    SceneParticipantManifestEntry{"metadata", "active_authority", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "run-nonce+epoch-v1", {}},
    SceneParticipantManifestEntry{"metadata", "prefab_provider", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "bindable-provider-v1", {}},
    SceneParticipantManifestEntry{"metadata", "prefab_provider_generation", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "u64", {}},
    SceneParticipantManifestEntry{"metadata", "persistence_baseline", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "scene-persistence-baseline-v1", {}},
    SceneParticipantManifestEntry{"metadata", "next_scene_revision", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "u64", {}},
    SceneParticipantManifestEntry{"metadata", "next_resolver_generation", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "u64", {}},
    SceneParticipantManifestEntry{"metadata", "next_authoring_object_id", real, participant, "prepare", "commit_metadata:single-swap", "discard-token", no_retire, "u64", {}},

    SceneParticipantManifestEntry{"scene_loader", "object_bindings", real, participant, "early_publish", swap_publish, swap_rollback, no_retire, "name+entity-v1", {}},
    SceneParticipantManifestEntry{"scene_loader", "runtime_object_bindings", real, participant, "early_publish", swap_publish, swap_rollback, no_retire, "authoring-id+entity-v1", {}},
    SceneParticipantManifestEntry{"scene_loader", "runtime_only_changes", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "bool", {}},
    SceneParticipantManifestEntry{"scene_loader", "transient_models", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "transient-model-count-v1", scene_load_mutation::transient_retire},

    SceneParticipantManifestEntry{"ecs", "entity_liveness", real, participant, "early_publish", "WP375-inverse-token:noexcept", inverse_rollback, "next-frame-retirement-queue", "entity-id-set-v1", scene_load_mutation::ecs_liveness},
    SceneParticipantManifestEntry{"ecs", "component_values", real, participant, "early_publish", "WP375-inverse-token:noexcept", inverse_rollback, "next-frame-retirement-queue", "component-semantic-v1", {}},
    SceneParticipantManifestEntry{"ecs", "component_versions", real, participant, "early_publish", "WP375-inverse-token:noexcept", inverse_rollback, "next-frame-retirement-queue", "component-version-v1", {}},

    SceneParticipantManifestEntry{"camera", "pos", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "vec3", {}},
    SceneParticipantManifestEntry{"camera", "dir", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "vec3", {}},
    SceneParticipantManifestEntry{"camera", "up", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "vec3", {}},
    SceneParticipantManifestEntry{"camera", "viewport_aspect", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "f32", {}},
    SceneParticipantManifestEntry{"camera", "viewport_width", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "u32", {}},
    SceneParticipantManifestEntry{"camera", "viewport_height", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "u32", {}},
    SceneParticipantManifestEntry{"camera", "projection", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "camera-projection-v1", {}},
    SceneParticipantManifestEntry{"camera", "sprite_policy", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "camera-sprite-policy-v1", {}},
    SceneParticipantManifestEntry{"camera", "projection_matrix", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "mat4", {}},
    SceneParticipantManifestEntry{"camera", "scene_cameras", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "named-camera-map-v1", {}},
    SceneParticipantManifestEntry{"camera", "controlled_scene_camera_order", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "ordered-camera-name-v1", {}},
    SceneParticipantManifestEntry{"camera", "active_camera_name", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "string", {}},
    SceneParticipantManifestEntry{"camera", "active_scene_camera_locked", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "bool", {}},
    SceneParticipantManifestEntry{"camera", "discontinuity_revision", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "u64", {}},

    SceneParticipantManifestEntry{"light", "directional_lights", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "directional-light-list-v1", {}},
    SceneParticipantManifestEntry{"light", "directional_names", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "name-index-map-v1", scene_load_mutation::light_directional_names},
    SceneParticipantManifestEntry{"light", "point_lights", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "point-light-list-v1", {}},
    SceneParticipantManifestEntry{"light", "point_names", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "name-index-map-v1", {}},
    SceneParticipantManifestEntry{"light", "spot_lights", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "spot-light-list-v1", {}},
    SceneParticipantManifestEntry{"light", "spot_names", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "name-index-map-v1", {}},

    SceneParticipantManifestEntry{"physics", "bindings", physics_presence, participant, "early_publish", swap_publish, swap_rollback, no_retire, "collider-binding-v1", {}},
    SceneParticipantManifestEntry{"physics", "active_trigger_pairs", physics_presence, participant, "prepare", swap_publish, swap_rollback, no_retire, "entity-pair-set-v1", {}},
    SceneParticipantManifestEntry{"physics", "next_collider_id", physics_presence, participant, "prepare", swap_publish, swap_rollback, no_retire, "u64", {}},

    SceneParticipantManifestEntry{"renderer.flat", "draw_inventory", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "draw-inventory-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "compiled_draw_queue", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "compiled-draw-queue-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "draw_queue_material_filters", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "material-filter-list-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "next_draw_declaration_ordinal", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "u64", {}},
    SceneParticipantManifestEntry{"renderer.flat", "ray_query_geometry_generation", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "u64", {}},
    SceneParticipantManifestEntry{"renderer.flat", "prepared_view_family_draws", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "view-family-draw-map-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "model_instances_data", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "mat4-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "previous_model_instances_data", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "mat4-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "model_history_valid", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "bool-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "skin_palettes", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "mat4-list-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "previous_skin_palettes", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "mat4-list-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "animation_revisions", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "u64-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "previous_animation_revisions", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "u64-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "animation_generations", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "u32-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "instance_slots", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "slot-generation+liveness-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "model_asset_ids", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "model-asset-id-list-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "ray_authoring_object_ids", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "u64-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "ray_instance_declarations", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "ray-instance-list-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "material_initial_value_tables", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "material-initial-table-list-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "vrm_semantics", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "vrm-semantic-list-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "morph_layouts", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "morph-layout-list-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "morph_weight_frames", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "morph-frame-list-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "morph_history_valid", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "bool-list", {}},
    SceneParticipantManifestEntry{"renderer.flat", "temporal_history_advance_count", real, participant, "prepare", swap_publish, swap_rollback, no_retire, "u64", {}},
    SceneParticipantManifestEntry{"renderer.flat", "material_override_frames", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "material-override-map-v1", {}},
    SceneParticipantManifestEntry{"renderer.flat", "material_absolute_override_frames", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "material-absolute-override-map-v1", {}},
    SceneParticipantManifestEntry{"renderer.xr", "scene_instance_projection", xr_presence, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "renderer-scene-v1", {}},
    SceneParticipantManifestEntry{"renderer.preview", "scene_instance_projection", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "renderer-scene-v1", {}},

    SceneParticipantManifestEntry{"seqplayer", "enabled", seq_presence, participant, "prepare", swap_publish, swap_rollback, "renderer-next-frame-retirement", "bool", {}},
    SceneParticipantManifestEntry{"seqplayer", "instances", seq_presence, participant, "prepare", swap_publish, swap_rollback, "renderer-next-frame-retirement", "model-instance-id-list-v1", {}},
    SceneParticipantManifestEntry{"vatplayer", "enabled", vat_presence, participant, "prepare", swap_publish, swap_rollback, "renderer-next-frame-retirement", "bool", {}},
    SceneParticipantManifestEntry{"vatplayer", "instance", vat_presence, participant, "prepare", swap_publish, swap_rollback, "renderer-next-frame-retirement", "optional-model-instance-id-v1", {}},

    SceneParticipantManifestEntry{"behavior_arena", "scene_attachments", real, participant, "prepare", "publish_candidates:noexcept", inverse_rollback, "finish:noexcept", "behavior-attachment-v1", scene_load_mutation::behavior_attachments},
    SceneParticipantManifestEntry{"behavior_arena", "next_handle", real, participant, "prepare", "publish_candidates:noexcept", inverse_rollback, no_retire, "u64", {}},
    SceneParticipantManifestEntry{"behavior_arena", "next_attachment_seq", real, participant, "prepare", "publish_candidates:noexcept", inverse_rollback, no_retire, "u64", {}},
    SceneParticipantManifestEntry{"event_outbox", "pending_events", real, participant, "prepare+activate", "flush:noexcept-preallocated-push", "discard-outbox", no_retire, "ordered-event-payload-v1", scene_load_mutation::event_pending},
    SceneParticipantManifestEntry{"rng_sequence", "current_seed", real, participant, "prepare", "flush:noexcept-copy", "discard-candidate", no_retire, "u64", scene_load_mutation::rng_current_seed},
    SceneParticipantManifestEntry{"rng_sequence", "state", real, participant, "prepare", "flush:noexcept-copy", "discard-candidate", no_retire, "u64", scene_load_mutation::rng_state},
    SceneParticipantManifestEntry{"rng_sequence", "increment", real, SceneManifestRole::invariant, "prepare", "flush:noexcept-copy", "discard-candidate", no_retire, "u64", {}},
    SceneParticipantManifestEntry{"pending_scene", "request", real, participant, "prepare+activate:candidate-aware", "flush:noexcept-swap", "discard-candidate", no_retire, "optional-scene-id-v1", scene_load_mutation::pending_scene},

    // Explicit non-participants are part of the manifest so tests cannot
    // silently promote caches, ID-A registry work, or WP380b work into this
    // transaction.
    SceneParticipantManifestEntry{"model_asset", "warm_cache", real, SceneManifestRole::cache, "lookup", "retained-on-failure", "none", no_retire, "not-compared", {}},
    SceneParticipantManifestEntry{"registry_slot", "candidate_registry", SceneParticipantPresence::absent, participant, "ID-A", "none", "none", no_retire, "absent", {}},
    SceneParticipantManifestEntry{"audio", "voice_and_handles", audio_presence, SceneManifestRole::excluded_wp380b, "WP380b", "none", "none", no_retire, "excluded", {}},
    SceneParticipantManifestEntry{"persistence", "bytes_and_settings", SceneParticipantPresence::absent, SceneManifestRole::excluded_wp380b, "WP380b", "none", "none", no_retire, "excluded", {}},

    SceneParticipantManifestEntry{"root.startup", "direct_load", real, SceneManifestRole::production_root, "Borrowed", "SceneLoader::load(request)", "result", no_retire, "root-wiring-v1", scene_load_mutation::root_startup},
    SceneParticipantManifestEntry{"root.rpc", "direct_load", rpc_presence, SceneManifestRole::production_root, "Borrowed", "SceneLoader::load(request)", "result", no_retire, "root-wiring-v1", rpc_presence == real ? scene_load_mutation::root_rpc : std::string_view{}},
    SceneParticipantManifestEntry{"root.pending", "apply", real, SceneManifestRole::production_root, "Borrowed:late-bound", "SceneLoader::load(request)", "retain-on-failure", no_retire, "root-wiring-v1", scene_load_mutation::root_pending},
    SceneParticipantManifestEntry{"root.import.unversioned", "callback", real, SceneManifestRole::production_root, "Owned", "SceneLoader::load(request)", "discard-token", no_retire, "root-wiring-v1", scene_load_mutation::root_import_unversioned},
    SceneParticipantManifestEntry{"root.import.versioned", "callback", real, SceneManifestRole::production_root, "Owned", "SceneLoader::load(request)", "discard-token", no_retire, "root-wiring-v1", scene_load_mutation::root_import_versioned},
};

} // namespace

std::span<const SceneParticipantManifestEntry>
sceneParticipantManifest() noexcept {
    return manifest;
}

std::string_view sceneParticipantPresenceName(
    SceneParticipantPresence presence) noexcept {
    switch (presence) {
    case SceneParticipantPresence::absent: return "absent";
    case SceneParticipantPresence::stub: return "stub";
    case SceneParticipantPresence::real: return "real";
    }
    return "unknown";
}

std::string_view sceneManifestRoleName(SceneManifestRole role) noexcept {
    switch (role) {
    case SceneManifestRole::participant: return "participant";
    case SceneManifestRole::invariant: return "invariant";
    case SceneManifestRole::cache: return "cache";
    case SceneManifestRole::production_root: return "production-root";
    case SceneManifestRole::excluded_wp380b: return "excluded-wp380b";
    }
    return "unknown";
}

} // namespace Pelican
