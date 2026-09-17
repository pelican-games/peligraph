#pragma once

#include "abi_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::Animation {

inline constexpr std::uint32_t poseStagingDescriptorVersionV1 = 1;
inline constexpr std::uint32_t poseStagingServiceVersionV1 = 1;

struct PoseStageHandle {
    std::uint64_t identity;
    std::uint32_t generation;
    std::uint32_t reserved;
};

inline constexpr PoseStageHandle invalidPoseStage() noexcept {
    return {0, 0, 0};
}

inline constexpr bool isValid(PoseStageHandle handle) noexcept {
    return handle.identity != 0 && handle.generation != 0 &&
           handle.reserved == 0;
}

enum PoseStagingServiceCapabilityBitsV1 : std::uint64_t {
    pose_staging_deferred_commit = 1ull << 0,
    pose_staging_mutable_local_pose = 1ull << 1,
    pose_staging_source_node_mapping = 1ull << 2,
    // AG-2. resolve_rig_source_node is present: the same glTF-node to
    // layout-index mapping, asked of a rig instead of a staged frame, plus
    // which channel kinds one clip drives on that node. The service version
    // stays 1 -- raising it would fail the negotiation of every client that
    // already asked for 1 -- so a caller learns about the entry from this bit.
    pose_staging_rig_source_node_mapping = 1ull << 3,
};

inline constexpr std::uint64_t poseStagingServiceCapabilitiesV1 =
    pose_staging_deferred_commit | pose_staging_mutable_local_pose |
    pose_staging_source_node_mapping | pose_staging_rig_source_node_mapping;

// Looks up the pose published by the active source for the current phase run.
// The returned PoseView storage remains owned by the frame arena and is valid
// only until that phase run completes. All callbacks observe the same storage.
struct AcquireStagedPoseDescV1 {
    std::uint32_t struct_size = sizeof(AcquireStagedPoseDescV1);
    std::uint32_t version = poseStagingDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    InstanceHandle instance{};
    std::uint64_t frame_revision = 0;
    PoseStageHandle stage{};
    PoseViewV1 local_pose{};
    PoseViewV1 model_pose{};
};

// Converts a source glTF node index into the active pose layout index. This
// keeps application evaluators independent of engine-private rig ordering.
struct ResolveStagedSourceNodeDescV1 {
    std::uint32_t struct_size = sizeof(ResolveStagedSourceNodeDescV1);
    std::uint32_t version = poseStagingDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    PoseStageHandle stage{};
    std::uint32_t source_node_index = 0;
    std::uint32_t layout_node_index = 0;
};

// The same conversion asked of a rig rather than a staged frame, for a caller
// that needs the layout index before anything has been staged: a pose.clip_sample
// node resolves its root_node once, when it attaches, and a staged frame only
// exists inside a publish. When `clip` is valid, `channel_kind_mask` reports the
// ChannelKind bits that clip drives on that node, which is how "this clip has no
// translation on the node you called the root" is answered without sampling.
struct ResolveRigSourceNodeDescV1 {
    std::uint32_t struct_size = sizeof(ResolveRigSourceNodeDescV1);
    std::uint32_t version = poseStagingDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    RigHandle rig{};
    ClipHandle clip{};
    std::uint32_t source_node_index = 0;
    std::uint32_t layout_node_index = 0;
    std::uint32_t channel_kind_mask = 0;
    std::uint32_t reserved2 = 0;
};

struct PoseStagingServiceV1 {
    std::uint32_t struct_size = sizeof(PoseStagingServiceV1);
    std::uint32_t version = poseStagingDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t service_version = 0;
    std::uint32_t minimum_client_service_version = 0;
    std::uint64_t capability_bits = 0;
    void *context = nullptr;
    Status (*acquire_staged_pose)(void *, AcquireStagedPoseDescV1 *) = nullptr;
    Status (*resolve_source_node)(void *, ResolveStagedSourceNodeDescV1 *) =
        nullptr;
    // Additive tail; present when pose_staging_rig_source_node_mapping is set.
    Status (*resolve_rig_source_node)(void *, ResolveRigSourceNodeDescV1 *) =
        nullptr;
};

// Additive negotiation surface. animation/abi_v1.hpp remains frozen.
PELICAN_API Status
getPoseStagingServiceV1(std::uint32_t client_service_version,
                        PoseStagingServiceV1 *out_service) noexcept;

static_assert(sizeof(PoseStageHandle) == 16);
static_assert(std::is_standard_layout_v<AcquireStagedPoseDescV1>);
static_assert(std::is_standard_layout_v<ResolveRigSourceNodeDescV1>);
static_assert(offsetof(ResolveRigSourceNodeDescV1, struct_size) == 0);
static_assert(offsetof(AcquireStagedPoseDescV1, struct_size) == 0);
static_assert(offsetof(PoseStagingServiceV1, struct_size) == 0);

} // namespace Pelican::Animation
