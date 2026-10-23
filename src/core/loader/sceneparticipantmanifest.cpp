#include "sceneparticipantmanifest.hpp"
#include "sceneload.hpp"

#include <array>

namespace Pelican {
namespace {

constexpr auto stub = SceneParticipantPresence::stub;
constexpr auto absent = SceneParticipantPresence::absent;

// PG-2: one rule places every build module's share of a scene load in a
// build that left the module out.
//   - A module whose declarations can still reach a scene without it is
//     `stub`: the OFF build keeps the part that refuses the declaration by
//     name. `off_declaration` says what reaches the scene and what refuses it.
//   - A module nothing in a scene can ask for is `absent`.
// So off_presence == (off_declaration.empty() ? absent : stub), and the
// participant rows below take their presence from here instead of each
// picking its own. A module with no transaction field of its own (animation,
// whose component values ride on the ecs rows; the developer UI) still has
// its row, so no module is left unclassified.
constexpr std::array feature_divisions{
    SceneFeatureDivision{"physics", "PELICAN_WITH_PHYSICS",
        static_cast<bool>(PELICAN_WITH_PHYSICS),
        "collider component: refused at scene prepare and by the editor as "
        "physics.module_unavailable@1; PhysWorld's stub refuses any binding",
        stub},
    SceneFeatureDivision{"animation", "PELICAN_WITH_ANIMATION",
        static_cast<bool>(PELICAN_WITH_ANIMATION),
        "animation component: refused at scene prepare and by the editor as "
        "animation.module_unavailable@1",
        stub},
    SceneFeatureDivision{"audio", "PELICAN_WITH_AUDIO",
        static_cast<bool>(PELICAN_WITH_AUDIO),
        "sound effects a scene callback stages (playSound, stopSound, "
        "setBusVolume): the effect transaction refuses them as "
        "build_feature_disabled@1",
        stub},
    SceneFeatureDivision{"vat", "PELICAN_WITH_VAT",
        static_cast<bool>(PELICAN_WITH_VAT),
        "a model whose GLB carries pelican.vat primitives: refused while the "
        "model loads; the VAT player's stub refuses --play-vat",
        stub},
    SceneFeatureDivision{"seqplayer", "PELICAN_WITH_SEQPLAYER",
        static_cast<bool>(PELICAN_WITH_SEQPLAYER),
        "the transform sequence --play-seq places in the scene: the sequence "
        "player's stub refuses it",
        stub},
    SceneFeatureDivision{"xr", "PELICAN_WITH_OPENXR",
        static_cast<bool>(PELICAN_WITH_OPENXR), {}, absent},
    SceneFeatureDivision{"rpc", "PELICAN_WITH_RPC",
        static_cast<bool>(PELICAN_WITH_RPC), {}, absent},
    SceneFeatureDivision{"imgui", "PELICAN_WITH_IMGUI",
        static_cast<bool>(PELICAN_WITH_IMGUI), {}, absent},
};

consteval SceneParticipantPresence presenceOf(std::string_view feature) {
    for (const auto &division : feature_divisions) {
        if (division.feature == feature) return division.presence();
    }
    throw "a participant row names a module without a division row";
}

constexpr auto physics_presence = presenceOf("physics");
constexpr auto seq_presence = presenceOf("seqplayer");
constexpr auto vat_presence = presenceOf("vat");
constexpr auto xr_presence = presenceOf("xr");
constexpr auto audio_presence = presenceOf("audio");
constexpr auto rpc_presence = presenceOf("rpc");

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

    SceneParticipantManifestEntry{"procedural", "aabb_buffer", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "aabb-positions-khr-v1", scene_load_mutation::procedural_aabb_buffer},
    SceneParticipantManifestEntry{"procedural", "allocation_generation", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "u64", scene_load_mutation::procedural_allocation_generation},
    SceneParticipantManifestEntry{"procedural", "instance_snapshot", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "authoring-id+component-occurrence-v1", scene_load_mutation::procedural_instance_snapshot},
    SceneParticipantManifestEntry{"procedural", "retire_queue", real, participant, "prepare", swap_publish, swap_rollback, "next-frame-retirement-queue", "procedural-state-queue-v1", scene_load_mutation::procedural_retire_queue},

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
    SceneParticipantManifestEntry{"renderer.flat", "ray_scene_declarations", real, participant, "prepare", swap_publish, swap_rollback, "shared-snapshot-release", "ray-declaration-catalog-v1", {}},
    SceneParticipantManifestEntry{"ray", "occurrence_catalog", real, participant, "prepare", swap_publish, swap_rollback, "shared-snapshot-release", "ray-inventory-v1", scene_load_mutation::ray_occurrence_catalog},
    SceneParticipantManifestEntry{"ray", "as_plan", real, participant, "prepare", swap_publish, swap_rollback, "shared-snapshot-release", "resolved-ray-rows-v1", scene_load_mutation::ray_as_plan},
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
    // silently promote caches or ID-A registry work into this transaction.
    SceneParticipantManifestEntry{"model_asset", "warm_cache", real, SceneManifestRole::cache, "lookup", "retained-on-failure", "none", no_retire, "not-compared", {}},
    SceneParticipantManifestEntry{"registry_slot", "candidate_registry", SceneParticipantPresence::absent, participant, "ID-A", "none", "none", no_retire, "absent", {}},
    SceneParticipantManifestEntry{"audio", "play", audio_presence, SceneManifestRole::effect, "prepare+activate:decode+reserve", "flush:backend-play", "discard-outbox", no_retire, "handle+decoded-sound-v1", audio_presence == real ? scene_load_mutation::effect_audio_play : std::string_view{}},
    SceneParticipantManifestEntry{"audio", "stop_existing", audio_presence, SceneManifestRole::effect, "activate:shadow", "flush:erase-then-backend-destroy", "discard-outbox", no_retire, "handle-v1", audio_presence == real ? scene_load_mutation::effect_audio_stop : std::string_view{}},
    SceneParticipantManifestEntry{"audio", "bus_volume", audio_presence, SceneManifestRole::effect, "prepare+activate:shadow", "flush:state-then-backend", "discard-outbox", no_retire, "bus-volume-v1", audio_presence == real ? scene_load_mutation::effect_audio_bus_volume : std::string_view{}},
    SceneParticipantManifestEntry{"audio", "handle_cursor", audio_presence, SceneManifestRole::non_rollback, "activate:permanent-consume", "none", "non-rollback", no_retire, "u64-gap-named-v1", {}},
    SceneParticipantManifestEntry{"persistence", "game_settings", real, SceneManifestRole::effect, "prepare+activate:shadow", "flush:noexcept-move", "discard-shadow", no_retire, "json-v1", {}},
    SceneParticipantManifestEntry{"persistence", "save_data", real, SceneManifestRole::effect, "activate:encode+coalesce", "flush:atomic-write", "discard-outbox", no_retire, "path+bytes-v1", scene_load_mutation::effect_save_data},
    SceneParticipantManifestEntry{"persistence", "save_settings", real, SceneManifestRole::effect, "activate:encode-shadow+coalesce", "flush:atomic-write", "discard-outbox", no_retire, "path+bytes-v1", scene_load_mutation::effect_save_settings},
    SceneParticipantManifestEntry{"persistence", "load_data_overlay", real, SceneManifestRole::read_only, "activate:outbox-first", "none", "none", no_retire, "json-v1", {}},
    SceneParticipantManifestEntry{"persistence", "list_saves_overlay", real, SceneManifestRole::read_only, "activate:disk-union-outbox", "none", "none", no_retire, "slot+timestamp-zero-v1", {}},
    SceneParticipantManifestEntry{"gameobjects", "direct_mutation", real, SceneManifestRole::reject, "transaction-scope", "reject", "unchanged", no_retire, "SceneTransactionMutationRejected-v1", {}},
    SceneParticipantManifestEntry{"behavior_context", "retired_self_setters", real, SceneManifestRole::reject, "retire:inverse-view", "effect-error", "unchanged", no_retire, "SceneRetiredObjectMutationRejected-v1", {}},
    SceneParticipantManifestEntry{"debug_text", "frame_text", real, SceneManifestRole::frame_lifetime, "frame", "direct", "frame-expiry", no_retire, "frame-lifetime", {}},

    SceneParticipantManifestEntry{"root.startup", "direct_load", real, SceneManifestRole::production_root, "Borrowed", "SceneLoader::load(request)", "result", no_retire, "root-wiring-v1", scene_load_mutation::root_startup},
    SceneParticipantManifestEntry{"root.rpc", "direct_load", rpc_presence, SceneManifestRole::production_root, "Borrowed", "SceneLoader::load(request)", "result", no_retire, "root-wiring-v1", rpc_presence == real ? scene_load_mutation::root_rpc : std::string_view{}},
    SceneParticipantManifestEntry{"root.pending", "apply", real, SceneManifestRole::production_root, "Borrowed:late-bound", "SceneLoader::load(request)", "retain-on-failure", no_retire, "root-wiring-v1", scene_load_mutation::root_pending},
    SceneParticipantManifestEntry{"root.import.versioned", "callback", real, SceneManifestRole::production_root, "Owned", "SceneLoader::load(request)", "discard-token", no_retire, "root-wiring-v1", scene_load_mutation::root_import_versioned},
};

// Exact public signatures are intentionally stored in the runtime manifest.
// tools/public_api_inventory.py parses this array and rejects either side's
// extras, so the checked-in JSON cannot drift into a second classification
// authority.
constexpr std::array public_api_manifest{
    ScenePublicApiManifestEntry{"GameContext", "GameObjectId createObject(const LocalTransformComponent&transform)const", "covered.deferred-object"},
    ScenePublicApiManifestEntry{"GameContext", "GameObjectId createSpriteObject(const LocalTransformComponent&transform,const SpriteViewComponent&sprite)const", "covered.deferred-object"},
    ScenePublicApiManifestEntry{"GameContext", "[[nodiscard]] bool removeObject(GameObjectId id)const", "covered.deferred-object"},
    ScenePublicApiManifestEntry{"GameContext", "[[nodiscard]] bool setLocalTransform(GameObjectId id,const LocalTransformComponent&transform)const", "covered.candidate-or-retired-reject"},
    ScenePublicApiManifestEntry{"GameContext", "[[nodiscard]] bool setSpriteView(GameObjectId id,const SpriteViewComponent&sprite)const", "covered.candidate-or-retired-reject"},
    ScenePublicApiManifestEntry{"GameContext", "[[nodiscard]] bool setSpriteTexture(GameObjectId id,std::string_view texture)const", "covered.candidate-or-retired-reject"},
    // GP-2: queued under the scene run the call addresses (the load candidate
    // inside a load's callbacks) and carried out at the next frame boundary;
    // a request of a run that is not live there, a failed candidate's
    // included, is dropped.
    ScenePublicApiManifestEntry{"GameContext", "TransientObjectRef spawn(std::string_view prefab,const SpawnParams&params)const", "covered.spawn-queue"},
    ScenePublicApiManifestEntry{"GameContext", "[[nodiscard]] Resolved<TransientObjectRef>destroy(const TransientObjectRef&ref)const", "covered.spawn-queue"},
    ScenePublicApiManifestEntry{"RenderContext", "[[nodiscard]] bool setDirectionalLightDirection(std::string_view name,vec3 direction)const", "covered.candidate"},
    ScenePublicApiManifestEntry{"RenderContext", "[[nodiscard]] bool setDirectionalLightIntensity(std::string_view name,float intensity)const", "covered.candidate"},
    ScenePublicApiManifestEntry{"RenderContext", "[[nodiscard]] bool setPointLightPosition(std::string_view name,vec3 position)const", "covered.candidate"},
    ScenePublicApiManifestEntry{"RenderContext", "[[nodiscard]] bool setSpotLightDirection(std::string_view name,vec3 direction)const", "covered.candidate"},
    ScenePublicApiManifestEntry{"RenderContext", "void setCamera(std::string_view name)const", "covered.candidate"},
    ScenePublicApiManifestEntry{"RenderContext", "void setRayTracingDispatchExtent(std::string_view task_name,std::uint32_t width,std::uint32_t height,std::uint32_t depth)const", "reject.transaction-scope"},
    ScenePublicApiManifestEntry{"RenderContext", "void setRayTracingCallableRecord(std::string_view task_name,std::uint32_t index,const nlohmann::json&values)const", "reject.transaction-scope"},
    // GP-3: the UI document is not a scene participant. A write queued while a
    // scene loads lands at the next frame boundary whatever the load's outcome.
    ScenePublicApiManifestEntry{"UiContext", "[[nodiscard]] UiWriteResult setText(std::string_view path,std::string_view text)const", "nonparticipant.ui-document"},
    ScenePublicApiManifestEntry{"UiContext", "[[nodiscard]] UiWriteResult setVisible(std::string_view path,bool visible)const", "nonparticipant.ui-document"},
    ScenePublicApiManifestEntry{"UiContext", "[[nodiscard]] UiWriteResult setValue(std::string_view path,float value)const", "nonparticipant.ui-document"},
    // GP-4: the cursor mode belongs to the window, not to a scene. A request
    // made while a scene loads is applied at the next frame whatever the
    // load's outcome.
    ScenePublicApiManifestEntry{"InputContext", "void setCursorMode(CursorMode mode)const", "nonparticipant.window-cursor"},
    ScenePublicApiManifestEntry{"GameContext", "double random()", "covered.rng-sequence"},
    ScenePublicApiManifestEntry{"GameContext", "int randomInt(int min,int max)", "covered.rng-sequence"},
    ScenePublicApiManifestEntry{"GameContext", "float randomFloat(float min,float max)", "covered.rng-sequence"},
    ScenePublicApiManifestEntry{"GameContext", "void setSeed(std::uint64_t seed)", "covered.rng-sequence"},
    ScenePublicApiManifestEntry{"AudioContext", "SoundHandle playSound(std::string_view path)const", "shadow_outbox.audio-play"},
    ScenePublicApiManifestEntry{"AudioContext", "SoundHandle playSound(std::string_view path,std::string_view bus)const", "shadow_outbox.audio-play"},
    ScenePublicApiManifestEntry{"AudioContext", "void stopSound(SoundHandle handle)const", "shadow_outbox.audio-stop"},
    ScenePublicApiManifestEntry{"AudioContext", "void setBusVolume(std::string_view bus,float volume)const", "shadow_outbox.audio-bus-volume"},
    ScenePublicApiManifestEntry{"AudioContext", "bool setSoundVolume(SoundHandle handle,float volume)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"AudioContext", "bool setSoundPitch(SoundHandle handle,float pitch)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"AudioContext", "bool setSoundPaused(SoundHandle handle,bool paused)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"RenderContext", "bool setRenderValues(std::string_view buffer,std::span<const float>values)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"RenderContext", "bool setValueGraphParameter(GameObjectId object,std::string_view parameter,std::optional<double>value)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"RenderContext", "bool emitValueGraphEvent(GameObjectId object,std::string_view input,Animation::CurvePlaybackCommand command)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"AnimationContext", "Animation::Status setParameter(GameObjectId object,std::string_view name,double value)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"AnimationContext", "Animation::Status forceState(GameObjectId object,std::string_view state)const", "refuse.transaction-scope"},
    ScenePublicApiManifestEntry{"GameContext", "void loadScene(std::string_view name)const", "covered.pending-scene"},
    ScenePublicApiManifestEntry{"RenderContext", "void debugText(int x,int y,std::string_view text,vec4 color = {1,1,1,1},int scale = 1)const", "nonparticipant.frame-lifetime"},
    ScenePublicApiManifestEntry{"GameContext", "template<class Event>void emit(const Event&event)const", "covered.event-outbox"},
    ScenePublicApiManifestEntry{"GameContext", "void setGameSettings(Json settings)const", "shadow_outbox.game-settings"},
    ScenePublicApiManifestEntry{"GameContext", "void saveSettings()const", "shadow_outbox.save-settings"},
    ScenePublicApiManifestEntry{"SaveContext", "void saveData(std::string_view slot,const Json&data)const", "shadow_outbox.save-data"},
    ScenePublicApiManifestEntry{"GameObjects", "static auto add()", "reject.direct-gameobjects"},
    ScenePublicApiManifestEntry{"GameObjects", "static GameObjectId createWithComponents(std::span<const ComponentId>ids,const std::function<void(std::span<void*>)>&populate)", "reject.direct-gameobjects"},
    ScenePublicApiManifestEntry{"GameObjects", "[[nodiscard]] static bool remove(GameObjectId id)", "reject.direct-gameobjects"},
    ScenePublicApiManifestEntry{"GameObjects", "static void removeAll()", "reject.direct-gameobjects"},
    ScenePublicApiManifestEntry{"GameObjects", "[[nodiscard]] static bool setLocalTransform(GameObjectId id,const LocalTransformComponent&transform)", "reject.direct-gameobjects"},
    ScenePublicApiManifestEntry{"BehaviorContext", "GameObjectId createObject(const LocalTransformComponent&transform)const", "covered.deferred-object"},
    ScenePublicApiManifestEntry{"BehaviorContext", "GameObjectId createSpriteObject(const LocalTransformComponent&transform,const SpriteViewComponent&sprite)const", "covered.deferred-object"},
    ScenePublicApiManifestEntry{"BehaviorContext", "[[nodiscard]] bool removeObject(GameObjectId id)const", "covered.deferred-object"},
};

} // namespace

std::span<const SceneParticipantManifestEntry>
sceneParticipantManifest() noexcept {
    return manifest;
}

std::span<const ScenePublicApiManifestEntry>
scenePublicApiManifest() noexcept {
    return public_api_manifest;
}

std::span<const SceneFeatureDivision> sceneFeatureDivisions() noexcept {
    return feature_divisions;
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
    case SceneManifestRole::effect: return "effect";
    case SceneManifestRole::non_rollback: return "non-rollback";
    case SceneManifestRole::reject: return "reject";
    case SceneManifestRole::frame_lifetime: return "frame-lifetime";
    case SceneManifestRole::read_only: return "read-only";
    }
    return "unknown";
}

} // namespace Pelican
