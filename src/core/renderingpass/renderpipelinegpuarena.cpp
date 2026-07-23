#include "renderpipelinegpuarena.hpp"

#include "computetask.hpp"
#include "rendertargetcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/shaderlibrary.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

std::string numericResourceName(std::string_view prefix,
                                std::int64_t handle) {
    return std::string{prefix} + "/" + std::to_string(handle);
}

void validatePreparedScope(
    const RenderPipelineGpuScopePreparation &scope) {
    if (scope.owner_scope.empty()) {
        throw std::runtime_error(
            "Render pipeline GPU owner scope must not be empty");
    }
    struct Key {
        RenderPipelineGpuResourceKind kind;
        std::int64_t handle;

        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key &key) const noexcept {
            const auto kind =
                static_cast<std::size_t>(key.kind);
            const auto handle =
                std::hash<std::int64_t>{}(key.handle);
            return handle ^ (kind + 0x9e3779b9u +
                             (handle << 6u) +
                             (handle >> 2u));
        }
    };
    std::unordered_set<Key, KeyHash> registrations;
    registrations.reserve(scope.resources.size());
    for (const auto &resource : scope.resources) {
        if (resource.handle < 0 || resource.name.empty()) {
            throw std::runtime_error(
                "Render pipeline GPU resource registration is invalid");
        }
        if (!registrations
                 .emplace(Key{resource.kind, resource.handle})
                 .second) {
            throw std::runtime_error(
                "Render pipeline GPU resource registration is duplicated");
        }
    }
}

} // namespace

std::string_view renderPipelineGpuResourceKindName(
    RenderPipelineGpuResourceKind kind) noexcept {
    switch (kind) {
    case RenderPipelineGpuResourceKind::render_target:
        return "render_target";
    case RenderPipelineGpuResourceKind::frame_graph_buffer:
        return "frame_graph_buffer";
    case RenderPipelineGpuResourceKind::shader_bundle:
        return "shader_bundle";
    case RenderPipelineGpuResourceKind::pipeline:
        return "pipeline";
    case RenderPipelineGpuResourceKind::fullscreen_pass:
        return "fullscreen_pass";
    case RenderPipelineGpuResourceKind::compute_task:
        return "compute_task";
    case RenderPipelineGpuResourceKind::debug_draw_pass:
        return "debug_draw_pass";
    case RenderPipelineGpuResourceKind::debug_text_pass:
        return "debug_text_pass";
    case RenderPipelineGpuResourceKind::shadow_depth_pass:
        return "shadow_depth_pass";
    case RenderPipelineGpuResourceKind::velocity_pass:
        return "velocity_pass";
    }
    return "unknown";
}

const RenderPipelineGpuResourceScope *
RenderPipelineGpuArena::findScope(
    std::string_view owner_scope) const noexcept {
    const auto found = std::find_if(
        scopes.begin(), scopes.end(),
        [owner_scope](const auto &scope) {
            return scope.owner_scope == owner_scope;
        });
    return found != scopes.end() ? &*found : nullptr;
}

std::size_t RenderPipelineGpuArena::resourceCount() const noexcept {
    std::size_t count = 0;
    for (const auto &scope : scopes) {
        count += scope.resources.size();
    }
    return count;
}

std::shared_ptr<const RenderPipelineGpuArena>
compileRenderPipelineGpuArena(
    const std::shared_ptr<const RenderPipelineGpuArena> &current,
    std::uint64_t runtime_generation,
    std::optional<RenderPipelineGpuScopePreparation> prepared_scope) {
    auto candidate =
        current != nullptr
            ? std::make_shared<RenderPipelineGpuArena>(*current)
            : std::make_shared<RenderPipelineGpuArena>();
    candidate->runtime_generation = runtime_generation;
    if (prepared_scope) {
        validatePreparedScope(*prepared_scope);
        if (candidate->findScope(
                prepared_scope->owner_scope) != nullptr) {
            throw std::runtime_error(
                "Render pipeline GPU owner scope is already published: " +
                prepared_scope->owner_scope);
        }
        candidate->scopes.push_back(
            RenderPipelineGpuResourceScope{
                std::move(prepared_scope->owner_scope),
                std::move(prepared_scope->resources),
                std::move(prepared_scope->resource_leases),
            });
    }
    return candidate;
}

struct RenderPipelineGpuRegistrationArena::Impl {
    RenderPipelineGpuRegistrationDependencies dependencies;
    RenderTargetContainer::RegistrationCheckpoint
        render_target_checkpoint;
    FrameGraphResourceContainer::RegistrationCheckpoint
        frame_graph_buffer_checkpoint;
    ComputeTaskContainer::RegistrationCheckpoint
        compute_task_checkpoint;
    FullscreenPassContainer::RegistrationCheckpoint
        fullscreen_pass_checkpoint;
    ShaderLibrary::RegistrationCheckpoint
        shader_checkpoint;
    PipelineFactory::RegistrationCheckpoint
        pipeline_checkpoint;
    ShadowDepthPassContainer::RegistrationCheckpoint
        shadow_depth_checkpoint;
    VelocityPassContainer::RegistrationCheckpoint
        velocity_checkpoint;
    DebugDraw *debug_draw = nullptr;
    std::optional<DebugDraw::RegistrationCheckpoint>
        debug_draw_checkpoint;
    DebugText *debug_text = nullptr;
    std::optional<DebugText::RegistrationCheckpoint>
        debug_text_checkpoint;
    bool active = true;

    explicit Impl(
        RenderPipelineGpuRegistrationDependencies dependencies_value)
        : dependencies{dependencies_value},
          render_target_checkpoint{
              dependencies.render_targets
                  .checkpointRegistrations()},
          frame_graph_buffer_checkpoint{
              dependencies.frame_graph_buffers
                  .checkpointRegistrations()},
          compute_task_checkpoint{
              dependencies.compute_tasks
                  .checkpointRegistrations()},
          fullscreen_pass_checkpoint{
              dependencies.fullscreen_passes
                  .checkpointRegistrations()},
          shader_checkpoint{
              dependencies.shader_library
                  .checkpointRegistrations()},
          pipeline_checkpoint{
              dependencies.pipeline_factory
                  .checkpointRegistrations()},
          shadow_depth_checkpoint{
              dependencies.shadow_depth_passes
                  .checkpointRegistrations()},
          velocity_checkpoint{
              dependencies.velocity_passes
                  .checkpointRegistrations()} {}

    void rollback() {
        if (!active) return;
        if (debug_text != nullptr && debug_text_checkpoint) {
            debug_text->rollbackRegistrations(
                *debug_text_checkpoint);
        }
        if (debug_draw != nullptr && debug_draw_checkpoint) {
            debug_draw->rollbackRegistrations(
                *debug_draw_checkpoint);
        }
        dependencies.velocity_passes.rollbackRegistrations(
            velocity_checkpoint);
        dependencies.shadow_depth_passes.rollbackRegistrations(
            shadow_depth_checkpoint);
        dependencies.fullscreen_passes.rollbackRegistrations(
            fullscreen_pass_checkpoint);
        dependencies.compute_tasks.rollbackRegistrations(
            compute_task_checkpoint);
        dependencies.frame_graph_buffers.rollbackRegistrations(
            frame_graph_buffer_checkpoint);
        dependencies.render_targets.rollbackRegistrations(
            render_target_checkpoint);
        dependencies.pipeline_factory.rollbackRegistrations(
            pipeline_checkpoint);
        dependencies.shader_library.rollbackRegistrations(
            shader_checkpoint);
        active = false;
    }
};

RenderPipelineGpuRegistrationArena::
    RenderPipelineGpuRegistrationArena(
        RenderPipelineGpuRegistrationDependencies dependencies)
    : impl_{std::make_unique<Impl>(dependencies)} {}

RenderPipelineGpuRegistrationArena::
    ~RenderPipelineGpuRegistrationArena() {
    rollback();
}

void RenderPipelineGpuRegistrationArena::enlist(
    DebugDraw &debug_draw) {
    if (!impl_->active) {
        throw std::runtime_error(
            "Render pipeline GPU registration arena is inactive");
    }
    if (impl_->debug_draw != nullptr &&
        impl_->debug_draw != &debug_draw) {
        throw std::runtime_error(
            "Render pipeline GPU registration arena changed DebugDraw owner");
    }
    if (impl_->debug_draw == nullptr) {
        impl_->debug_draw = &debug_draw;
        impl_->debug_draw_checkpoint =
            debug_draw.checkpointRegistrations();
    }
}

void RenderPipelineGpuRegistrationArena::enlist(
    DebugText &debug_text) {
    if (!impl_->active) {
        throw std::runtime_error(
            "Render pipeline GPU registration arena is inactive");
    }
    if (impl_->debug_text != nullptr &&
        impl_->debug_text != &debug_text) {
        throw std::runtime_error(
            "Render pipeline GPU registration arena changed DebugText owner");
    }
    if (impl_->debug_text == nullptr) {
        impl_->debug_text = &debug_text;
        impl_->debug_text_checkpoint =
            debug_text.checkpointRegistrations();
    }
}

RenderPipelineGpuScopePreparation
RenderPipelineGpuRegistrationArena::preparedScope(
    std::string owner_scope) const {
    if (!impl_->active) {
        throw std::runtime_error(
            "Render pipeline GPU registration arena is inactive");
    }
    RenderPipelineGpuScopePreparation result;
    result.owner_scope = std::move(owner_scope);
    const auto append = [&result](
                            RenderPipelineGpuResourceKind kind,
                            std::int64_t handle,
                            std::string name,
                            std::uint64_t declared_bytes = 0) {
        result.resources.push_back(
            RenderPipelineGpuResourceRegistration{
                kind, handle, std::move(name),
                declared_bytes});
    };

    for (const auto &[name, id] :
         impl_->dependencies.render_targets
             .registrationsSince(
                 impl_->render_target_checkpoint)) {
        append(RenderPipelineGpuResourceKind::render_target,
               id.value, name);
    }
    std::int64_t buffer_ordinal = 0;
    for (const auto &[name, bytes] :
         impl_->dependencies.frame_graph_buffers
             .registrationsSince(
                 impl_->frame_graph_buffer_checkpoint)) {
        append(
            RenderPipelineGpuResourceKind::frame_graph_buffer,
            buffer_ordinal++, name,
            static_cast<std::uint64_t>(bytes));
    }
    for (const auto id :
         impl_->dependencies.shader_library
             .registrationsSince(impl_->shader_checkpoint)) {
        append(RenderPipelineGpuResourceKind::shader_bundle,
               id.value,
               numericResourceName("shader_bundle", id.value));
    }
    for (const auto handle :
         impl_->dependencies.pipeline_factory
             .registrationsSince(impl_->pipeline_checkpoint)) {
        append(RenderPipelineGpuResourceKind::pipeline,
               handle.value,
               numericResourceName("pipeline", handle.value));
    }
    for (const auto id :
         impl_->dependencies.fullscreen_passes
             .registrationsSince(
                 impl_->fullscreen_pass_checkpoint)) {
        append(RenderPipelineGpuResourceKind::fullscreen_pass,
               id.value,
               numericResourceName("fullscreen_pass",
                                   id.value));
    }
    for (const auto &[name, id] :
         impl_->dependencies.compute_tasks
             .registrationsSince(
                 impl_->compute_task_checkpoint)) {
        append(RenderPipelineGpuResourceKind::compute_task,
               id.value, name);
    }
    if (impl_->debug_draw != nullptr &&
        impl_->debug_draw_checkpoint) {
        for (const auto id :
             impl_->debug_draw->registrationsSince(
                 *impl_->debug_draw_checkpoint)) {
            append(RenderPipelineGpuResourceKind::debug_draw_pass,
                   id.value,
                   numericResourceName("debug_draw_pass",
                                       id.value));
        }
    }
    if (impl_->debug_text != nullptr &&
        impl_->debug_text_checkpoint) {
        for (const auto id :
             impl_->debug_text->registrationsSince(
                 *impl_->debug_text_checkpoint)) {
            append(RenderPipelineGpuResourceKind::debug_text_pass,
                   id.value,
                   numericResourceName("debug_text_pass",
                                       id.value));
        }
    }
    for (const auto id :
         impl_->dependencies.shadow_depth_passes
             .registrationsSince(
                 impl_->shadow_depth_checkpoint)) {
        append(
            RenderPipelineGpuResourceKind::shadow_depth_pass,
            id.value,
            numericResourceName("shadow_depth_pass",
                                id.value));
    }
    for (const auto id :
         impl_->dependencies.velocity_passes
             .registrationsSince(
                 impl_->velocity_checkpoint)) {
        append(RenderPipelineGpuResourceKind::velocity_pass,
               id.value,
               numericResourceName("velocity_pass",
                                   id.value));
    }
    return result;
}

void RenderPipelineGpuRegistrationArena::commit() noexcept {
    if (impl_) impl_->active = false;
}

void RenderPipelineGpuRegistrationArena::rollback() noexcept {
    if (!impl_ || !impl_->active) return;
    try {
        impl_->rollback();
    } catch (...) {
        // A rollback failure would leave handles referring to partially
        // destroyed Vulkan state. Continuing is less safe than terminating.
        std::terminate();
    }
}

bool RenderPipelineGpuRegistrationArena::active() const noexcept {
    return impl_ != nullptr && impl_->active;
}

RenderPipelineGpuRegistryCounts
inspectRenderPipelineGpuRegistryCounts(
    const RenderPipelineGpuRegistrationDependencies &dependencies,
    const DebugDraw *debug_draw,
    const DebugText *debug_text) noexcept {
    return RenderPipelineGpuRegistryCounts{
        .render_targets =
            dependencies.render_targets.registrationCount(),
        .frame_graph_buffers =
            dependencies.frame_graph_buffers.registrationCount(),
        .compute_tasks =
            dependencies.compute_tasks.registrationCount(),
        .fullscreen_passes =
            dependencies.fullscreen_passes.registrationCount(),
        .shader_bundles =
            dependencies.shader_library.registrationCount(),
        .pipelines =
            dependencies.pipeline_factory.registrationCount(),
        .shadow_depth_passes =
            dependencies.shadow_depth_passes.registrationCount(),
        .velocity_passes =
            dependencies.velocity_passes.registrationCount(),
        .debug_draw_passes =
            debug_draw != nullptr
                ? debug_draw->registrationCount()
                : 0,
        .debug_text_passes =
            debug_text != nullptr
                ? debug_text->registrationCount()
                : 0,
    };
}

} // namespace Pelican
