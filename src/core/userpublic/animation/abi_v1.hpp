#pragma once

#include "../export.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::Animation {

inline constexpr std::uint32_t abiVersionV1 = 1;
inline constexpr std::uint32_t descriptorVersionV1 = 1;
inline constexpr std::uint32_t poseAlignmentV1 = 64;

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
