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
#include <atomic>
#include <limits>
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
        const auto existing = std::find_if(
            candidate->scopes.begin(),
            candidate->scopes.end(),
            [&prepared_scope](const auto &scope) {
                return scope.owner_scope ==
                       prepared_scope->owner_scope;
            });
        RenderPipelineGpuResourceScope replacement{
            std::move(prepared_scope->owner_scope),
            std::move(prepared_scope->resources),
            std::move(prepared_scope->resource_leases),
        };
        if (existing != candidate->scopes.end()) {
            *existing = std::move(replacement);
        } else {
            candidate->scopes.push_back(
                std::move(replacement));
        }
    }
    return candidate;
}

namespace {

template <typename Module>
bool moduleIsLive(Module *module) noexcept {
    return module != nullptr &&
           FastModuleContainer::tryGet<Module>() == module;
}

class ScopeRegistrationLease {
    RenderPipelineGpuRegistrationDependencies dependencies_;
    DebugDraw *debug_draw_ = nullptr;
    DebugText *debug_text_ = nullptr;
    std::vector<GlobalRenderTargetId> render_targets_;
    std::vector<FrameGraphBufferId> frame_graph_buffers_;
    std::vector<ComputeTaskId> compute_tasks_;
    std::vector<FullscreenPassContainer::PipelineId>
        fullscreen_passes_;
    std::vector<ShaderBundleId> shader_bundles_;
    std::vector<PipelineHandle> pipelines_;
    std::vector<PassId> debug_draw_passes_;
    std::vector<PassId> debug_text_passes_;
    std::vector<PassId> shadow_depth_passes_;
    std::vector<PassId> velocity_passes_;
    std::atomic_bool armed_{false};

  public:
    ScopeRegistrationLease(
        RenderPipelineGpuRegistrationDependencies dependencies,
        DebugDraw *debug_draw, DebugText *debug_text,
        const std::vector<RenderPipelineGpuResourceRegistration>
            &resources)
        : dependencies_{dependencies},
          debug_draw_{debug_draw},
          debug_text_{debug_text} {
        for (const auto &resource : resources) {
            if (resource.handle >
                std::numeric_limits<int>::max()) {
                throw std::runtime_error(
                    "Render pipeline GPU resource handle is too large");
            }
            const auto int_handle =
                static_cast<int>(resource.handle);
            switch (resource.kind) {
            case RenderPipelineGpuResourceKind::render_target:
                render_targets_.push_back(
                    GlobalRenderTargetId{int_handle});
                break;
            case RenderPipelineGpuResourceKind::frame_graph_buffer:
                frame_graph_buffers_.push_back(
                    FrameGraphBufferId{int_handle});
                break;
            case RenderPipelineGpuResourceKind::shader_bundle:
                shader_bundles_.push_back(
                    ShaderBundleId{int_handle});
                break;
            case RenderPipelineGpuResourceKind::pipeline:
                pipelines_.push_back(
                    PipelineHandle{int_handle});
                break;
            case RenderPipelineGpuResourceKind::fullscreen_pass:
                if (resource.handle >
                    std::numeric_limits<std::uint32_t>::max()) {
                    throw std::runtime_error(
                        "Fullscreen pass handle is too large");
                }
                fullscreen_passes_.push_back(
                    FullscreenPassContainer::PipelineId{
                        static_cast<std::uint32_t>(
                            resource.handle)});
                break;
            case RenderPipelineGpuResourceKind::compute_task:
                compute_tasks_.push_back(
                    ComputeTaskId{int_handle});
                break;
            case RenderPipelineGpuResourceKind::debug_draw_pass:
                debug_draw_passes_.push_back(
                    PassId{int_handle});
                break;
            case RenderPipelineGpuResourceKind::debug_text_pass:
                debug_text_passes_.push_back(
                    PassId{int_handle});
                break;
            case RenderPipelineGpuResourceKind::shadow_depth_pass:
                shadow_depth_passes_.push_back(
                    PassId{int_handle});
                break;
            case RenderPipelineGpuResourceKind::velocity_pass:
                velocity_passes_.push_back(
                    PassId{int_handle});
                break;
            }
        }
    }

    void arm() noexcept {
        armed_.store(true, std::memory_order_release);
    }

    ~ScopeRegistrationLease() {
        if (!armed_.load(std::memory_order_acquire)) return;

        // Drop descriptor/pass records before the pipelines and shaders
        // they reference. Each registry defers its Vulkan payload through
        // the engine deletion queue when it is still accepting resources.
        if (moduleIsLive(debug_text_)) {
            debug_text_->retireRegistrations(
                debug_text_passes_);
        }
        if (moduleIsLive(debug_draw_)) {
            debug_draw_->retireRegistrations(
                debug_draw_passes_);
        }
        if (moduleIsLive(
                &dependencies_.velocity_passes)) {
            dependencies_.velocity_passes.retireRegistrations(
                velocity_passes_);
        }
        if (moduleIsLive(
                &dependencies_.shadow_depth_passes)) {
            dependencies_.shadow_depth_passes.retireRegistrations(
                shadow_depth_passes_);
        }
        if (moduleIsLive(
                &dependencies_.fullscreen_passes)) {
            dependencies_.fullscreen_passes.retireRegistrations(
                fullscreen_passes_);
        }
        if (moduleIsLive(&dependencies_.compute_tasks)) {
            dependencies_.compute_tasks.retireRegistrations(
                compute_tasks_);
        }
        if (moduleIsLive(
                &dependencies_.frame_graph_buffers)) {
            dependencies_.frame_graph_buffers.retireRegistrations(
                frame_graph_buffers_);
        }
        if (moduleIsLive(&dependencies_.render_targets)) {
            dependencies_.render_targets.retireRegistrations(
                render_targets_);
        }
        if (moduleIsLive(&dependencies_.pipeline_factory)) {
            dependencies_.pipeline_factory.retireRegistrations(
                pipelines_);
        }
        if (moduleIsLive(&dependencies_.shader_library)) {
            dependencies_.shader_library.retireRegistrations(
                shader_bundles_);
        }
    }
};

void hideReplacedScopeNames(
    const RenderPipelineGpuResourceScope &scope,
    RenderPipelineGpuRegistrationDependencies &dependencies) {
    for (const auto &resource : scope.resources) {
        if (resource.handle < 0 ||
            resource.handle >
                std::numeric_limits<int>::max()) {
            continue;
        }
        const auto handle =
            static_cast<int>(resource.handle);
        switch (resource.kind) {
        case RenderPipelineGpuResourceKind::render_target:
            dependencies.render_targets.hideRegistrationName(
                resource.name,
                GlobalRenderTargetId{handle});
            break;
        case RenderPipelineGpuResourceKind::frame_graph_buffer:
            dependencies.frame_graph_buffers
                .hideRegistrationName(
                    resource.name,
                    FrameGraphBufferId{handle});
            break;
        case RenderPipelineGpuResourceKind::compute_task:
            dependencies.compute_tasks.hideRegistrationName(
                resource.name, ComputeTaskId{handle});
            break;
        default:
            break;
        }
    }
}

} // namespace

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
    std::shared_ptr<ScopeRegistrationLease>
        prepared_ownership;
    bool active = true;

    explicit Impl(
        RenderPipelineGpuRegistrationDependencies dependencies_value,
        const RenderPipelineGpuResourceScope *replaced_scope)
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
                  .checkpointRegistrations()} {
        if (replaced_scope != nullptr) {
            hideReplacedScopeNames(
                *replaced_scope, dependencies);
        }
    }

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
        RenderPipelineGpuRegistrationDependencies dependencies,
        const RenderPipelineGpuResourceScope *replaced_scope)
    : impl_{std::make_unique<Impl>(
          dependencies, replaced_scope)} {}

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
    std::string owner_scope) {
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
    for (const auto &[name, id, bytes] :
         impl_->dependencies.frame_graph_buffers
             .registrationsSince(
                 impl_->frame_graph_buffer_checkpoint)) {
        append(
            RenderPipelineGpuResourceKind::frame_graph_buffer,
            id.value, name,
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
    if (impl_->prepared_ownership != nullptr) {
        throw std::runtime_error(
            "Render pipeline GPU scope was prepared more than once");
    }
    impl_->prepared_ownership =
        std::make_shared<ScopeRegistrationLease>(
            impl_->dependencies, impl_->debug_draw,
            impl_->debug_text, result.resources);
    result.resource_leases.push_back(
        impl_->prepared_ownership);
    return result;
}

void RenderPipelineGpuRegistrationArena::commit() noexcept {
    if (!impl_) return;
    if (impl_->prepared_ownership != nullptr) {
        impl_->prepared_ownership->arm();
    }
    impl_->active = false;
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
