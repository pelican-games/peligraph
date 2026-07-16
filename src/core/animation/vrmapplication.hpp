#pragma once

#include "../model/modeltemplate.hpp"
#include "../userpublic/animation/vrm_application_v1.hpp"

#include <glm/glm.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Pelican::Vrm {

struct ExpressionInputSnapshot {
    Animation::InstanceHandle instance{};
    std::uint64_t input_revision = 0;
    std::uint64_t frame_revision = 0;
    std::uint32_t source_ordinal = 0;
    std::uint32_t flags = expression_input_none;
    bool look_at_enabled = false;
    float look_at_yaw_degrees = 0.0f;
    float look_at_pitch_degrees = 0.0f;
    std::map<std::string, float, std::less<>> expression_weights;
};

struct ApplicationDiagnostic {
    ApplicationDiagnosticCodeV1 code =
        ApplicationDiagnosticCodeV1::unsupported_material_color_type;
    std::string expression_name;
    std::string material_color_type;
    std::uint32_t source_material_index = noSourceMaterialIndex;
    std::uint32_t bind_ordinal = 0;
};

struct ResolvedMaterialOverride {
    std::uint32_t source_material_index = noSourceMaterialIndex;
    std::uint32_t mask = 0;
    glm::vec4 base_color_factor{1.0f};
    glm::vec4 emissive_factor{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv_offset{0.0f};
    glm::vec2 uv_scale{1.0f};
    float uv_rotation = 0.0f;
};

struct ResolvedExpressionFrame {
    Animation::InstanceHandle instance{};
    std::uint64_t input_revision = 0;
    std::uint64_t frame_revision = 0;
    std::uint32_t source_ordinal = 0;
    std::uint32_t flags = expression_input_none;
    std::uint64_t morph_layout_generation = 0;
    std::map<std::string, float, std::less<>> expression_weights;
    std::vector<float> morph_weights;
    std::vector<ResolvedMaterialOverride> material_overrides;
    std::vector<ApplicationDiagnostic> diagnostics;
};

// Adds expression-type lookAt procedural weights to an immutable frame copy.
// Bone lookAt and target/camera-to-yaw/pitch pose staging are intentionally S1c.
Animation::Status evaluateExpressionLookAt(const VrmSemanticData &semantic,
                                           ExpressionInputSnapshot &snapshot) noexcept;

Animation::Status resolveExpressionFrame(
    const VrmSemanticData &semantic, const MorphTargetLayout *morph_layout,
    const SourceMaterialInitialValueTable *material_initial_values,
    const ExpressionInputSnapshot &snapshot,
    ResolvedExpressionFrame &resolved) noexcept;

class ApplicationServiceRuntime {
  public:
    ApplicationServiceRuntime();
    ~ApplicationServiceRuntime();
    ApplicationServiceRuntime(const ApplicationServiceRuntime &) = delete;
    ApplicationServiceRuntime &operator=(const ApplicationServiceRuntime &) = delete;

    Animation::Status ensureStandardPhasesRegistered() noexcept;
    void reset() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend ApplicationServiceRuntime &applicationServiceRuntime();
    friend Animation::Status getApplicationServiceV1(
        std::uint32_t, ApplicationServiceV1 *) noexcept;
};

ApplicationServiceRuntime &applicationServiceRuntime();

} // namespace Pelican::Vrm
