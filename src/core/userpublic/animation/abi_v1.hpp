#pragma once

#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::Animation {

inline constexpr std::uint32_t abiVersionV1 = 1;
inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::uint32_t poseAlignmentV1 = 64;
inline constexpr std::uint32_t maxIntervalTraversalLoopsV1 = 65536;
inline constexpr std::uint32_t maxIntervalCrossingsV1 = 65536;

// Every descriptor starts with these four fields. Reserved fields must be zero.
struct DescriptorHeaderV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};

#define PELICAN_ANIM_HANDLE(name)                                                                                      \
    struct name {                                                                                                      \
        std::uint64_t identity;                                                                                        \
        std::uint32_t generation;                                                                                      \
        std::uint32_t reserved;                                                                                        \
    }

PELICAN_ANIM_HANDLE(RigHandle);
PELICAN_ANIM_HANDLE(PoseLayoutHandle);
PELICAN_ANIM_HANDLE(SkinBindingHandle);
PELICAN_ANIM_HANDLE(ClipHandle);
PELICAN_ANIM_HANDLE(CursorHandle);
PELICAN_ANIM_HANDLE(PoseHandle);
PELICAN_ANIM_HANDLE(PoseArenaHandle);
PELICAN_ANIM_HANDLE(InstanceHandle);
PELICAN_ANIM_HANDLE(AnimationSinkHandle);
PELICAN_ANIM_HANDLE(AnimationSourceHandle);
PELICAN_ANIM_HANDLE(AnimationOwnerHandle);
PELICAN_ANIM_HANDLE(PhaseRegistrationHandle);

#undef PELICAN_ANIM_HANDLE

template <class T> constexpr T invalidHandle() noexcept { return T{0, 0, 0}; }
template <class T> constexpr bool isValid(T handle) noexcept {
    return handle.identity != 0 && handle.generation != 0 && handle.reserved == 0;
}

enum class Status : std::uint32_t {
    ok = 0,
    buffer_too_small = 1,
    invalid_argument = 2,
    unsupported_version = 3,
    reserved_not_zero = 4,
    invalid_handle = 5,
    stale_generation = 6,
    incompatible_layout = 7,
    wrong_thread = 8,
    out_of_memory = 9,
    duplicate_revision = 10,
    phase_order_error = 11,
    not_found = 12,
    authority_conflict = 13,
    callback_failed = 14,
};

enum class WrapMode : std::uint32_t { clamp = 0, repeat = 1 };
enum class ChannelKind : std::uint32_t {
    translation = 0,
    rotation = 1,
    scale = 2,
    morph_weight = 3,
    scalar_curve = 4,
    vector_curve = 5,
    attribute = 6,
};
enum class AnnotationKind : std::uint32_t { event = 0, marker = 1 };
enum class ValueKind : std::uint32_t { boolean = 0, signed_integer = 1, scalar = 2, vector4 = 3 };
enum class BlendMode : std::uint32_t { normal = 0, additive = 1 };
enum class AdditiveSpace : std::uint32_t { local = 0, model = 1, mesh = 2 };
enum class SidebandPolicy : std::uint32_t { suppress = 0, dominant_weight = 1, weighted = 2, all = 3 };
enum class RestFallback : std::uint32_t { use_rest_pose = 0, use_first_input = 1, error = 2 };
enum class Phase : std::uint32_t {
    parameter_snapshot = 0,
    base_pose_and_root_modifier = 1,
    movement_root_resolution = 2,
    world_post_process = 3,
    commit = 4,
};
enum class AnimationSinkKind : std::uint32_t {
    object_transform = 0,
    skeletal_pose = 1,
    expression_curve = 2,
};
enum class AnimationClipKindV1 : std::uint32_t {
    skeletal_clip = 0,
    vrma_retargeted_clip = 1,
};
enum class AnimationSourceAuthorityV1 : std::uint32_t {
    graph_apply = 0,
    timeline_extract_only = 1,
};
enum AnimationTypedChannelFlagsV1 : std::uint32_t {
    animation_typed_channel_none = 0,
    animation_typed_channel_expression = 1u << 0u,
    animation_typed_channel_gaze = 1u << 1u,
};
enum class AnimationNotificationKind : std::uint32_t {
    set_time = 0,
    replay_seek = 1,
    graph_reload = 2,
    model_reload = 3,
    layout_generation_mismatch = 4,
};

enum AdvanceFlags : std::uint32_t {
    advance_none = 0,
    advance_absolute_seek = 1u << 0,
    advance_discontinuity = 1u << 1,
};
enum IntervalResultFlags : std::uint32_t {
    interval_none = 0,
    interval_looped = 1u << 0,
    interval_reverse = 1u << 1,
    interval_seeked = 1u << 2,
    interval_discontinuous = 1u << 3,
};
enum CommitFlags : std::uint32_t {
    commit_none = 0,
    commit_reset_history = 1u << 0,
    commit_discontinuity = 1u << 1,
};

struct Vec4fV1 { float x, y, z, w; };
struct QuatfV1 { float x, y, z, w; };
struct TransformV1 { Vec4fV1 translation; QuatfV1 rotation; Vec4fV1 scale; };
struct Matrix4fV1 { float column_major[16]; };
struct RootDeltaV1 { Vec4fV1 translation; QuatfV1 rotation; };

struct PoseViewV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    PoseHandle pose;
    PoseLayoutHandle layout;
    std::uint32_t joint_count;
    std::uint32_t element_stride;
    Vec4fV1 *translations;
    QuatfV1 *rotations;
    Vec4fV1 *scales;
};

struct ClipMetadataV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    ClipHandle clip;
    RigHandle source_rig;
    double start_seconds;
    double end_seconds;
    WrapMode wrap_mode;
    std::uint32_t channel_kind_mask;
    std::uint64_t annotation_identity;
    std::uint32_t annotation_generation;
    std::uint32_t sampling_context_generation;
    std::uint32_t cursor_generation;
    std::uint32_t reserved2;
    // Additive VRMA-I0 tail. Older callers may stop at reserved2.
    AnimationClipKindV1 clip_kind;
    std::uint32_t typed_channel_flags;
    std::uint32_t expression_channel_count;
    std::uint32_t profile_version;
    std::uint64_t asset_identity;
    std::uint32_t asset_generation;
    std::uint32_t reserved3;
    std::uint8_t source_rig_sha256[32];
    std::uint8_t target_rig_sha256[32];
};

struct CrossingV1 {
    std::uint32_t element_size;
    std::uint32_t version;
    AnnotationKind kind;
    std::uint32_t source;
    std::uint32_t ordinal;
    std::uint32_t reserved0;
    double clip_time_seconds;
    std::int64_t loop_index;
    std::uint64_t annotation_identity;
};

struct ValueV1 {
    std::uint32_t element_size;
    std::uint32_t version;
    ValueKind kind;
    std::uint32_t source;
    std::uint64_t key_identity;
    Vec4fV1 value;
};

struct AdvanceDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    CursorHandle cursor;
    double delta_seconds;
    double absolute_seconds;
    std::uint32_t flags;
    std::uint32_t reserved2;
};

struct IntervalResultV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint32_t result_flags;
    float normalized_phase;
    RootDeltaV1 root_delta;
    CrossingV1 *crossings;
    std::uint32_t crossing_capacity;
    std::uint32_t crossing_count;
    ValueV1 *values;
    std::uint32_t value_capacity;
    std::uint32_t value_count;
};

struct BlendLayerV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    PoseHandle pose;
    float weight;
    const float *joint_weights;
    std::uint32_t joint_weight_count;
    BlendMode mode;
    AdditiveSpace additive_space;
    PoseHandle additive_reference_pose;
    RestFallback rest_fallback;
    SidebandPolicy root_policy;
    SidebandPolicy curve_policy;
    SidebandPolicy event_marker_policy;
};

struct PhaseRegistrationV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    Phase phase;
    std::int32_t priority;
    std::uint64_t registration_identity;
    std::uint32_t registration_generation;
    std::uint32_t source_ordinal;
};

struct PublishAnimationFrameDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    InstanceHandle instance;
    PoseHandle local_pose;
    PoseHandle model_pose;
    const Matrix4fV1 *palette;
    std::uint32_t palette_count;
    std::uint32_t flags;
    std::uint64_t frame_revision;
    RootDeltaV1 root_delta;
};

struct AdvanceTemporalHistoryDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint64_t rendered_frame_revision;
    std::uint32_t velocity_consumed;
    std::uint32_t reserved2;
};

// A1.1 public service descriptors. String pointers and callback pointers are
// borrowed for the duration of the call only; the engine never retains them.
struct CurrentAnimationOwnerDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
};

struct ResolveAnimationSinkDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    const char *object_name;
    std::uint32_t object_name_size;
    AnimationSinkKind sink_kind;
    AnimationSinkHandle sink;
};

struct ResolveAnimationInstanceDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationSinkHandle sink;
    InstanceHandle instance;
};

struct ResolveAnimationRigDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    InstanceHandle instance;
    RigHandle rig;
};

struct ResolvePoseLayoutDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    RigHandle rig;
    PoseLayoutHandle layout;
    SkinBindingHandle skin_binding;
    std::uint32_t joint_count;
    std::uint32_t palette_count;
};

struct ResolveAnimationClipDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    RigHandle rig;
    const char *clip_name;
    std::uint32_t clip_name_size;
    std::uint32_t reserved2;
    ClipHandle clip;
};

struct BeginPoseArenaFrameDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    std::uint64_t frame_revision;
    PoseArenaHandle arena;
};

struct AcquirePoseDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    PoseArenaHandle arena;
    PoseLayoutHandle layout;
    std::uint32_t joint_count;
    std::uint32_t reserved2;
    PoseViewV1 *out_view;
};

struct CreateClipCursorDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    ClipHandle clip;
    CursorHandle cursor;
};

struct DestroyClipCursorDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    CursorHandle cursor;
};

struct SamplePoseAtDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    ClipHandle clip;
    double time_seconds;
    PoseHandle output_pose;
};

// Names point into the immutable source asset and are borrowed only for the
// duration of sample_animation_source_at. Callers that retain them must copy.
struct AnimationExpressionSampleV1 {
    std::uint32_t element_size;
    std::uint32_t version;
    const char *name;
    std::uint32_t name_size;
    float weight;
    std::uint32_t preset;
    std::uint32_t reserved0;
};

struct AnimationGazeSampleV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint32_t present;
    std::uint32_t offset_present;
    Vec4fV1 offset_from_head_bone;
    QuatfV1 rotation;
};

struct SampleAnimationSourceAtDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    ClipHandle clip;
    double time_seconds;
    PoseHandle output_pose;
    AnimationExpressionSampleV1 *expressions;
    std::uint32_t expression_capacity;
    std::uint32_t expression_count;
    AnimationGazeSampleV1 gaze;
};

struct BlendNormalDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    RigHandle rig;
    const BlendLayerV1 *layers;
    std::uint32_t layer_count;
    std::uint32_t reserved2;
    PoseHandle output_pose;
};

struct LocalToModelDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    RigHandle rig;
    PoseHandle local_pose;
    PoseHandle model_pose;
    Matrix4fV1 *model_matrices;
    std::uint32_t model_matrix_capacity;
    std::uint32_t model_matrix_count;
};

struct BuildSkinPaletteDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    SkinBindingHandle skin_binding;
    const Matrix4fV1 *model_matrices;
    std::uint32_t model_matrix_count;
    std::uint32_t reserved2;
    Matrix4fV1 *palette;
    std::uint32_t palette_capacity;
    std::uint32_t palette_count;
};

struct AnimationPhaseContextV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    Phase phase;
    std::uint32_t reserved2;
    AnimationSinkHandle sink;
    InstanceHandle instance;
    std::uint64_t frame_revision;
};

using AnimationPhaseCallbackV1 = Status (*)(void *user_context, const AnimationPhaseContextV1 *context);

struct RegisterAnimationPhaseDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    PhaseRegistrationV1 registration;
    AnimationPhaseCallbackV1 callback;
    void *user_context;
    PhaseRegistrationHandle registration_handle;
};

struct UnregisterAnimationPhaseDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    PhaseRegistrationHandle registration;
};

struct ClaimAnimationSourceDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    AnimationSinkHandle sink;
    std::uint32_t source_ordinal;
    std::uint32_t reserved2;
    AnimationSourceHandle source;
};

struct ClaimAnimationSourcePolicyDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    AnimationSinkHandle sink;
    std::uint32_t source_ordinal;
    AnimationSourceAuthorityV1 authority;
    AnimationSourceHandle source;
};

struct ReleaseAnimationSourceDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationOwnerHandle owner;
    AnimationSourceHandle source;
};

struct HandoffAnimationSourceDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationSourceHandle current_source;
    AnimationOwnerHandle next_owner;
    std::uint32_t next_source_ordinal;
    std::uint32_t reserved2;
    AnimationSourceHandle next_source;
};

struct AnimationNotificationDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationNotificationKind kind;
    std::uint32_t reserved2;
    AnimationSinkHandle sink;
    AnimationSourceHandle source;
    PoseLayoutHandle observed_layout;
    double time_seconds;
    std::uint64_t notification_revision;
};

struct PublishAnimationFrameFromSourceDescV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    AnimationSourceHandle source;
    PublishAnimationFrameDescV1 frame;
};

inline constexpr std::uint32_t animationServiceVersionV1 = 1;
enum AnimationServiceCapabilityBitsV1 : std::uint64_t {
    animation_service_asset_resolution = 1ull << 0,
    animation_service_pose_jobs = 1ull << 1,
    animation_service_phase_registration = 1ull << 2,
    animation_service_source_authority = 1ull << 3,
    animation_service_notifications = 1ull << 4,
    animation_service_typed_source_sampling = 1ull << 5,
    animation_service_extract_only_authority = 1ull << 6,
};
inline constexpr std::uint64_t animationServiceCapabilitiesV1 =
    animation_service_asset_resolution | animation_service_pose_jobs |
    animation_service_phase_registration | animation_service_source_authority |
    animation_service_notifications | animation_service_typed_source_sampling |
    animation_service_extract_only_authority;

struct AnimationServiceV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint32_t service_version;
    std::uint32_t minimum_client_service_version;
    std::uint64_t capability_bits;
    void *context;
    Status (*get_current_owner)(void *, CurrentAnimationOwnerDescV1 *);
    Status (*resolve_sink)(void *, ResolveAnimationSinkDescV1 *);
    Status (*resolve_instance)(void *, ResolveAnimationInstanceDescV1 *);
    Status (*resolve_rig)(void *, ResolveAnimationRigDescV1 *);
    Status (*resolve_layout)(void *, ResolvePoseLayoutDescV1 *);
    Status (*resolve_clip)(void *, ResolveAnimationClipDescV1 *);
    Status (*begin_pose_frame)(void *, BeginPoseArenaFrameDescV1 *);
    Status (*acquire_pose)(void *, AcquirePoseDescV1 *);
    Status (*get_clip_metadata)(void *, ClipMetadataV1 *);
    Status (*create_cursor)(void *, CreateClipCursorDescV1 *);
    Status (*destroy_cursor)(void *, const DestroyClipCursorDescV1 *);
    Status (*sample_pose_at)(void *, const SamplePoseAtDescV1 *);
    Status (*blend_normal)(void *, const BlendNormalDescV1 *);
    Status (*local_to_model)(void *, LocalToModelDescV1 *);
    Status (*build_skin_palette)(void *, BuildSkinPaletteDescV1 *);
    Status (*register_phase)(void *, RegisterAnimationPhaseDescV1 *);
    Status (*unregister_phase)(void *, const UnregisterAnimationPhaseDescV1 *);
    Status (*claim_source)(void *, ClaimAnimationSourceDescV1 *);
    Status (*release_source)(void *, const ReleaseAnimationSourceDescV1 *);
    Status (*handoff_source)(void *, HandoffAnimationSourceDescV1 *);
    Status (*notify)(void *, const AnimationNotificationDescV1 *);
    Status (*publish_animation_frame_from_source)(void *, const PublishAnimationFrameFromSourceDescV1 *);
    // Additive VRMA-I0 tail.
    Status (*sample_animation_source_at)(void *, SampleAnimationSourceAtDescV1 *);
    Status (*claim_source_policy)(void *, ClaimAnimationSourcePolicyDescV1 *);
};

using GetAnimationServiceV1Fn = Status (*)(void *, std::uint32_t, AnimationServiceV1 *);

struct ApiV1 {
    std::uint32_t struct_size;
    std::uint32_t version;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint32_t engine_abi_version;
    std::uint32_t minimum_client_abi_version;
    std::uint64_t capability_bits;
    void *context;
    Status (*advance_cursor)(void *, const AdvanceDescV1 *, IntervalResultV1 *);
    Status (*publish_animation_frame)(void *, const PublishAnimationFrameDescV1 *);
    Status (*advance_temporal_history_after_render)(void *, const AdvanceTemporalHistoryDescV1 *);
    GetAnimationServiceV1Fn get_animation_service;
};

// Additive negotiation entry point. out_api->struct_size is supplied by the caller;
// the engine writes at most min(caller size, sizeof(ApiV1)).
PELICAN_API Status getApiV1(std::uint32_t client_abi_version, ApiV1 *out_api) noexcept;

static_assert(sizeof(RigHandle) == 16);
static_assert(sizeof(PoseHandle) == 16);
static_assert(std::is_standard_layout_v<DescriptorHeaderV1>);
static_assert(offsetof(AdvanceDescV1, struct_size) == 0);
static_assert(offsetof(AdvanceDescV1, version) == 4);
static_assert(offsetof(IntervalResultV1, struct_size) == 0);
static_assert(offsetof(IntervalResultV1, version) == 4);

} // namespace Pelican::Animation
