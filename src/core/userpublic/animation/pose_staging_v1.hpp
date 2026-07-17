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
};

inline constexpr std::uint64_t poseStagingServiceCapabilitiesV1 =
    pose_staging_deferred_commit | pose_staging_mutable_local_pose |
    pose_staging_source_node_mapping;

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
};

// Additive negotiation surface. animation/abi_v1.hpp remains frozen.
PELICAN_API Status
getPoseStagingServiceV1(std::uint32_t client_service_version,
                        PoseStagingServiceV1 *out_service) noexcept;

static_assert(sizeof(PoseStageHandle) == 16);
static_assert(std::is_standard_layout_v<AcquireStagedPoseDescV1>);
static_assert(offsetof(AcquireStagedPoseDescV1, struct_size) == 0);
static_assert(offsetof(PoseStagingServiceV1, struct_size) == 0);

} // namespace Pelican::Animation
