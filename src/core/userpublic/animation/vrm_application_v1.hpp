#pragma once

#include "abi_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Pelican::Vrm {

inline constexpr std::uint32_t applicationDescriptorVersionV1 = 1;
inline constexpr std::uint32_t applicationServiceVersionV1 = 1;

struct ApplicationFrameHandle {
    std::uint64_t identity;
    std::uint32_t generation;
    std::uint32_t reserved;
};

inline constexpr ApplicationFrameHandle invalidApplicationFrame() noexcept {
    return {0, 0, 0};
}

inline constexpr bool isValid(ApplicationFrameHandle handle) noexcept {
    return handle.identity != 0 && handle.generation != 0 && handle.reserved == 0;
}

// Fixed public phase keys. An exact collision is an error; clients that need
// work around these stages must use a different registration identity or
// source ordinal and rely on the normal public phase ordering.
inline constexpr std::int32_t parameterSnapshotPhasePriorityV1 = 0;
inline constexpr std::int32_t expressionLookAtPhasePriorityV1 = 100;
inline constexpr std::int32_t expressionResolvePhasePriorityV1 = 200;
inline constexpr std::int32_t applicationCommitPhasePriorityV1 = 200;
inline constexpr std::uint64_t parameterSnapshotRegistrationIdentityV1 =
    0x56524d4150500001ull;
inline constexpr std::uint64_t expressionLookAtRegistrationIdentityV1 =
    0x56524d4150500002ull;
inline constexpr std::uint64_t expressionResolveRegistrationIdentityV1 =
    0x56524d4150500003ull;
inline constexpr std::uint64_t applicationCommitRegistrationIdentityV1 =
    0x56524d4150500004ull;
inline constexpr std::uint32_t standardApplicationSourceOrdinalV1 = 0;

enum ExpressionInputFlagsV1 : std::uint32_t {
    expression_input_none = 0,
    expression_input_look_at = 1u << 0u,
    expression_input_reset_history = 1u << 1u,
    expression_input_discontinuity = 1u << 2u,
};

enum ApplicationServiceCapabilityBitsV1 : std::uint64_t {
    application_service_expression_snapshot = 1ull << 0,
    application_service_expression_look_at = 1ull << 1,
    application_service_morph_publish = 1ull << 2,
    application_service_material_publish = 1ull << 3,
    application_service_phase_evaluator = 1ull << 4,
    application_service_diagnostics = 1ull << 5,
    application_service_typed_animation_sink = 1ull << 6,
};

inline constexpr std::uint64_t applicationServiceCapabilitiesV1 =
    application_service_expression_snapshot |
    application_service_expression_look_at |
    application_service_morph_publish |
    application_service_material_publish |
    application_service_phase_evaluator |
    application_service_diagnostics |
    application_service_typed_animation_sink;

enum class ApplicationDiagnosticCodeV1 : std::uint32_t {
    unsupported_material_color_type = 1,
};

struct ExpressionWeightV1 {
    std::uint32_t element_size = sizeof(ExpressionWeightV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    const char *name = nullptr;
    std::uint32_t name_size = 0;
    float value = 0.0f;
    std::uint32_t reserved0 = 0;
};

struct SetExpressionInputDescV1 {
    std::uint32_t struct_size = sizeof(SetExpressionInputDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    Animation::InstanceHandle instance{};
    const ExpressionWeightV1 *weights = nullptr;
    std::uint32_t weight_count = 0;
    std::uint32_t source_ordinal = 0;
    std::uint64_t input_revision = 0;
    float look_at_yaw_degrees = 0.0f;
    float look_at_pitch_degrees = 0.0f;
    std::uint32_t flags = expression_input_none;
    std::uint32_t reserved2 = 0;
};

// Typed animation-source input. Unlike the general input setter, the frame
// revision is explicit so a base-pose evaluator can join the already-open VRM
// application transaction for that same frame.
struct SetTypedAnimationInputDescV1 {
    std::uint32_t struct_size = sizeof(SetTypedAnimationInputDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    Animation::InstanceHandle instance{};
    const ExpressionWeightV1 *weights = nullptr;
    std::uint32_t weight_count = 0;
    std::uint32_t source_ordinal = 0;
    std::uint64_t frame_revision = 0;
    float look_at_yaw_degrees = 0.0f;
    float look_at_pitch_degrees = 0.0f;
    std::uint32_t flags = expression_input_none;
    std::uint32_t reserved2 = 0;
    std::uint64_t asset_identity = 0;
    std::uint32_t asset_generation = 0;
    std::uint32_t profile_version = 0;
};

struct SnapshotExpressionInputDescV1 {
    std::uint32_t struct_size = sizeof(SnapshotExpressionInputDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    Animation::InstanceHandle instance{};
    std::uint64_t frame_revision = 0;
    ApplicationFrameHandle snapshot{};
};

struct EvaluateExpressionLookAtDescV1 {
    std::uint32_t struct_size = sizeof(EvaluateExpressionLookAtDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ApplicationFrameHandle snapshot{};
};

struct ResolveExpressionFrameDescV1 {
    std::uint32_t struct_size = sizeof(ResolveExpressionFrameDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ApplicationFrameHandle snapshot{};
    ApplicationFrameHandle resolved{};
    std::uint32_t expression_count = 0;
    std::uint32_t morph_weight_count = 0;
    std::uint32_t material_override_count = 0;
    std::uint32_t diagnostic_count = 0;
    std::uint32_t source_ordinal = 0;
    std::uint32_t reserved2 = 0;
    std::uint64_t frame_revision = 0;
};

struct QueryResolvedExpressionWeightDescV1 {
    std::uint32_t struct_size = sizeof(QueryResolvedExpressionWeightDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ApplicationFrameHandle resolved{};
    const char *name = nullptr;
    std::uint32_t name_size = 0;
    std::uint32_t reserved2 = 0;
    float value = 0.0f;
    std::uint32_t reserved3 = 0;
};

struct GetApplicationDiagnosticDescV1 {
    std::uint32_t struct_size = sizeof(GetApplicationDiagnosticDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ApplicationFrameHandle resolved{};
    std::uint32_t diagnostic_index = 0;
    ApplicationDiagnosticCodeV1 code =
        ApplicationDiagnosticCodeV1::unsupported_material_color_type;
    std::uint32_t source_material_index = 0;
    std::uint32_t bind_ordinal = 0;
    char *expression_name = nullptr;
    std::uint32_t expression_name_capacity = 0;
    std::uint32_t expression_name_size = 0;
    char *material_color_type = nullptr;
    std::uint32_t material_color_type_capacity = 0;
    std::uint32_t material_color_type_size = 0;
};

struct PublishApplicationFrameDescV1 {
    std::uint32_t struct_size = sizeof(PublishApplicationFrameDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ApplicationFrameHandle resolved{};
};

struct DiscardApplicationFrameDescV1 {
    std::uint32_t struct_size = sizeof(DiscardApplicationFrameDescV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    ApplicationFrameHandle frame{};
};

struct ApplicationServiceV1 {
    std::uint32_t struct_size = sizeof(ApplicationServiceV1);
    std::uint32_t version = applicationDescriptorVersionV1;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t service_version = 0;
    std::uint32_t minimum_client_service_version = 0;
    std::uint64_t capability_bits = 0;
    void *context = nullptr;
    Animation::Status (*set_expression_inputs)(void *,
                                                const SetExpressionInputDescV1 *) = nullptr;
    Animation::Status (*snapshot_expression_inputs)(void *,
                                                     SnapshotExpressionInputDescV1 *) = nullptr;
    Animation::Status (*evaluate_expression_look_at)(void *,
                                                      const EvaluateExpressionLookAtDescV1 *) = nullptr;
    Animation::Status (*resolve_expression_frame)(void *,
                                                   ResolveExpressionFrameDescV1 *) = nullptr;
    Animation::Status (*query_expression_weight)(void *,
                                                  QueryResolvedExpressionWeightDescV1 *) = nullptr;
    Animation::Status (*get_diagnostic)(void *, GetApplicationDiagnosticDescV1 *) = nullptr;
    Animation::Status (*publish_application_frame)(void *,
                                                    const PublishApplicationFrameDescV1 *) = nullptr;
    Animation::Status (*discard_application_frame)(void *,
                                                    const DiscardApplicationFrameDescV1 *) = nullptr;
    // Additive VRMA-I0 tail.
    Animation::Status (*set_typed_animation_inputs)(
        void *, const SetTypedAnimationInputDescV1 *) = nullptr;
};

// Separate additive negotiation surface. animation/abi_v1.hpp remains frozen.
PELICAN_API Animation::Status
getApplicationServiceV1(std::uint32_t client_service_version,
                        ApplicationServiceV1 *out_service) noexcept;

static_assert(sizeof(ApplicationFrameHandle) == 16);
static_assert(std::is_standard_layout_v<ExpressionWeightV1>);
static_assert(offsetof(SetExpressionInputDescV1, struct_size) == 0);
static_assert(offsetof(ApplicationServiceV1, struct_size) == 0);

} // namespace Pelican::Vrm
