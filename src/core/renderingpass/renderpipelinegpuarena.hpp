#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

class ComputeTaskContainer;
class DebugDraw;
class DebugText;
class FrameGraphResourceContainer;
class FullscreenPassContainer;
class PipelineFactory;
class RenderTargetContainer;
class ShaderLibrary;
class ShadowDepthPassContainer;
class VelocityPassContainer;

enum class RenderPipelineGpuResourceKind {
    render_target,
    frame_graph_buffer,
    shader_bundle,
    pipeline,
    fullscreen_pass,
    compute_task,
    debug_draw_pass,
    debug_text_pass,
    shadow_depth_pass,
    velocity_pass,
};

std::string_view renderPipelineGpuResourceKindName(
    RenderPipelineGpuResourceKind kind) noexcept;

struct RenderPipelineGpuResourceRegistration {
    RenderPipelineGpuResourceKind kind =
        RenderPipelineGpuResourceKind::render_target;
    std::int64_t handle = -1;
    std::string name;
    std::uint64_t declared_bytes = 0;

    bool operator==(
        const RenderPipelineGpuResourceRegistration &) const = default;
};

// One append-only owner scope prepared by the GPU registration transaction.
// Type-erased leases are available for registries that can already transfer
// ownership into the runtime root. The legacy Vulkan registries currently
// contribute records and remain engine-owned until scope replacement lands.
struct RenderPipelineGpuScopePreparation {
    std::string owner_scope;
    std::vector<RenderPipelineGpuResourceRegistration> resources;
    std::vector<std::shared_ptr<const void>> resource_leases;
};

struct RenderPipelineGpuResourceScope {
    std::string owner_scope;
    std::vector<RenderPipelineGpuResourceRegistration> resources;
    std::vector<std::shared_ptr<const void>> resource_leases;
};

// Immutable metadata and lifetime root paired with a runtime generation.
// RPE10b1 is append-only: replacing/removing an existing owner scope is
// deliberately rejected until the scope-aware replacement WP.
struct RenderPipelineGpuArena {
    std::uint64_t runtime_generation = 0;
    std::vector<RenderPipelineGpuResourceScope> scopes;

    const RenderPipelineGpuResourceScope *findScope(
        std::string_view owner_scope) const noexcept;
    std::size_t resourceCount() const noexcept;
};

std::shared_ptr<const RenderPipelineGpuArena>
compileRenderPipelineGpuArena(
    const std::shared_ptr<const RenderPipelineGpuArena> &current,
    std::uint64_t runtime_generation,
    std::optional<RenderPipelineGpuScopePreparation> prepared_scope);

struct RenderPipelineGpuRegistrationDependencies {
    RenderTargetContainer &render_targets;
    FrameGraphResourceContainer &frame_graph_buffers;
    ComputeTaskContainer &compute_tasks;
    FullscreenPassContainer &fullscreen_passes;
    ShaderLibrary &shader_library;
    PipelineFactory &pipeline_factory;
    ShadowDepthPassContainer &shadow_depth_passes;
    VelocityPassContainer &velocity_passes;
};

struct RenderPipelineGpuRegistryCounts {
    std::size_t render_targets = 0;
    std::size_t frame_graph_buffers = 0;
    std::size_t compute_tasks = 0;
    std::size_t fullscreen_passes = 0;
    std::size_t shader_bundles = 0;
    std::size_t pipelines = 0;
    std::size_t shadow_depth_passes = 0;
    std::size_t velocity_passes = 0;
    std::size_t debug_draw_passes = 0;
    std::size_t debug_text_passes = 0;

    bool operator==(const RenderPipelineGpuRegistryCounts &) const = default;
};

RenderPipelineGpuRegistryCounts inspectRenderPipelineGpuRegistryCounts(
    const RenderPipelineGpuRegistrationDependencies &dependencies,
    const DebugDraw *debug_draw = nullptr,
    const DebugText *debug_text = nullptr) noexcept;

// Captures append-only checkpoints across every GPU registry touched by
// render-config compilation. Unless commit() is called, destruction rolls
// every enlisted registry back to its exact pre-prepare registration state.
class RenderPipelineGpuRegistrationArena {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit RenderPipelineGpuRegistrationArena(
        RenderPipelineGpuRegistrationDependencies dependencies);
    ~RenderPipelineGpuRegistrationArena();

    RenderPipelineGpuRegistrationArena(
        const RenderPipelineGpuRegistrationArena &) = delete;
    RenderPipelineGpuRegistrationArena &operator=(
        const RenderPipelineGpuRegistrationArena &) = delete;
    RenderPipelineGpuRegistrationArena(
        RenderPipelineGpuRegistrationArena &&) = delete;
    RenderPipelineGpuRegistrationArena &operator=(
        RenderPipelineGpuRegistrationArena &&) = delete;

    void enlist(DebugDraw &debug_draw);
    void enlist(DebugText &debug_text);

    RenderPipelineGpuScopePreparation preparedScope(
        std::string owner_scope) const;

    void commit() noexcept;
    void rollback() noexcept;
    bool active() const noexcept;
};

} // namespace Pelican
