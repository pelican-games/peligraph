#include "renderer.hpp"
#include "../userpublic/valuegraph.hpp"
#include "../renderingpass/graphinputs.hpp"
#include "../userpublic/gamecontext.hpp"
#include "../renderer/camera.hpp"
#include "accelerationstructure.hpp"
#include "../renderer/fullscreenpassrenderer.hpp"
#include "../renderer/frameresources.hpp"
#include "../renderer/materialrender.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../renderer/proceduralgeometrycontainer.hpp"
#include "../renderer/shadowdepthpasscontainer.hpp"
#include "../renderer/skyambientlighting.hpp"
#include "../renderer/spriterenderer.hpp"
#include "../renderer/spritescene.hpp"
#include "../renderer/atlasassetresource.hpp"
#include "../renderer/velocitypasscontainer.hpp"
#include "../renderer/uicontainer.hpp"
#include "../renderer/uirenderer.hpp"
#include "../ui/module.hpp"
#include "../watch/assetkey.hpp"
#include "../watch/reloadgate.hpp"
#include "../watch/reloadservice.hpp"
#include "../launchconfig.hpp"
#include "../light/lightcontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/renderconfigcandidate.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/standardmaterialresource.hpp"
#include "../renderer/debugdraw.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderer/gizmo.hpp"
#include "../renderer/directionalshadowviewprovider.hpp"
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
#include "../render_algorithms/punctual_shadow/punctualshadowviewprovider.hpp"
#endif
#include "../renderer/viewfamilyproviderregistry.hpp"
#include "../model/vertbufcontainer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/viewexecutionscheduler.hpp"
#include "../renderingpass/renderingpassjsonhelpers.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderingpass/vulkannativescopeexecutor.hpp"
#include "../loader/scene.hpp"
#include "../ecs/core.hpp"
#include "../os/window.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../appflow/enginetime.hpp"
#include "../../project/frameresolutionwire.hpp"
#include "../../project/vulkanviewplanning.hpp"
#include "deletionqueue.hpp"
#include "core.hpp"
#include "debugutils.hpp"
#include "externaldepthsubmission.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_executor.hpp"
#include "render_pass_frame_setup.hpp"
#include "renderer_config.hpp"
#include "rendertarget.hpp"
#include "rendertiming.hpp"
#include "shaderresolution.hpp"
#include "util.hpp"
#if PELICAN_WITH_OPENXR
#include "../openxr/openxrmirrorsink.hpp"
#endif
#if PELICAN_WITH_IMGUI
#include "../imgui/imguiruntime.hpp"
#include "../imgui/imguisystem.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_inverse.hpp>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Pelican {

struct RenderPipelineReloadState {
    std::string source_reference;
    std::set<watch::AssetKey> watched_sources;
    std::uint64_t attempted = 0;
    std::uint64_t applied = 0;
    std::uint64_t failed = 0;
    std::uint64_t last_generation = 0;
    std::uint64_t staged_generation = 0;
    std::string last_error;
};

bool Renderer::hasPendingRenderPipelinePublication() const noexcept {
    return render_pipeline_reload_state && render_pipeline_reload_state->staged_generation != 0;
}

// Everything read by a delayed ray-layout publication hook is owned here.
// In particular, ReloadService's batch vectors and the authoring call's
// candidate/source callback are gone by the time the submitted frame commits.
struct RenderPipelinePublicationTransaction {
    std::vector<watch::ReloadRequest> companion_requests;
    std::unique_ptr<watch::PreparedReloadTransaction> model_reload;
    std::unique_ptr<SceneLoader::PreparedGraphRuntimeTransfer> graph_runtime_transfer;
    std::optional<RenderConfigCandidateDocumentSet> candidate_documents;
    std::function<void()> source_commit;
    RenderPipelineAuthoringCompletion completion;
    std::optional<RenderPipelineAuthoringApplyResult> outcome;
    std::optional<RenderGraphVariantConfig> variants;
    std::set<watch::AssetKey> watched_sources;
    std::string config_json;
    std::string failure;
    // The only per-path wording: each construction site fills it in.
    std::string rollback_failure_message;
    RenderingPassId current_rendering_pass_id = invalidRenderingPassId();
    std::uint64_t generation = 0;
    bool authored = false;
    bool completion_required = false;
    bool runtime_published = false;
    bool ready = false;
    bool finalized = false;

    // Both render pipeline reload paths publish and roll back by the same
    // rules; the renderer state they touch is reached through `renderer`.
    void publish(Renderer &renderer) noexcept;
    void rollback(Renderer &renderer) noexcept;
};

void RenderPipelinePublicationTransaction::publish(
    Renderer &renderer) noexcept {
    if (finalized || !ready || !runtime_published) {
        return;
    }
    if (graph_runtime_transfer) graph_runtime_transfer->commit();
    std::string post_commit_error;
    auto published_variants = std::move(*variants);
    renderer.flat_rendering_pass_id = published_variants.flat;
    renderer.xr_rendering_pass_id = published_variants.xr;
    renderer.xr_excluded_features =
        std::move(published_variants.xr_excluded_features);
    renderer.preview_graph_program =
        std::move(published_variants.preview);
    renderer.current_rendering_pass_id =
        current_rendering_pass_id;
    GET_MODULE(ProjectBasicConfig)
        .publishRenderingConfigJson(
            std::move(config_json));

    try {
        if (auto *targets =
                FastModuleContainer::tryGet<
                    RenderTargetContainer>()) {
            targets->resetHistory();
        }
        if (auto *instances =
                FastModuleContainer::tryGet<
                    PolygonInstanceContainer>()) {
            instances->resetTemporalHistory();
        }
        renderer.render_target_layout_tracker.reset();
        renderer.temporal_reset_requested = true;
    } catch (const std::exception &caught) {
        post_commit_error = caught.what();
    } catch (...) {
        post_commit_error =
            "unknown post-commit render pipeline cleanup failure";
    }

    if (renderer.render_pipeline_reload_state != nullptr) {
        auto &published =
            *renderer.render_pipeline_reload_state;
        published.watched_sources =
            std::move(watched_sources);
        published.last_generation = generation;
        published.staged_generation = 0;
        published.last_error.clear();
        ++published.applied;
    }
    outcome =
        RenderPipelineAuthoringApplyResult{
            .committed = true,
            .published_generation = generation,
            .post_commit_error =
                std::move(post_commit_error),
        };
    finalized = true;
    if (completion_required && completion) {
        completion(*outcome);
    }
}

void RenderPipelinePublicationTransaction::rollback(
    Renderer &renderer) noexcept {
    model_reload.reset();
    if (finalized) return;
    if (failure.empty()) {
        failure = rollback_failure_message;
    }
    if (renderer.render_pipeline_reload_state != nullptr) {
        auto &rolled_back =
            *renderer.render_pipeline_reload_state;
        rolled_back.staged_generation = 0;
        rolled_back.last_error = failure;
        ++rolled_back.failed;
    }
    outcome =
        RenderPipelineAuthoringApplyResult{
            .error = failure,
        };
    finalized = true;
    if (completion_required && completion) {
        completion(*outcome);
    }
}

namespace {

bool programRequiresRayQuery(
    const CompiledRenderProgram &program) {
    const auto &target_plan =
        program.frame_graph.target_plan;
    return target_plan != nullptr &&
           std::find(
               target_plan->required_physical_features.begin(),
               target_plan->required_physical_features.end(),
               vulkanRayQueryCapability) !=
               target_plan->required_physical_features.end();
}

std::shared_ptr<RayQueryAccelerationStructureScope>
rayQueryAccelerationStructureScope(
    const RendererRuntimeGeneration &generation,
    const CompiledRenderProgram &program) {
    const auto planned = programRequiresRayQuery(program);
    if (generation.gpu_arena == nullptr) {
        if (!planned) return {};
        throw std::runtime_error(
            "ray-query program has no GPU resource arena");
    }
    const auto *scope = generation.gpu_arena->findScope(
        program.owner_scope);
    if (scope != nullptr &&
        scope->acceleration_structures != nullptr) {
        return scope->acceleration_structures;
    }
    if (planned) {
        throw std::runtime_error(
            "ray-query program has no acceleration-structure GPU "
            "owner scope");
    }
    return {};
}

const RenderPipelineGpuResourceScope *renderPipelineGpuOwnerScope(
    const RendererRuntimeGeneration &generation,
    const CompiledRenderProgram &program) noexcept {
    return generation.gpu_arena != nullptr
               ? generation.gpu_arena->findScope(program.owner_scope)
               : nullptr;
}

std::optional<watch::AssetKey> projectAssetKeyForReference(
    std::string_view reference) {
    if (reference.empty() ||
        reference.starts_with("engine://") ||
        reference.starts_with("user://")) {
        return std::nullopt;
    }
    try {
        return watch::makeAssetKey(reference);
    } catch (...) {
        // Absolute/foreign sources are loadable only through explicit policy
        // and do not belong to the project watcher identity space.
        return std::nullopt;
    }
}

std::set<watch::AssetKey> renderPipelineWatchSources(
    std::string_view root_reference,
    const RendererRuntimeGeneration &generation,
    RenderingPassId flat_rendering_pass_id) {
    std::set<watch::AssetKey> result;
    const auto append =
        [&result](std::string_view reference) {
            if (auto key =
                    projectAssetKeyForReference(
                        reference)) {
                result.insert(std::move(*key));
            }
        };
    append(root_reference);
    const auto *program =
        generation.find(flat_rendering_pass_id);
    if (program == nullptr ||
        !program->frame_graph.render_pipeline) {
        return result;
    }
    const auto &pipeline =
        *program->frame_graph.render_pipeline;
    const auto &resolver = GET_MODULE(PathResolver);
    for (const auto &file : program->frame_graph.attribute_source_files)
        if (auto key = watch::assetKeyForFile(resolver, file)) result.insert(std::move(*key));
    if (pipeline.pipeline_preset) {
        append(pipeline.pipeline_preset->reference);
    }
    for (const auto &feature :
         pipeline.feature_instances) {
        append(feature.reference);
    }
    if (pipeline.ray_group_layout_reference) {
        append(*pipeline.ray_group_layout_reference);
    }
    if (pipeline.ray_bindings_reference) {
        append(*pipeline.ray_bindings_reference);
    }
    return result;
}

struct SpriteRenderModules {
    SpriteScene *scene = nullptr;
    SpriteRenderer *renderer = nullptr;
    AtlasAssetResource *atlas = nullptr;
};

struct ShaderHotReloadModules {
    watch::ReloadService *reload_service = nullptr;
};

struct RenderFrameModules {
    const DebugUtilsDispatch &debug_utils;
    RenderTarget &render_target;
    RenderTargetContainer &render_target_container;
    RenderingPassContainer &rendering_pass_container;
    RenderPassExecutor &pass_executor;
    FrameGraphRuntimeContainer &frame_graph_runtime;
    ComputeTaskContainer &compute_task_container;
    FrameGraphResourceContainer &frame_graph_resources;
    VulkanUtils &vk_utils;
    MaterialRenderer &material_renderer;
    ShadowDepthPassContainer &shadow_depth_pass_container;
    VelocityPassContainer &velocity_pass_container;
    PolygonInstanceContainer &instance_container;
    const VertBufContainer &vert_buf_container;
    const MaterialContainer &material_container;
    FrameResources &frame_resources;
    FullscreenPassRenderer &fullscreen_pass_renderer;
    FullscreenPassContainer &fullscreen_pass_container;
    UiRenderer *ui_renderer;
    const UIContainer *ui_container;
    const ui::UiModule *ui_module;
    DebugDraw *debug_draw;
    Gizmo *gizmo;
    DebugText *debug_text;
#if PELICAN_WITH_IMGUI
    ImGuiSystem *imgui_system;
#endif
    RenderTiming *render_timing;
    const Camera &camera;
    LightContainer &light_container;
    SpriteRenderModules sprite;
};

struct RenderViewFamilyExecutionState {
    const RenderViewFamily *family = nullptr;
    std::vector<RenderFrameSnapshot> snapshots;
    std::vector<FrameUniformData> frame_uniforms;
    std::vector<FrameResolutionUniformData>
        frame_resolutions;
    std::uint32_t sequential_slot_base = 0;
};

const RenderViewFamilyExecutionState &
requireRenderViewFamilyExecutionState(
    std::span<const RenderViewFamilyExecutionState>
        states,
    std::string_view family_id) {
    const auto found = std::find_if(
        states.begin(), states.end(),
        [family_id](
            const RenderViewFamilyExecutionState
                &state) {
            return state.family != nullptr &&
                   state.family->family_id ==
                       family_id;
        });
    if (found == states.end()) {
        throw std::runtime_error(
            "render execution has no prepared view family '" +
            std::string{family_id} + "'");
    }
    return *found;
}

SpriteRenderModules resolveSpriteRenderModules(const RenderingPassContainer &rendering_pass_container) {
    if (!rendering_pass_container.isFeatureEnabled("sprite")) return {};

    auto *scene = FastModuleContainer::tryGet<SpriteScene>();
    if (scene == nullptr || scene->commandCountForTesting() == 0) return {};

    return {scene, &GET_MODULE(SpriteRenderer), &GET_MODULE(AtlasAssetResource)};
}

ShaderHotReloadModules resolveShaderHotReloadModules() {
    auto &gate = GET_MODULE(watch::ReloadGate);
    // EngineLaunchConfig remains a compatibility adapter. Syncing here keeps
    // direct renderer fixtures and legacy embedders on the centralized gate.
    gate.configureFromLaunch(GET_MODULE(EngineLaunchConfig));
    if (!gate.shaderReloadEnabled()) {
        // A publication committed at frame start still has to be consumed if
        // a dynamic determinism gate closes before render start.
        return {FastModuleContainer::tryGet<watch::ReloadService>()};
    }

    // The composition root creates the domain modules. ReloadService then
    // invokes them through the shared runtime-participant boundary.
    (void)GET_MODULE(ShaderLibrary);
    (void)GET_MODULE(PipelineFactory);
    return {&GET_MODULE(watch::ReloadService)};
}

DeletionQueue &resolveFrameDeletionQueue() {
    return GET_MODULE(DeletionQueue);
}

EngineTime &resolveFrameEngineTime() {
    return GET_MODULE(EngineTime);
}

RenderFrameModules resolveRenderFrameModules() {
    auto &rendering_pass_container = GET_MODULE(RenderingPassContainer);
    DebugDraw *debug_draw =
        rendering_pass_container.isFeatureEnabled("debug_draw") ? &GET_MODULE(DebugDraw) : nullptr;
    DebugText *debug_text =
        rendering_pass_container.isFeatureEnabled("debug_text") ? &GET_MODULE(DebugText) : nullptr;
    Gizmo *gizmo = rendering_pass_container.isFeatureEnabled("gizmo")
                       ? &GET_MODULE(Gizmo)
                       : nullptr;
    RenderTiming *render_timing =
        rendering_pass_container.isFeatureEnabled("gpu_timing") ? &GET_MODULE(RenderTiming) : nullptr;
#if PELICAN_WITH_IMGUI
    ImGuiSystem *imgui_system = nullptr;
    invokeImGuiRuntimeCallback(GET_MODULE(EngineLaunchConfig), [&] {
        imgui_system = &GET_MODULE(ImGuiSystem);
    });
#endif
    const bool ui_enabled = rendering_pass_container.isFeatureEnabled("ui");

    return RenderFrameModules{
        GET_MODULE(VulkanManageCore).getDebugUtils(),
        GET_MODULE(RenderTarget),
        GET_MODULE(RenderTargetContainer),
        rendering_pass_container,
        GET_MODULE(RenderPassExecutor),
        GET_MODULE(FrameGraphRuntimeContainer),
        GET_MODULE(ComputeTaskContainer),
        GET_MODULE(FrameGraphResourceContainer),
        GET_MODULE(VulkanUtils),
        GET_MODULE(MaterialRenderer),
        GET_MODULE(ShadowDepthPassContainer),
        GET_MODULE(VelocityPassContainer),
        GET_MODULE(PolygonInstanceContainer),
        GET_MODULE(VertBufContainer),
        GET_MODULE(MaterialContainer),
        GET_MODULE(FrameResources),
        GET_MODULE(FullscreenPassRenderer),
        GET_MODULE(FullscreenPassContainer),
        ui_enabled ? &GET_MODULE(UiRenderer) : nullptr,
        ui_enabled ? &GET_MODULE(UIContainer) : nullptr,
        ui_enabled ? &GET_MODULE(ui::UiModule) : nullptr,
        debug_draw,
        gizmo,
        debug_text,
#if PELICAN_WITH_IMGUI
        imgui_system,
#endif
        render_timing,
        GET_MODULE(Camera),
        GET_MODULE(LightContainer),
        resolveSpriteRenderModules(rendering_pass_container),
    };
}

void updateFrameLights(
    LightContainer &light_container,
    FrameGraphResourceContainer
        &frame_graph_resources,
    const RenderViewFamilies
        &view_families,
    const CompiledRenderPipeline
        &render_pipeline) {
    const auto sky_ambient =
        resolveSkyAmbientLighting(
            render_pipeline);
    if (const auto *shadow_family =
            view_families.find(
                directionalShadowRenderViewFamilyId);
        shadow_family != nullptr) {
        std::vector<glm::mat4>
            view_projections;
        std::vector<float>
            cascade_far_distances;
        const auto directional_light_count =
            light_container.directionalLightCount();
        const auto light_slots =
            std::max<std::size_t>(
                1, directional_light_count);
        if (shadow_family->views.empty() ||
            shadow_family->views.size() % light_slots != 0) {
            throw std::runtime_error(
                "directional shadow view family cardinality does not divide by the light count");
        }
        const auto cascade_count_size =
            shadow_family->views.size() / light_slots;
        if (cascade_count_size >
                std::numeric_limits<std::uint32_t>::max() ||
            directional_light_count >
                std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error(
                "directional shadow family exceeds the runtime index range");
        }
        const auto cascade_count =
            static_cast<std::uint32_t>(
                cascade_count_size);
        const auto shadow_light_count =
            static_cast<std::uint32_t>(
                directional_light_count);
        view_projections.reserve(
            shadow_family->views.size());
        cascade_far_distances.reserve(
            cascade_count);
        for (const auto &shadow_view :
             shadow_family->views) {
            view_projections.push_back(
                shadow_view.projection *
                shadow_view.view);
        }
        for (std::uint32_t cascade = 0;
             cascade < cascade_count; ++cascade) {
            const auto &shadow_view =
                shadow_family->views[cascade];
            cascade_far_distances.push_back(
                shadow_view.depth_range
                    ? shadow_view
                          .depth_range
                          ->far_distance
                    : std::numeric_limits<
                          float>::max());
        }
        light_container.updateDirectionalShadows(
            view_projections,
            cascade_far_distances,
            cascade_count,
            shadow_light_count,
            sky_ambient);
    } else {
        light_container.update(
            sky_ambient);
    }
    constexpr auto source =
        FrameGraphHostBufferSource::scene_lights_v2;
    for (const auto &target : frame_graph_resources.hostBufferTargets(
             FrameGraphHostBufferSource::view_family_matrices_v1)) {
        const auto &definition = frame_graph_resources.definition(target.id);
        const auto &family = view_families.require(definition.host_view_family);
        const auto data = packRenderViewFamilyMatrices(family);
        frame_graph_resources.writeHostBuffer(target.id, std::as_bytes(std::span(data)),
            FrameGraphResourceContainer::HostBufferPopulation{family.views.size(), family.views.size()});
    }
    if (!frame_graph_resources
             .hasHostBufferSource(source)) {
        return;
    }
    for (const auto &target :
         frame_graph_resources.hostBufferTargets(
             source)) {
        const auto packed =
            light_container.lightInventoryV2(
                target.size);
        frame_graph_resources.writeHostBuffer(
            target.id,
            std::as_bytes(
                std::span{
                    packed.elements.data(),
                    packed.elements.size()}),
            FrameGraphResourceContainer::
                HostBufferPopulation{
                    packed.input_count,
                    packed.accepted_count,
                });
    }
}

void prepareSecondaryViewFamilyDraws(
    PolygonInstanceContainer &instances,
    const RenderViewFamilies &view_families,
    std::span<const std::string>
        locally_sorted_families) {
    instances.prepareViewFamilyDraws(
        view_families,
        locally_sorted_families);
}

void updateFrameDrawCandidates(
    const PolygonInstanceContainer
        &instance_container,
    FrameGraphResourceContainer
        &frame_graph_resources) {
    constexpr auto command_source =
        FrameGraphHostBufferSource::
            scene_draw_commands_v1;
    constexpr auto bounds_source =
        FrameGraphHostBufferSource::
            scene_draw_bounds_v1;
    constexpr auto segment_source =
        FrameGraphHostBufferSource::
            scene_draw_segments_v1;
    const auto needs_commands =
        frame_graph_resources
            .hasHostBufferSource(
                command_source);
    const auto needs_bounds =
        frame_graph_resources
            .hasHostBufferSource(
                bounds_source);
    const auto needs_segments =
        frame_graph_resources
            .hasHostBufferSource(
                segment_source);
    if (!needs_commands && !needs_bounds &&
        !needs_segments) {
        return;
    }
    const auto candidates =
        instance_container
            .sceneDrawCandidatesForFrameGraph();
    const auto upload =
        [&](FrameGraphHostBufferSource source,
            const auto &records) {
            using Record =
                typename std::decay_t<
                    decltype(records)>::value_type;
            for (const auto &target :
                 frame_graph_resources
                     .hostBufferTargets(source)) {
                const auto capacity =
                    static_cast<std::size_t>(
                        target.size /
                        sizeof(Record));
                std::vector<Record> staging(
                    capacity);
                const auto written =
                    std::min(
                        capacity,
                        records.size());
                std::copy_n(
                    records.begin(), written,
                    staging.begin());
                frame_graph_resources
                    .writeHostBuffer(
                        target.id,
                        std::as_bytes(
                            std::span{
                                staging.data(),
                                staging.size()}),
                        FrameGraphResourceContainer::
                            HostBufferPopulation{
                                records.size(),
                                written,
                            });
            }
        };
    if (needs_commands) {
        upload(command_source,
               candidates.commands);
    }
    if (needs_bounds) {
        upload(bounds_source,
               candidates.bounds);
    }
    if (needs_segments) {
        upload(segment_source,
               candidates.segments);
    }
}

glm::vec4 resolutionVector(vk::Extent2D extent) {
    const auto inverse_width =
        extent.width == 0
            ? 0.0f
            : 1.0f /
                  static_cast<float>(extent.width);
    const auto inverse_height =
        extent.height == 0
            ? 0.0f
            : 1.0f /
                  static_cast<float>(extent.height);
    return glm::vec4{
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        inverse_width, inverse_height};
}

vk::Extent2D resolvePlannedExtent(
    const ResourceExtentPlan &plan,
    vk::Extent2D output_extent) {
    const auto resolved = resolveResourceExtent(
        plan, ResolvedResourceExtent{
                  .width = output_extent.width,
                  .height = output_extent.height,
              });
    return {resolved.width, resolved.height};
}

vk::Extent2D resolveResolutionSourceExtent(
    std::string_view resource,
    const CompiledFrameGraphExecution &frame_graph,
    const RenderTargetContainer &render_targets,
    vk::Extent2D output_extent) {
    if (resource == "swapchain") return output_extent;
    const auto binding =
        frame_graph.render_target_bindings.find(
            std::string{resource});
    if (binding ==
            frame_graph.render_target_bindings.end() ||
        !isConcreteRenderTarget(binding->second)) {
        throw std::runtime_error(
            "Frame resolution source is not bound to a concrete render target: " +
            std::string{resource});
    }
    return render_targets.getMetadata(binding->second).extent;
}

struct FrameResolutionExtents {
    vk::Extent2D render;
    vk::Extent2D output;
};

std::optional<vk::Extent2D>
resolveViewFamilyRasterExtent(
    std::string_view family_id,
    const CompiledRenderingPass
        &rendering_pass,
    const CompiledFrameGraphExecution
        &frame_graph,
    const RenderTargetContainer
        &render_targets) {
    std::optional<vk::Extent2D> result;
    const auto observe =
        [&](GlobalRenderTargetId target) {
            if (!isConcreteRenderTarget(
                    target)) {
                return;
            }
            const auto extent =
                render_targets
                    .getMetadata(target)
                    .extent;
            if (!result) {
                result = extent;
            } else if (*result != extent) {
                throw std::runtime_error(
                    "render view family '" +
                    std::string{family_id} +
                    "' spans multiple raster extents; pass-specific "
                    "family resolution is required");
            }
        };
    for (const auto &node :
         frame_graph.nodes) {
        if (node.view_family != family_id ||
            node.kind !=
                FramePlanNodeKind::render) {
            continue;
        }
        const auto &pass =
            rendering_pass.passes.at(
                node.index);
        for (const auto target :
             pass.definition.output_color) {
            observe(target);
        }
        observe(
            pass.definition.output_depth);
    }
    return result;
}

FrameResolutionUniformData frameResolutionData(
    vk::Extent2D render_extent,
    vk::Extent2D output_extent) {
    return FrameResolutionUniformData{
        .render_resolution =
            resolutionVector(render_extent),
        .output_resolution =
            resolutionVector(output_extent),
    };
}

FrameResolutionExtents resolveFrameResolutionExtents(
    const CompiledFrameGraphExecution &frame_graph,
    const RenderTargetContainer &render_targets,
    vk::Extent2D output_extent) {
    if (!frame_graph.target_plan ||
        !frame_graph.target_plan->resolution_plan) {
        return {output_extent, output_extent};
    }
    const auto &plan =
        *frame_graph.target_plan->resolution_plan;
    const auto render_extent =
        resolveResolutionSourceExtent(
            plan.render_source_resource, frame_graph,
            render_targets, output_extent);
    const auto resolved_output_extent =
        resolveResolutionSourceExtent(
            plan.output_source_resource, frame_graph,
            render_targets, output_extent);
    const auto planned_render =
        resolvePlannedExtent(
            plan.render_extent, output_extent);
    if (render_extent != planned_render) {
        throw std::runtime_error(
            "Runtime render extent does not match the compiled resolution plan");
    }
    if (resolvePlannedExtent(
            plan.output_extent, output_extent) !=
        resolved_output_extent) {
        throw std::runtime_error(
            "Runtime output extent does not match the compiled resolution plan");
    }
    return {render_extent, resolved_output_extent};
}

FrameUniformData updateFrameResources(
    RenderFrameModules &modules, EngineTime &engine_time,
    vk::Extent2D render_extent, vk::Extent2D output_extent,
    const RenderFrameSnapshot &snapshot,
    const std::optional<RenderViewClipPlane>
        &clip_plane,
    std::uint32_t view_index,
    std::uint32_t view_count,
    std::string_view view_family) {
    const auto frame_index = engine_time.frameIndex();
    const auto family_token =
        renderViewFamilyToken(
            view_family);

    FrameUniformData data;
    data.time_delta = glm::vec4{static_cast<float>(engine_time.now()),
                                static_cast<float>(engine_time.dt()), 0.0f, 0.0f};
    data.frame_index = glm::uvec4{static_cast<uint32_t>(frame_index),
                                  static_cast<uint32_t>(frame_index >> 32),
                                  family_token[0], family_token[1]};
    // Legacy consumers remain output-relative. New render-resolution-aware
    // shaders use the dedicated PelicanResolutionUBO below.
    data.resolution = resolutionVector(output_extent);
    data.camera_position = glm::vec4{snapshot.camera_position, 1.0f};
    data.view = snapshot.view;
    data.projection = snapshot.projection_jittered;
    data.previous_view = snapshot.previous_view;
    data.previous_projection = snapshot.previous_projection_jittered;
    data.jitter_ndc = snapshot.jitter_ndc;
    data.previous_jitter_ndc = snapshot.previous_jitter_ndc;
    data.temporal_reset_epoch = snapshot.temporal_reset_epoch;
    data.previous_temporal_reset_epoch = snapshot.previous_temporal_reset_epoch;
    data.view_index = view_index;
    data.view_count = view_count;
    if (clip_plane) {
        data.clip_plane =
            glm::vec4{
                clip_plane->normal,
                clip_plane->offset};
    }

    modules.frame_resources.setSceneBuffers(modules.instance_container.getObjectBuf(),
                                             modules.instance_container.getPreviousObjectBuf(),
                                             modules.light_container.lightBuffer(),
                                             modules.light_container.directionalShadowBuffer());
    modules.frame_resources.update(data);
    modules.frame_resources.updateResolution(
        frameResolutionData(
            render_extent, output_extent));
    return data;
}

std::string loadOpName(vk::AttachmentLoadOp op) {
    switch (op) {
    case vk::AttachmentLoadOp::eLoad:
        return "load";
    case vk::AttachmentLoadOp::eClear:
        return "clear";
    case vk::AttachmentLoadOp::eDontCare:
        return "dont_care";
    default:
        return vk::to_string(op);
    }
}

std::string storeOpName(vk::AttachmentStoreOp op) {
    switch (op) {
    case vk::AttachmentStoreOp::eStore:
        return "store";
    case vk::AttachmentStoreOp::eDontCare:
        return "dont_care";
    default:
        return vk::to_string(op);
    }
}

std::string layoutName(vk::ImageLayout layout) {
    switch (layout) {
    case vk::ImageLayout::eUndefined:
        return "undefined";
    case vk::ImageLayout::eGeneral:
        return "general";
    case vk::ImageLayout::eColorAttachmentOptimal:
        return "color_attachment_optimal";
    case vk::ImageLayout::eDepthAttachmentOptimal:
        return "depth_attachment_optimal";
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return "shader_read_only_optimal";
    case vk::ImageLayout::eRenderingLocalReadKHR:
        return "rendering_local_read";
    case vk::ImageLayout::eTransferSrcOptimal:
        return "transfer_src_optimal";
    case vk::ImageLayout::eTransferDstOptimal:
        return "transfer_dst_optimal";
    case vk::ImageLayout::ePresentSrcKHR:
        return "present_src";
    default:
        return vk::to_string(layout);
    }
}

std::string renderTargetName(GlobalRenderTargetId rt_id, const RenderTargetContainer &rt_container) {
    if (isSwapchainRenderTarget(rt_id)) {
        return "swapchain";
    }
    if (!isConcreteRenderTarget(rt_id)) {
        return "none";
    }
    return rt_container.getMetadata(rt_id).name;
}

std::string trackedLayoutName(GlobalRenderTargetId rt_id, vk::ImageLayout special_layout,
                              const RenderTargetLayoutTracker &layout_tracker,
                              const RenderTargetContainer &rt_container,
                              bool history_read = false) {
    return layoutName(isConcreteRenderTarget(rt_id)
                          ? layout_tracker.currentLayout(rt_id, history_read, &rt_container)
                          : special_layout);
}

nlohmann::json clearColorJson(
    const std::array<double, 4> &clear_color) {
    return nlohmann::json::array(
        {clear_color[0], clear_color[1],
         clear_color[2], clear_color[3]});
}

nlohmann::json renderNodeTrace(const CompiledPass &pass, size_t order,
                               const RenderTargetContainer &rt_container,
                               const RenderTargetLayoutTracker &layout_tracker) {
    const auto &definition = pass.definition;
    nlohmann::json attachments = nlohmann::json::array();
    for (std::size_t index = 0;
         index < definition.output_color.size();
         ++index) {
        const auto target =
            definition.output_color[index];
        const auto operations =
            definition.colorAttachmentOperations(
                index);
        const auto format = isConcreteRenderTarget(target)
                                ? rt_container.getMetadata(target).format
                                : vk::Format::eUndefined;
        nlohmann::json attachment{
            {"resource", renderTargetName(target, rt_container)},
            {"aspect", "color"},
            {"load", loadOpName(operations.load_op)},
            {"store", storeOpName(operations.store_op)},
            {"final_layout", trackedLayoutName(target, vk::ImageLayout::eColorAttachmentOptimal,
                                                 layout_tracker, rt_container)},
            {"format", isConcreteRenderTarget(target) ? formatToString(format) : "frame_target"},
            {"samples", 1},
        };
        if (operations.load_op ==
            vk::AttachmentLoadOp::eClear) {
            attachment["clear"] =
                clearColorJson(
                    definition
                        .logicalColorClearValue(
                            index));
        }
        attachments.push_back(std::move(attachment));
    }
    if (isConcreteRenderTarget(definition.output_depth)) {
        const auto operations =
            definition.depthAttachmentOperations();
        nlohmann::json attachment{
            {"resource", renderTargetName(definition.output_depth, rt_container)},
            {"aspect", "depth"},
            {"load", loadOpName(operations.load_op)},
            {"store", storeOpName(operations.store_op)},
            {"final_layout", trackedLayoutName(definition.output_depth,
                                                 vk::ImageLayout::eDepthAttachmentOptimal,
                                                 layout_tracker, rt_container)},
        };
        if (operations.load_op ==
            vk::AttachmentLoadOp::eClear) {
            attachment["clear"] = 1.0;
        }
        attachments.push_back(std::move(attachment));
    }

    nlohmann::json inputs = nlohmann::json::array();
    for (size_t i = 0; i < definition.input_targets.size(); ++i) {
        const auto target = definition.input_targets[i];
        const bool history_read = definition.input_target_history.at(i);
        inputs.push_back({
            {"resource", renderTargetName(target, rt_container) + (history_read ? "@history" : "")},
            {"final_layout", trackedLayoutName(target, vk::ImageLayout::eShaderReadOnlyOptimal,
                                                 layout_tracker, rt_container, history_read)},
        });
    }

    return nlohmann::json{
        {"name", definition.name},
        {"kind", "render"},
        {"order", order},
        {"inputs", std::move(inputs)},
        {"input_buffers", definition.input_buffers},
        {"attachments", std::move(attachments)},
        {"blend", definition.isUi() || definition.isDebugDraw() ||
                      definition.isGizmo() || definition.isDebugText()
#if PELICAN_WITH_IMGUI
                      || definition.isImGui()
#endif
        },
    };
}

nlohmann::json anchorNodeTrace(const std::string &name, size_t order) {
    return nlohmann::json{{"name", name}, {"kind", "anchor"}, {"order", order}};
}

std::size_t formatTexelBytes(vk::Format format) {
    switch (format) {
    case vk::Format::eR8Unorm:
    case vk::Format::eR8Snorm:
    case vk::Format::eR8Uint:
    case vk::Format::eR8Sint:
        return 1;
    case vk::Format::eR8G8Unorm:
    case vk::Format::eR8G8Snorm:
    case vk::Format::eR8G8Uint:
    case vk::Format::eR8G8Sint:
    case vk::Format::eR16Sfloat:
    case vk::Format::eR16Unorm:
    case vk::Format::eR16Snorm:
    case vk::Format::eR16Uint:
    case vk::Format::eR16Sint:
    case vk::Format::eD16Unorm:
        return 2;
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Snorm:
    case vk::Format::eR8G8B8A8Uint:
    case vk::Format::eR8G8B8A8Sint:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eA2B10G10R10UnormPack32:
    case vk::Format::eA2R10G10B10UnormPack32:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eR16G16Unorm:
    case vk::Format::eR16G16Snorm:
    case vk::Format::eR16G16Uint:
    case vk::Format::eR16G16Sint:
    case vk::Format::eR16G16Sfloat:
    case vk::Format::eR32Uint:
    case vk::Format::eR32Sint:
    case vk::Format::eR32Sfloat:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat:
        return 4;
    case vk::Format::eR16G16B16A16Unorm:
    case vk::Format::eR16G16B16A16Snorm:
    case vk::Format::eR16G16B16A16Uint:
    case vk::Format::eR16G16B16A16Sint:
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32Uint:
    case vk::Format::eR32G32Sint:
    case vk::Format::eR32G32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return 8;
    case vk::Format::eR32G32B32A32Uint:
    case vk::Format::eR32G32B32A32Sint:
    case vk::Format::eR32G32B32A32Sfloat:
        return 16;
    default: return 0;
    }
}

vk::ImageAspectFlags snapshotAspect(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD32SfloatS8Uint:
        return vk::ImageAspectFlagBits::eDepth;
    default:
        return vk::ImageAspectFlagBits::eColor;
    }
}

nlohmann::json snapshotCopyTrace(const FramePlanNode &node, const RenderTargetMetadata &source,
                                 const RenderTargetMetadata &destination) {
    return nlohmann::json{
        {"name", node.name},
        {"kind", "snapshot_copy"},
        {"order", node.order},
        {"snapshot_after", node.snapshot_after},
        {"source", source.name},
        {"destination", destination.name},
        {"format", formatToString(destination.format)},
        {"aspect", snapshotAspect(destination.format) ==
                           vk::ImageAspectFlagBits::eDepth
                       ? "depth"
                       : "color"},
        {"extent", {destination.extent.width, destination.extent.height}},
        {"texel_block_bytes", formatTexelBytes(destination.format)},
        {"byte_size", static_cast<std::size_t>(destination.extent.width) * destination.extent.height *
                          formatTexelBytes(destination.format)},
        {"semantics", "fixed_once_before_transparency"},
        {"sequential_refraction", false},
        {"transitions", nlohmann::json::array({
            {{"resource", source.name}, {"new_layout", "transfer_src_optimal"}},
            {{"resource", destination.name}, {"new_layout", "transfer_dst_optimal"}},
            {{"resource", destination.name}, {"new_layout", "shader_read_only_optimal"}},
        })},
    };
}

nlohmann::json outputTransformTrace(size_t order, vk::ImageLayout source_old_layout,
                                    vk::ImageLayout destination_final_layout,
                                    const RenderTargetMetadata &display,
                                    vk::Format destination_format,
                                    const std::vector<std::string> &paired_storage_edges) {
    const bool fallback = destination_format == vk::Format::eR8G8B8A8Unorm ||
                          destination_format == vk::Format::eB8G8R8A8Unorm;
    return nlohmann::json{
        {"name", "output_transform"},
        {"kind", "output_transform"},
        {"order", order},
        {"mode", fallback ? "shader_oetf_unorm_fallback" : "hardware_srgb_encode"},
        {"source", "display"},
        {"destination", "swapchain"},
        {"source_format", formatToString(display.format)},
        {"destination_format", formatToString(destination_format)},
        {"extent", {display.extent.width, display.extent.height}},
        {"samples", 1},
        {"texel_block_bytes", 4},
        {"color_counters", {{"tone_curve", 1},
                            {"terminal_display_encode", 1},
                            {"shader_oetf", fallback ? 1 : 0},
                            {"paired_storage_round_trip", paired_storage_edges.size()}}},
        {"paired_storage_edges", paired_storage_edges},
        {"queue_ownership", "same_family_or_concurrent; VK_QUEUE_FAMILY_IGNORED"},
        {"transitions",
         nlohmann::json::array({
             {{"resource", "display"},
              {"old_layout", layoutName(source_old_layout)},
              {"new_layout", "shader_read_only_optimal"},
              {"src_stage", "color_attachment_output"},
              {"src_access", "color_attachment_read|color_attachment_write"},
              {"dst_stage", "fragment_shader"},
              {"dst_access", "shader_read"}},
             {{"resource", "swapchain"},
              {"old_layout", "color_attachment_optimal"},
              {"new_layout", "color_attachment_optimal"},
              {"src_stage", "color_attachment_output"},
              {"src_access", "color_attachment_read|color_attachment_write"},
              {"dst_stage", "color_attachment_output"},
              {"dst_access", "color_attachment_write"}},
             {{"resource", "swapchain"},
              {"old_layout", "color_attachment_optimal"},
              {"new_layout", layoutName(destination_final_layout)},
              {"src_stage", "color_attachment_output"},
              {"src_access", "color_attachment_write"},
              {"dst_stage", destination_final_layout == vk::ImageLayout::ePresentSrcKHR
                                    ? "bottom_of_pipe"
                                    : "transfer"},
              {"dst_access", destination_final_layout == vk::ImageLayout::ePresentSrcKHR
                                     ? "none"
                                     : "transfer_read"}},
         })},
    };
}

GlobalRenderTargetId boundRenderTarget(
    const CompiledFrameGraphExecution &frame_graph,
    const std::string &name) {
    const auto found =
        frame_graph.render_target_bindings.find(
            name);
    return found !=
                   frame_graph.render_target_bindings.end()
               ? found->second
               : noRenderTargetId();
}

FrameGraphBufferId boundFrameGraphBuffer(
    const CompiledFrameGraphExecution &frame_graph,
    const std::string &name) {
    const auto found =
        frame_graph.buffer_bindings.find(
            name);
    return found != frame_graph.buffer_bindings.end()
               ? found->second
               : noFrameGraphBufferId();
}

nlohmann::json computeNodeTrace(const CompiledComputeTask &task, size_t order,
                                const CompiledFrameGraphExecution &frame_graph,
                                const RenderTargetContainer &rt_container,
                                const RenderTargetLayoutTracker &layout_tracker) {
    const auto trace_resources = [&](const std::vector<std::string> &resources) {
        nlohmann::json result = nlohmann::json::array();
        for (const auto &resource : resources) {
            nlohmann::json entry{{"resource", resource}};
            const auto target =
                boundRenderTarget(frame_graph, resource);
            if (isConcreteRenderTarget(target)) {
                entry["final_layout"] = layoutName(layout_tracker.currentLayout(target, false, &rt_container));
            } else {
                entry["kind"] = "buffer";
            }
            result.push_back(std::move(entry));
        }
        return result;
    };

    return nlohmann::json{
        {"name", task.definition.name},
        {"kind", "compute"},
        {"order", order},
        {"reads", trace_resources(task.definition.reads)},
        {"writes", trace_resources(task.definition.writes)},
    };
}

nlohmann::json finalLayoutsTrace(const CompiledRenderingPass &rendering_pass,
                                 const CompiledFrameGraphExecution &frame_graph,
                                 const RenderTargetContainer &rt_container,
                                 const RenderTargetLayoutTracker &layout_tracker,
                                 vk::ImageLayout frame_target_layout) {
    std::map<std::string, std::string> layouts;
    const auto add_target = [&](GlobalRenderTargetId target) {
        if (isSwapchainRenderTarget(target)) {
            layouts["swapchain"] = layoutName(frame_target_layout);
        } else if (isConcreteRenderTarget(target)) {
            layouts[renderTargetName(target, rt_container)] =
                layoutName(layout_tracker.currentLayout(target, false, &rt_container));
        }
    };
    for (const auto &pass : rendering_pass.passes) {
        for (const auto target : pass.definition.input_targets) {
            add_target(target);
        }
        for (const auto target : pass.definition.output_color) {
            add_target(target);
        }
        add_target(pass.definition.output_depth);
    }
    for (const auto &task : rendering_pass.compute_tasks) {
        for (const auto &resource : task.definition.reads) {
            add_target(boundRenderTarget(
                frame_graph, resource));
        }
        for (const auto &resource : task.definition.writes) {
            add_target(boundRenderTarget(
                frame_graph, resource));
        }
    }
    const auto display =
        boundRenderTarget(frame_graph, "display");
    if (isConcreteRenderTarget(display)) {
        add_target(display);
    }
    layouts["swapchain"] = layoutName(frame_target_layout);

    nlohmann::json result = nlohmann::json::array();
    for (const auto &[resource, layout] : layouts) {
        result.push_back({{"resource", resource}, {"layout", layout}});
    }
    return result;
}

GpuTimingNodeDescriptor plannedTimingNode(
    const CompiledFrameGraphExecution &frame_graph,
    const SpriteRenderModules &sprite,
    std::size_t ordinal) {
    const auto &node = frame_graph.nodes.at(ordinal);
    bool native_scope_has_work = false;
    if (frame_graph.native_scopes != nullptr &&
        frame_graph.target_plan != nullptr) {
        const auto scope = std::find_if(
            frame_graph.target_plan->scopes.begin(),
            frame_graph.target_plan->scopes.end(),
            [&](const auto &candidate) {
                return std::find(
                           candidate.nodes.begin(),
                           candidate.nodes.end(),
                           node.name) !=
                       candidate.nodes.end();
            });
        native_scope_has_work =
            scope !=
                frame_graph.target_plan->scopes.end() &&
            !scope->nodes.empty() &&
            scope->nodes.front() == node.name &&
            frame_graph.native_scopes->find(
                scope->id) != nullptr;
    }
    const bool anchor_has_work =
        node.kind != FramePlanNodeKind::anchor ||
        (node.name == "__anchor_sprite" &&
         sprite.scene != nullptr) ||
        native_scope_has_work;
    return GpuTimingNodeDescriptor{
        ordinal,
        std::string{framePlanNodeKindName(node.kind)},
        node.name,
        anchor_has_work};
}

std::vector<GpuTimingNodeDescriptor> plannedTimingNodes(
    const CompiledFrameGraphExecution &frame_graph,
    const SpriteRenderModules &sprite,
    std::span<const LogicalFrameNodeInvocation> schedule) {
    std::vector<GpuTimingNodeDescriptor> nodes;
    nodes.reserve(schedule.size());
    std::vector<std::uint8_t> included(
        frame_graph.nodes.size(), 0);
    for (const auto &invocation : schedule) {
        if (invocation.node_index >=
            frame_graph.nodes.size()) {
            throw std::runtime_error(
                "GPU timing schedule references an invalid frame-graph node");
        }
        if (included[invocation.node_index]) {
            continue;
        }
        included[invocation.node_index] = 1;
        nodes.push_back(
            plannedTimingNode(
                frame_graph, sprite,
                invocation.node_index));
    }
    return nodes;
}

std::vector<std::string> pairedSrgbStorageEdges(
    const CompiledRenderingPass &rendering_pass,
    const CompiledFrameGraphExecution &frame_graph,
    const RenderTargetContainer &rt_container) {
    std::vector<std::string> edges;
    for (size_t to = 0; to < frame_graph.nodes.size(); ++to) {
        const auto &node = frame_graph.nodes[to];
        for (const auto &barrier : node.incoming_barriers) {
            const auto target =
                boundRenderTarget(
                    frame_graph, barrier.resource);
            if (!isConcreteRenderTarget(target)) {
                continue;
            }
            const auto format = rt_container.getMetadata(target).format;
            if (format != vk::Format::eR8G8B8A8Srgb && format != vk::Format::eB8G8R8A8Srgb) {
                continue;
            }
            bool sampled = node.kind == FramePlanNodeKind::output_transform && barrier.resource == "display";
            if (node.kind == FramePlanNodeKind::render) {
                const auto &pass = rendering_pass.passes.at(node.index).definition;
                sampled = std::find_if(pass.input_targets.begin(), pass.input_targets.end(),
                                       [&](GlobalRenderTargetId id) { return id == target; }) !=
                          pass.input_targets.end();
            }
            if (sampled) {
                edges.push_back(frame_graph.nodes[barrier.from_node_index].name + ":" + barrier.resource +
                                "->" + node.name);
            }
        }
    }
    return edges;
}

void executeCompiledFrameGraphBarrier(
    const FrameRenderContext &render_ctx,
    const CompiledFrameGraphExecution &frame_graph,
    RenderFrameModules &modules,
    RenderTargetLayoutTracker &layout_tracker,
    std::size_t consumer_node_index,
    const CompiledFrameGraphBarrier &barrier,
    std::span<const std::uint8_t> completed_nodes) {
    (void)consumer_node_index;
    if (barrier.from_node_index >=
            completed_nodes.size() ||
        !completed_nodes[
            barrier.from_node_index]) {
        throw std::runtime_error(
            "Compiled frame graph barrier source was not executed "
            "before target");
    }
    const auto buffer_id =
        boundFrameGraphBuffer(
            frame_graph, barrier.resource);
    if (isValidFrameGraphBufferId(buffer_id)) {
        modules.compute_task_container
            .bufferReadAfterWriteBarrier(
                render_ctx.cmd_buf, buffer_id,
                barrier.from_kind,
                barrier.to_kind);
        return;
    }
    if (barrier.resource == "swapchain") {
        if (!render_ctx.color_image) {
            throw std::runtime_error(
                "Compiled frame graph swapchain barrier has no "
                "frame image");
        }
        vk::ImageMemoryBarrier image_barrier;
        image_barrier.srcAccessMask =
            vk::AccessFlagBits::
                eColorAttachmentRead |
            vk::AccessFlagBits::
                eColorAttachmentWrite;
        image_barrier.dstAccessMask =
            vk::AccessFlagBits::
                eColorAttachmentRead |
            vk::AccessFlagBits::
                eColorAttachmentWrite;
        image_barrier.oldLayout =
            vk::ImageLayout::
                eColorAttachmentOptimal;
        image_barrier.newLayout =
            vk::ImageLayout::
                eColorAttachmentOptimal;
        image_barrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        image_barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image =
            render_ctx.color_image;
        image_barrier.subresourceRange = {
            vk::ImageAspectFlagBits::eColor,
            0,
            1,
            render_ctx.color_base_array_layer,
            render_ctx.color_array_layers};
        render_ctx.cmd_buf.pipelineBarrier(
            vk::PipelineStageFlagBits::
                eColorAttachmentOutput,
            vk::PipelineStageFlagBits::
                eColorAttachmentOutput,
            {}, {}, {}, {image_barrier});
        return;
    }
    const auto target_id =
        boundRenderTarget(
            frame_graph, barrier.resource);
    if (!isConcreteRenderTarget(target_id)) {
        throw std::runtime_error(
            "Compiled frame graph barrier references an unknown "
            "resource: " +
            barrier.resource);
    }
    layout_tracker.memoryDependency(
        render_ctx.cmd_buf,
        modules.render_target_container,
        modules.vk_utils, target_id);
}

bool passReadsBarrierAsLocalAttachment(
    const CompiledPass &pass,
    const CompiledFrameGraphExecution &frame_graph,
    const CompiledFrameGraphBarrier &barrier) {
    const auto target =
        boundRenderTarget(
            frame_graph, barrier.resource);
    if (!isConcreteRenderTarget(target)) {
        return false;
    }
    for (std::size_t input_index = 0;
         input_index <
         pass.definition.input_targets.size();
         ++input_index) {
        if (pass.definition
                    .input_targets[input_index] ==
                target &&
            !pass.definition
                 .input_target_history.at(
                     input_index) &&
            passInputUsesLocalRead(
                pass, input_index)) {
            return true;
        }
    }
    return false;
}

bool isPhysicalScopeAttachment(
    const CompiledPassRenderingContract &rendering,
    const CompiledFrameGraphExecution &frame_graph,
    std::string_view resource) {
    const auto target =
        boundRenderTarget(
            frame_graph, std::string{resource});
    return isConcreteRenderTarget(target) &&
           (std::find_if(
                rendering.color_attachments.begin(),
                rendering.color_attachments.end(),
                [&](const auto &attachment) {
                    return attachment.target.value ==
                           target.value;
                }) !=
                rendering.color_attachments.end() ||
            target.value ==
                rendering.depth_attachment.target.value);
}

// Everything one planned frame-graph execution needs from the caller that
// planned it. The dependencies the caller has to build first
// (RenderPassExecutorDependencies, the per-invocation view binding hook) stay
// explicit parameters so that the request itself is pure input.
// The reference members all name objects the caller already owns for the whole
// execution; `record_scene_as` is held by value because callers pass a lambda,
// which would otherwise bind the reference to a materialized temporary.
struct FrameGraphExecutionRequest {
    const FrameRenderContext &render_ctx;
    const CompiledRenderingPass &rendering_pass;
    const CompiledFrameGraphExecution &frame_graph;
    RenderFrameModules &modules;
    RenderTargetLayoutTracker &layout_tracker;
    std::function<bool(const FrameRenderContext &,
                       std::string_view)> record_scene_as;
    const ValueGraph::InactiveScopes &inactive_graph_scopes;
    vk::Format frame_target_format;
    std::uint64_t logical_frame;
    std::string_view graph_variant;
    std::uint32_t view_index;
    std::uint32_t logical_view_count;
    std::span<const LogicalFrameNodeInvocation>
        authored_schedule{};
    nlohmann::json *trace_out = nullptr;
};

void executePlannedFrameGraph(
    const FrameGraphExecutionRequest &request,
    const RenderPassExecutorDependencies &pass_executor_dependencies,
    const std::function<void(const LogicalFrameNodeInvocation &)>
        &prepare_invocation) {
    // The schedule is replaced by a locally built fallback when the caller
    // supplied none, so it needs a mutable local of its own.
    std::span<const LogicalFrameNodeInvocation> schedule =
        request.authored_schedule;
    std::vector<LogicalFrameNodeInvocation>
        fallback_schedule;
    if (schedule.empty() &&
        request.frame_graph.target_plan == nullptr) {
        fallback_schedule.reserve(
            request.frame_graph.nodes.size());
        for (std::size_t node_index = 0;
             node_index < request.frame_graph.nodes.size();
             ++node_index) {
            const auto &node =
                request.frame_graph.nodes[node_index];
            const bool main_family =
                node.view_family ==
                mainRenderViewFamilyId;
            if (!main_family &&
                request.view_index != 0) {
                continue;
            }
            fallback_schedule.push_back(
                LogicalFrameNodeInvocation{
                    .node_index = node_index,
                    .execution =
                        main_family &&
                                request.logical_view_count > 1
                            ? VulkanScopeViewExecution::
                                  sequential
                            : VulkanScopeViewExecution::
                                  single_view,
                    .logical_view_count =
                        main_family
                            ? request.logical_view_count
                            : 1u,
                    .view_index = request.view_index,
                    .view_family =
                        node.view_family,
                });
        }
        schedule = fallback_schedule;
    }
    constexpr auto invalid_timing_query_index =
        std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t>
        timing_query_index_by_node(
            request.frame_graph.nodes.size(),
            invalid_timing_query_index);
    std::vector<std::size_t>
        timing_first_schedule_position(
            request.frame_graph.nodes.size(),
            invalid_timing_query_index);
    std::vector<std::size_t>
        timing_last_schedule_position(
            request.frame_graph.nodes.size(),
            invalid_timing_query_index);
    std::size_t next_timing_query_index = 0;
    for (std::size_t schedule_position = 0;
         schedule_position < schedule.size();
         ++schedule_position) {
        const auto node_index =
            schedule[schedule_position]
                .node_index;
        if (node_index >=
            request.frame_graph.nodes.size()) {
            throw std::runtime_error(
                "logical-frame view schedule references an invalid node");
        }
        if (timing_query_index_by_node[node_index] ==
            invalid_timing_query_index) {
            timing_query_index_by_node[node_index] =
                next_timing_query_index++;
            timing_first_schedule_position[node_index] =
                schedule_position;
        }
        timing_last_schedule_position[node_index] =
            schedule_position;
    }
    if (request.modules.render_timing != nullptr) {
        request.modules.render_timing->beginGpuRange(
            request.render_ctx.cmd_buf, request.render_ctx.in_flight_frame_index, request.view_index,
            GpuTimingRangeIdentity{request.logical_frame, std::string{request.graph_variant}, request.view_index},
            plannedTimingNodes(
                request.frame_graph, request.modules.sprite,
                schedule));
    }
    const auto paired_storage_edges = pairedSrgbStorageEdges(
        request.rendering_pass, request.frame_graph, request.modules.render_target_container);

    bool rendering_scope_active = false;
    std::size_t active_rendering_scope =
        std::numeric_limits<std::size_t>::max();
    bool native_scope_active = false;
    std::size_t active_native_scope =
        std::numeric_limits<std::size_t>::max();
    std::vector<std::uint8_t> completed_nodes(
        request.frame_graph.nodes.size(), 0);
    GlobalRenderTargetId latest_scene_color =
        noRenderTargetId();
    GlobalRenderTargetId latest_scene_depth =
        noRenderTargetId();

    for (std::size_t schedule_position = 0;
         schedule_position < schedule.size();
         ++schedule_position) {
        const auto &scheduled =
            schedule[schedule_position];
        if (scheduled.node_index >=
            request.frame_graph.nodes.size()) {
            throw std::runtime_error(
                "logical-frame view schedule references an invalid node");
        }
        const auto node_index =
            static_cast<std::uint32_t>(
                scheduled.node_index);
        const auto timing_query_index =
            timing_query_index_by_node[node_index];
        const bool begins_recorded_node =
            schedule_position ==
            timing_first_schedule_position[node_index];
        const bool ends_recorded_node =
            schedule_position ==
            timing_last_schedule_position[node_index];
        if (request.modules.render_timing != nullptr &&
            timing_query_index ==
                invalid_timing_query_index) {
            throw std::logic_error(
                "scheduled frame-graph node has no GPU timing query slot");
        }
        const auto &execution_node =
            request.frame_graph.nodes[node_index];
        if (scheduled.view_family !=
            execution_node.view_family) {
            throw std::runtime_error(
                "logical-frame schedule view family does not match node '" +
                execution_node.name + "'");
        }
        if (prepare_invocation) {
            prepare_invocation(scheduled);
        }
        if (node_index >= request.frame_graph.plan.nodes.size() ||
            request.frame_graph.plan.nodes[node_index].name != execution_node.name ||
            request.frame_graph.plan.nodes[node_index].kind != execution_node.kind) {
            throw std::runtime_error("Frame graph execution no longer matches frame plan");
        }
        const VulkanPhysicalScopePlan *physical_scope =
            nullptr;
        const PreparedVulkanNativeScopeExecutor
            *native_scope_executor = nullptr;
        if (request.frame_graph.target_plan != nullptr) {
            if (scheduled.scope_index >=
                request.frame_graph.target_plan->scopes.size()) {
                throw std::runtime_error(
                    "logical-frame schedule references an invalid "
                    "physical scope");
            }
            physical_scope =
                &request.frame_graph.target_plan
                     ->scopes[scheduled.scope_index];
            if (request.frame_graph.native_scopes != nullptr) {
                native_scope_executor =
                    request.frame_graph.native_scopes->find(
                        physical_scope->id);
            }
        }
        if (request.frame_graph.value_graph &&
            !request.frame_graph.value_graph->nodeActive(execution_node.name, request.inactive_graph_scopes)) {
            // Physical scopes have a single owner-lifetime condition, checked
            // at publication. Skip before beginning a scope: begin can clear
            // attachments even when no draw command is recorded.
            if (request.modules.render_timing != nullptr) {
                for (const auto range : {GpuTimingSubrange::barriers, GpuTimingSubrange::body}) {
                    if (begins_recorded_node)
                        request.modules.render_timing->writeNodeSubrangeStart(request.render_ctx.cmd_buf,
                            static_cast<std::uint32_t>(timing_query_index), range);
                    if (ends_recorded_node)
                        request.modules.render_timing->writeNodeSubrangeEnd(request.render_ctx.cmd_buf,
                            static_cast<std::uint32_t>(timing_query_index), range);
                }
            }
            if (request.trace_out != nullptr)
                request.trace_out->push_back({{"name", execution_node.name},
                    {"kind", framePlanNodeKindName(execution_node.kind)}, {"order", node_index},
                    {"recorded", false}, {"view_index", scheduled.view_index},
                    {"logical_view_count", scheduled.logical_view_count}});
            completed_nodes[node_index] = 1;
            continue;
        }
        const CompiledPass *render_pass =
            execution_node.kind ==
                    FramePlanNodeKind::render
                ? &request.rendering_pass.passes.at(
                      execution_node.index)
                : nullptr;
        const bool fused_rendering_scope =
            native_scope_executor == nullptr &&
            render_pass != nullptr &&
            render_pass->rendering
                .fused_rendering_scope;
        std::vector<std::uint8_t>
            starting_native_scope_nodes(
                request.frame_graph.nodes.size(),
                0);
        if (native_scope_executor != nullptr) {
            if (physical_scope == nullptr ||
                native_scope_executor->scopeId() !=
                    physical_scope->id) {
                throw std::logic_error(
                    "prepared NativeScope does not match the scheduled "
                    "physical scope");
            }
            if (scheduled.beginsScopeExecution()) {
                if (native_scope_active ||
                    rendering_scope_active) {
                    throw std::logic_error(
                        "physical NativeScope executions overlap");
                }
                if (scheduled.scope_node_count == 0 ||
                    scheduled.scope_node_count !=
                        physical_scope->nodes.size() ||
                    schedule_position +
                            scheduled.scope_node_count >
                        schedule.size()) {
                    throw std::runtime_error(
                        "NativeScope schedule is incomplete");
                }
                for (std::size_t offset = 0;
                     offset <
                     scheduled.scope_node_count;
                     ++offset) {
                    const auto &candidate =
                        schedule[
                            schedule_position + offset];
                    if (candidate.scope_index !=
                            scheduled.scope_index ||
                        candidate.scope_node_index !=
                            offset ||
                        candidate.scope_node_count !=
                            scheduled.scope_node_count ||
                        candidate.execution_index !=
                            scheduled.execution_index ||
                        candidate.view_index !=
                            scheduled.view_index) {
                        throw std::runtime_error(
                            "NativeScope schedule is not contiguous");
                    }
                    if (candidate.node_index >=
                        request.frame_graph.nodes.size()) {
                        throw std::runtime_error(
                            "NativeScope schedule references an invalid "
                            "node");
                    }
                    if (request.frame_graph
                            .nodes[candidate.node_index]
                            .name !=
                        physical_scope->nodes[offset]) {
                        throw std::runtime_error(
                            "NativeScope schedule node order differs from "
                            "its physical scope");
                    }
                    starting_native_scope_nodes[
                        candidate.node_index] = 1;
                }
            } else if (
                !native_scope_active ||
                active_native_scope !=
                    scheduled.scope_index) {
                throw std::runtime_error(
                    "NativeScope continuation has no active execution");
            }
        } else if (native_scope_active) {
            throw std::runtime_error(
                "NativeScope ended before its scheduled boundary");
        }
        std::vector<const CompiledPass *>
            starting_scope_passes;
        std::vector<std::uint8_t>
            starting_scope_nodes(
                request.frame_graph.nodes.size(),
                0);
        if (fused_rendering_scope &&
            scheduled.beginsScopeExecution()) {
            if (rendering_scope_active) {
                throw std::logic_error(
                    "fused physical rendering scopes overlap");
            }
            if (scheduled.scope_node_count < 2 ||
                schedule_position +
                        scheduled.scope_node_count >
                    schedule.size()) {
                throw std::runtime_error(
                    "fused physical rendering scope schedule is "
                    "incomplete");
            }
            starting_scope_passes.reserve(
                scheduled.scope_node_count);
            for (std::size_t offset = 0;
                 offset <
                 scheduled.scope_node_count;
                 ++offset) {
                const auto &candidate =
                    schedule[
                        schedule_position + offset];
                if (candidate.scope_index !=
                        scheduled.scope_index ||
                    candidate.scope_node_index !=
                        offset ||
                    candidate.scope_node_count !=
                        scheduled.scope_node_count ||
                    candidate.execution_index !=
                        scheduled.execution_index ||
                    candidate.view_index !=
                        scheduled.view_index) {
                    throw std::runtime_error(
                        "fused physical rendering scope schedule is not "
                        "contiguous");
                }
                const auto &candidate_node =
                    request.frame_graph.nodes.at(
                        candidate.node_index);
                if (candidate_node.kind !=
                    FramePlanNodeKind::render) {
                    throw std::runtime_error(
                        "fused physical rendering scope contains a "
                        "non-render node: " +
                        candidate_node.name);
                }
                const auto &candidate_pass =
                    request.rendering_pass.passes.at(
                        candidate_node.index);
                if (!candidate_pass.rendering
                         .fused_rendering_scope ||
                    candidate_pass.rendering
                            .scope_index !=
                        scheduled.scope_index) {
                    throw std::runtime_error(
                        "fused scheduled pass disagrees with "
                        "its physical scope: " +
                        candidate_node.name);
                }
                starting_scope_passes.push_back(
                    &candidate_pass);
                starting_scope_nodes[
                    candidate.node_index] = 1;
            }
        } else if (fused_rendering_scope) {
            if (!rendering_scope_active ||
                active_rendering_scope !=
                    scheduled.scope_index) {
                throw std::runtime_error(
                    "fused physical rendering scope continuation has no "
                    "active rendering instance");
            }
        } else if (rendering_scope_active) {
            throw std::runtime_error(
                "fused physical rendering scope ended before its "
                "scheduled boundary");
        }
        std::optional<VulkanNativeScopeRuntimeInvocation>
            native_runtime_invocation;

        std::string node_debug_name;
        if (request.modules.debug_utils.commandLabelsEnabled()) {
            node_debug_name = makeFrameGraphDebugLabel(FrameGraphDebugLabelIdentity{
                request.logical_frame, request.graph_variant,
                scheduled.view_index, node_index,
                framePlanNodeKindName(execution_node.kind), execution_node.name});
        }
        ScopedCommandDebugLabel node_label{request.modules.debug_utils, request.render_ctx.cmd_buf,
                                           node_debug_name.c_str()};
        if (request.modules.render_timing != nullptr &&
            begins_recorded_node) {
            request.modules.render_timing->writeNodeSubrangeStart(
                request.render_ctx.cmd_buf,
                static_cast<std::uint32_t>(
                    timing_query_index),
                GpuTimingSubrange::barriers);
        }
        {
            ScopedCommandDebugLabel barrier_label{
                request.modules.debug_utils,
                request.render_ctx.cmd_buf, "barriers"};
            if (native_scope_executor != nullptr &&
                scheduled.beginsScopeExecution()) {
                if (begins_recorded_node) {
                    for (std::size_t offset = 0;
                         offset <
                         scheduled.scope_node_count;
                         ++offset) {
                        const auto &consumer =
                            schedule[
                                schedule_position +
                                offset];
                        const auto &consumer_node =
                            request.frame_graph.nodes.at(
                                consumer.node_index);
                        for (const auto &barrier :
                             consumer_node
                                 .incoming_barriers) {
                            if (barrier.from_node_index >=
                                request.frame_graph.nodes.size()) {
                                throw std::runtime_error(
                                    "Compiled frame graph barrier "
                                    "source is out of range");
                            }
                            if (starting_native_scope_nodes[
                                    barrier
                                        .from_node_index]) {
                                // The provider owns all ordering and
                                // synchronization inside its exact scope.
                                continue;
                            }
                            executeCompiledFrameGraphBarrier(
                                request.render_ctx, request.frame_graph,
                                request.modules, request.layout_tracker,
                                consumer.node_index,
                                barrier,
                                completed_nodes);
                        }
                    }
                }
                native_runtime_invocation.emplace(
                    beginVulkanNativeScopeRuntimeInvocation(
                        *native_scope_executor,
                        request.render_ctx,
                        request.frame_target_format,
                        RenderPassViewInvocation{
                            scheduled
                                .logical_view_count,
                            scheduled.view_index},
                        request.modules
                            .render_target_container,
                        request.modules
                            .frame_graph_resources,
                        request.modules.vk_utils,
                        request.layout_tracker));
                native_scope_active = true;
                active_native_scope =
                    scheduled.scope_index;
            } else if (
                native_scope_executor != nullptr) {
                // One callback records the complete physical scope. Its
                // remaining logical nodes retain trace/dependency identity
                // but do not dispatch engine bodies.
            } else if (fused_rendering_scope &&
                scheduled.beginsScopeExecution()) {
                if (begins_recorded_node) {
                    std::vector<std::uint8_t>
                        prior_scope_nodes(
                            request.frame_graph.nodes.size(),
                            0);
                    for (std::size_t offset = 0;
                         offset <
                         scheduled.scope_node_count;
                         ++offset) {
                        const auto &consumer =
                            schedule[
                                schedule_position +
                                offset];
                        const auto &consumer_node =
                            request.frame_graph.nodes.at(
                                consumer.node_index);
                        const auto &consumer_pass =
                            *starting_scope_passes[
                                offset];
                        for (const auto &barrier :
                             consumer_node
                                 .incoming_barriers) {
                            if (barrier.from_node_index >=
                                request.frame_graph.nodes.size()) {
                                throw std::runtime_error(
                                    "Compiled frame graph barrier "
                                    "source is out of range");
                            }
                            const auto internal =
                                starting_scope_nodes[
                                    barrier.from_node_index] !=
                                0;
                            if (internal) {
                                if (!prior_scope_nodes[
                                        barrier
                                            .from_node_index]) {
                                    throw std::runtime_error(
                                        "fused physical rendering "
                                        "scope reverses an internal "
                                        "dependency");
                                }
                                if (passReadsBarrierAsLocalAttachment(
                                        consumer_pass,
                                        request.frame_graph,
                                        barrier) ||
                                    isPhysicalScopeAttachment(
                                        consumer_pass
                                            .rendering,
                                        request.frame_graph,
                                        barrier.resource)) {
                                    continue;
                                }
                                throw std::runtime_error(
                                    "fused physical rendering scope has "
                                    "an internal dependency that "
                                    "cannot execute inside dynamic "
                                    "rendering: " +
                                    barrier.resource);
                            }
                            executeCompiledFrameGraphBarrier(
                                request.render_ctx, request.frame_graph,
                                request.modules, request.layout_tracker,
                                consumer.node_index,
                                barrier,
                                completed_nodes);
                        }
                        prior_scope_nodes[
                            consumer.node_index] = 1;
                    }
                }
                request.modules.pass_executor
                    .beginRenderingScope(
                        request.render_ctx,
                        starting_scope_passes,
                        pass_executor_dependencies,
                        request.layout_tracker,
                        RenderPassViewInvocation{
                            scheduled
                                .logical_view_count,
                            scheduled.view_index});
                rendering_scope_active = true;
                active_rendering_scope =
                    scheduled.scope_index;
            } else if (fused_rendering_scope) {
                request.modules.pass_executor
                    .renderingScopeDependency(
                        request.render_ctx.cmd_buf);
            } else if (begins_recorded_node) {
                for (const auto &barrier :
                     execution_node.incoming_barriers) {
                    executeCompiledFrameGraphBarrier(
                        request.render_ctx, request.frame_graph, request.modules,
                        request.layout_tracker, node_index,
                        barrier, completed_nodes);
                }
            }
        }
        if (request.modules.render_timing != nullptr &&
            begins_recorded_node) {
            request.modules.render_timing->writeNodeSubrangeEnd(
                request.render_ctx.cmd_buf,
                static_cast<std::uint32_t>(
                    timing_query_index),
                GpuTimingSubrange::barriers);
        }
        ScopedCommandDebugLabel body_label{request.modules.debug_utils, request.render_ctx.cmd_buf, "body"};

        if (request.modules.render_timing != nullptr &&
            begins_recorded_node) {
            request.modules.render_timing->writeNodeSubrangeStart(
                request.render_ctx.cmd_buf,
                static_cast<std::uint32_t>(
                    timing_query_index),
                GpuTimingSubrange::body);
        }

        if (native_scope_executor != nullptr) {
            if (scheduled.beginsScopeExecution()) {
                if (!native_runtime_invocation) {
                    throw std::logic_error(
                        "NativeScope execution was not prepared");
                }
                recordVulkanNativeScopeRuntimeInvocation(
                    *native_runtime_invocation,
                    request.render_ctx,
                    request.modules.frame_resources);
                endVulkanNativeScopeRuntimeInvocation(
                    *native_runtime_invocation,
                    request.modules.render_target_container,
                    request.layout_tracker);
            } else if (native_runtime_invocation) {
                throw std::logic_error(
                    "NativeScope continuation prepared a second callback");
            }
            if (render_pass != nullptr) {
                if (!render_pass->definition
                         .output_color.empty()) {
                    latest_scene_color =
                        render_pass->definition
                            .output_color.front();
                }
                if (isConcreteRenderTarget(
                        render_pass->definition
                            .output_depth)) {
                    latest_scene_depth =
                        render_pass->definition
                            .output_depth;
                }
            }
            if (request.trace_out != nullptr) {
                request.trace_out->push_back(
                    {
                        {"name", execution_node.name},
                        {"kind", "native_scope"},
                        {"logical_kind",
                         framePlanNodeKindName(
                             execution_node.kind)},
                        {"order", node_index},
                        {"scope",
                         native_scope_executor
                             ->scopeId()},
                        {"implementation",
                         native_scope_executor
                             ->selection()
                             .implementation},
                        {"provider",
                         native_scope_executor
                             ->selection()
                             .provider},
                        {"recorded",
                         scheduled
                             .beginsScopeExecution()},
                    });
            }
            if (scheduled.endsScopeExecution()) {
                native_scope_active = false;
                active_native_scope =
                    std::numeric_limits<
                        std::size_t>::max();
            }
        } else if (execution_node.kind == FramePlanNodeKind::render) {
            const auto &pass = *render_pass;
            if (fused_rendering_scope) {
                request.modules.pass_executor
                    .executeRenderingScopePass(
                        request.render_ctx, pass,
                        pass_executor_dependencies,
                        RenderPassViewInvocation{
                            scheduled
                                .logical_view_count,
                            scheduled.view_index});
                if (scheduled
                        .endsScopeExecution()) {
                    request.modules.pass_executor
                        .endRenderingScope(
                            request.render_ctx.cmd_buf);
                    rendering_scope_active = false;
                    active_rendering_scope =
                        std::numeric_limits<
                            std::size_t>::max();
                }
            } else {
                request.modules.pass_executor.execute(
                    request.render_ctx, pass,
                    pass_executor_dependencies,
                    request.layout_tracker,
                    RenderPassViewInvocation{
                        scheduled.logical_view_count,
                        scheduled.view_index});
            }
            if (!pass.definition.output_color.empty()) {
                latest_scene_color =
                    pass.definition
                        .output_color.front();
            }
            if (isConcreteRenderTarget(
                    pass.definition.output_depth)) {
                latest_scene_depth =
                    pass.definition.output_depth;
            }
            if (request.trace_out != nullptr) {
                request.trace_out->push_back(renderNodeTrace(pass, node_index, request.modules.render_target_container,
                                                      request.layout_tracker));
            }
        } else if (execution_node.kind == FramePlanNodeKind::acceleration_structure) {
            const bool recorded = request.record_scene_as(request.render_ctx, execution_node.name);
            if (request.trace_out != nullptr) {
                request.trace_out->push_back({{"name", execution_node.name},
                    {"kind", "acceleration_structure"}, {"order", node_index},
                    {"recorded", recorded}});
            }
        } else if (execution_node.kind == FramePlanNodeKind::compute) {
            const auto &task = request.rendering_pass.compute_tasks.at(execution_node.index);
            request.modules.compute_task_container.transitionResourcesForDispatch(
                request.render_ctx.cmd_buf, task.task_id, request.modules.render_target_container, request.modules.vk_utils,
                request.layout_tracker);
            request.modules.compute_task_container.dispatch(
                    request.render_ctx.cmd_buf, task.task_id,
                    request.modules.frame_resources,
                    scheduled.view_index);
            if (request.trace_out != nullptr) {
                request.trace_out->push_back(computeNodeTrace(task, node_index, request.frame_graph,
                                                       request.modules.render_target_container,
                                                       request.layout_tracker));
                request.trace_out->back()["recorded"] = true;
            }
        } else if (execution_node.kind == FramePlanNodeKind::anchor) {
            if (request.trace_out != nullptr) {
                request.trace_out->push_back(anchorNodeTrace(execution_node.name, node_index));
            }
            if (execution_node.name == "__anchor_sprite" && request.modules.sprite.scene != nullptr) {
                const auto color_id =
                    latest_scene_color;
                const auto depth_id =
                    latest_scene_depth;
                if ((!isConcreteRenderTarget(color_id) && !isSwapchainRenderTarget(color_id)) ||
                    !isConcreteRenderTarget(depth_id))
                    throw std::runtime_error("sprite feature requires a scene color and depth attachment before sprite anchor");
                PassDefinition sprite_attachments;
                sprite_attachments.output_color = {color_id};
                sprite_attachments.output_depth = depth_id;
                transitionPassOutputsToAttachmentLayouts(request.render_ctx.cmd_buf, sprite_attachments,
                                                          request.modules.render_target_container, request.modules.vk_utils,
                                                          request.layout_tracker);
                const auto sequential_layer =
                    [&](std::uint32_t array_layers) {
                        return scheduled.execution ==
                                       VulkanScopeViewExecution::sequential &&
                                   array_layers >=
                                       scheduled.logical_view_count
                                   ? scheduled.view_index
                                   : 0u;
                    };
                const auto color_layer =
                    isConcreteRenderTarget(color_id)
                        ? sequential_layer(
                              request.modules.render_target_container
                                  .getMetadata(color_id)
                                  .array_layers)
                        : scheduled.view_index;
                const auto depth_layer =
                    sequential_layer(
                        request.modules.render_target_container
                            .getMetadata(depth_id)
                            .array_layers);
                const auto color_view =
                    isSwapchainRenderTarget(color_id)
                        ? (!request.render_ctx.color_layer_attachments.empty()
                               ? request.render_ctx.color_layer_attachments.at(
                                     color_layer)
                               : request.render_ctx.color_attachment)
                        : request.modules.render_target_container
                              .getAttachmentImageLayerView(
                                  color_id, color_layer);
                const auto &depth_meta = request.modules.render_target_container.getMetadata(depth_id);
                const auto color_format = isSwapchainRenderTarget(color_id)
                                              ? request.frame_target_format
                                              : request.modules.render_target_container.getMetadata(color_id).format;
                const auto extent = isSwapchainRenderTarget(color_id)
                                        ? request.render_ctx.extent
                                        : request.modules.render_target_container.getMetadata(color_id).extent;
                if (depth_meta.extent != extent)
                    throw std::runtime_error("sprite color and depth attachments must have matching extents");
                request.modules.sprite.renderer->render(
                    request.render_ctx.cmd_buf,
                    SpriteDrawRequest{
                        .color_view = color_view,
                        .depth_view =
                            request.modules.render_target_container
                                .getAttachmentImageLayerView(
                                    depth_id, depth_layer),
                        .extent = extent,
                        .color_format = color_format,
                        .depth_format = depth_meta.format,
                        .color_resolve_view =
                            isConcreteRenderTarget(color_id) &&
                                    request.modules.render_target_container
                                        .hasSeparateAttachment(color_id)
                                ? request.modules.render_target_container
                                      .getImageLayerView(
                                          color_id, color_layer)
                                : vk::ImageView{},
                        .depth_resolve_view =
                            request.modules.render_target_container
                                    .hasSeparateAttachment(depth_id)
                                ? request.modules.render_target_container
                                      .getImageLayerView(
                                          depth_id, depth_layer)
                                : vk::ImageView{},
                        .samples =
                            isConcreteRenderTarget(color_id)
                                ? request.modules.render_target_container
                                      .sampleCount(color_id)
                                : vk::SampleCountFlagBits::e1,
                        .color_resolve_mode =
                            isConcreteRenderTarget(color_id)
                                ? request.modules.render_target_container
                                      .resolveMode(color_id)
                                : vk::ResolveModeFlagBits::eNone,
                        .depth_resolve_mode =
                            request.modules.render_target_container
                                .resolveMode(depth_id),
                    },
                    SpriteRendererDependencies{*request.modules.sprite.scene, *request.modules.sprite.atlas,
                                               request.modules.frame_resources});
                if (request.trace_out != nullptr && isConcreteRenderTarget(color_id)) {
                    request.trace_out->back()["sprite_draw"] = {
                        {"color", request.modules.render_target_container.getMetadata(color_id).name},
                        {"depth", depth_meta.name}, {"depth_test", true}, {"depth_write", false},
                        {"blend", "straight_alpha"},
                        {"policy", request.modules.sprite.scene->statusJson()},
                    };
                }
            }
        } else if (execution_node.kind == FramePlanNodeKind::snapshot_copy) {
            const auto &planned_node = request.frame_graph.plan.nodes.at(node_index);
            if (planned_node.reads.size() != 1 || planned_node.writes.size() != 1) {
                throw std::runtime_error("snapshot copy node requires exactly one source and destination: " +
                                         execution_node.name);
            }
            const auto source_id = boundRenderTarget(
                request.frame_graph, planned_node.reads.front());
            const auto destination_id = boundRenderTarget(
                request.frame_graph, planned_node.writes.front());
            if (!isConcreteRenderTarget(source_id) || !isConcreteRenderTarget(destination_id)) {
                throw std::runtime_error("snapshot copy node references an unknown render target: " +
                                         execution_node.name);
            }
            const auto source = request.modules.render_target_container.getMetadata(source_id);
            const auto destination = request.modules.render_target_container.getMetadata(destination_id);
            if (source.format != destination.format || source.extent != destination.extent) {
                throw std::runtime_error("snapshot copy source/destination mismatch: " +
                                         execution_node.name);
            }
            request.layout_tracker.transition(request.render_ctx.cmd_buf, request.modules.render_target_container,
                                      request.modules.vk_utils, source_id,
                                      vk::ImageLayout::eTransferSrcOptimal);
            request.layout_tracker.transition(request.render_ctx.cmd_buf, request.modules.render_target_container,
                                      request.modules.vk_utils, destination_id,
                                      vk::ImageLayout::eTransferDstOptimal);
            vk::ImageCopy copy;
            const auto aspect = snapshotAspect(source.format);
            const auto source_is_layered =
                source.array_layers >=
                scheduled.logical_view_count;
            const auto destination_is_layered =
                destination.array_layers >=
                scheduled.logical_view_count;
            if (source_is_layered !=
                destination_is_layered) {
                throw std::runtime_error(
                    "snapshot copy source/destination view dimensions do "
                    "not match: " +
                    execution_node.name);
            }
            const auto copy_layer =
                scheduled.execution ==
                            VulkanScopeViewExecution::sequential &&
                        source_is_layered
                    ? scheduled.view_index
                    : 0u;
            copy.srcSubresource = {
                aspect, 0, copy_layer, 1};
            copy.dstSubresource = {
                aspect, 0, copy_layer, 1};
            copy.extent = vk::Extent3D{source.extent.width, source.extent.height, 1};
            request.render_ctx.cmd_buf.copyImage(request.modules.render_target_container.getImage(source_id).image.get(),
                                         vk::ImageLayout::eTransferSrcOptimal,
                                         request.modules.render_target_container.getImage(destination_id).image.get(),
                                         vk::ImageLayout::eTransferDstOptimal, copy);
            request.layout_tracker.transition(request.render_ctx.cmd_buf, request.modules.render_target_container,
                                      request.modules.vk_utils, destination_id,
                                      vk::ImageLayout::eShaderReadOnlyOptimal);
            if (request.trace_out != nullptr) {
                request.trace_out->push_back(snapshotCopyTrace(planned_node, source, destination));
            }
        } else if (execution_node.kind == FramePlanNodeKind::output_transform) {
            const auto display_id =
                boundRenderTarget(request.frame_graph, "display");
            if (!isConcreteRenderTarget(display_id)) {
                throw std::runtime_error("output_transform requires the canonical display target");
            }
            const auto display = request.modules.render_target_container.getMetadata(display_id);
            const auto source_old_layout = request.layout_tracker.currentLayout(display_id, false,
                                                                         &request.modules.render_target_container);
            const auto output_pass = std::find_if(
                request.rendering_pass.passes.begin(), request.rendering_pass.passes.end(),
                [](const CompiledPass &pass) { return pass.definition.name == "output_transform"; });
            if (output_pass == request.rendering_pass.passes.end()) {
                throw std::runtime_error("output_transform fullscreen pass was not compiled");
            }
            request.modules.pass_executor.execute(request.render_ctx, *output_pass,
                                          pass_executor_dependencies, request.layout_tracker,
                                          RenderPassViewInvocation{
                                              scheduled.logical_view_count,
                                              scheduled.view_index});
            if (request.trace_out != nullptr) {
                request.trace_out->push_back(outputTransformTrace(node_index, source_old_layout,
                                                           request.render_ctx.required_layout, display,
                                                            request.frame_target_format,
                                                           paired_storage_edges));
            }
        } else {
            throw std::runtime_error("Unsupported frame graph execution node: " + execution_node.name);
        }

        if (request.trace_out != nullptr &&
            !request.trace_out->empty()) {
            auto &trace = request.trace_out->back();
            trace["view_execution"] =
                vulkanScopeViewExecutionName(
                    scheduled.execution);
            trace["view_index"] =
                scheduled.view_index;
            trace["logical_view_count"] =
                scheduled.logical_view_count;
            if (scheduled.view_family !=
                mainRenderViewFamilyId) {
                trace["view_family"] =
                    scheduled.view_family;
            }
        }

        if (request.modules.render_timing != nullptr &&
            ends_recorded_node) {
            request.modules.render_timing->writeNodeSubrangeEnd(
                request.render_ctx.cmd_buf,
                static_cast<std::uint32_t>(
                    timing_query_index),
                GpuTimingSubrange::body);
        }
        // Completion is local to this command-recording call. A sequential
        // XR schedule may be filtered to one view, so waiting for the final
        // logical-view invocation would leave valid same-view dependencies
        // falsely incomplete.
        completed_nodes[node_index] = 1;
    }

    if (rendering_scope_active) {
        throw std::runtime_error(
            "fused physical rendering scope remained open after the "
            "logical frame schedule");
    }
    if (native_scope_active) {
        throw std::runtime_error(
            "NativeScope remained active after the logical frame "
            "schedule");
    }
    if (request.modules.render_timing != nullptr) request.modules.render_timing->endGpuRange();
}

void executeRenderingPasses(
    const FrameGraphExecutionRequest &request,
    std::span<const RenderViewFamilyExecutionState> view_family_states,
    bool per_view_sort) {
    const auto &main_state =
        requireRenderViewFamilyExecutionState(
            view_family_states,
            mainRenderViewFamilyId);
    if (main_state.family == nullptr ||
        main_state.snapshots.empty() ||
        main_state.family->views.size() !=
            request.logical_view_count ||
        request.view_index >=
            main_state.snapshots.size()) {
        throw std::runtime_error(
            "render execution view state is incomplete");
    }
    const auto &views =
        main_state.family->views;
    const auto &snapshots =
        main_state.snapshots;
    const auto &default_snapshot =
        snapshots[request.view_index];
    MaterialRendererDependencies material_renderer_dependencies{
        request.modules.instance_container,
        request.modules.vert_buf_container,
        request.modules.material_container,
        request.modules.frame_resources,
        request.modules.frame_graph_resources,
        request.modules.light_container,
        request.modules.camera,
        default_snapshot.view_projection_jittered,
        views[request.view_index].first_person_view,
        per_view_sort ? request.view_index : 0u};
    const FullscreenPassRendererDependencies fullscreen_pass_renderer_dependencies{request.modules.fullscreen_pass_container,
                                                                                  request.modules.frame_resources};
    std::optional<UiRendererDependencies> ui_renderer_dependencies;
    if (request.modules.ui_renderer != nullptr && request.modules.ui_container != nullptr && request.modules.ui_module != nullptr)
        ui_renderer_dependencies.emplace(*request.modules.ui_container, *request.modules.ui_module, request.modules.frame_resources);
    std::optional<glm::vec3> gizmo_world_position;
    if (request.modules.gizmo != nullptr) {
        if (const auto gizmo_request = request.modules.gizmo->displayRequest()) {
            const auto target = resolveGizmoTargetTransform(
                gizmo_request->selection, GET_MODULE(ProjectBasicConfig),
                GET_MODULE(SceneLoader), GET_MODULE(ECSCore));
            if (target) gizmo_world_position = target->position;
        }
    }
    const auto *window = FastModuleContainer::tryGet<Window>();
    const auto output_content_scale =
        window != nullptr
            ? gizmoContentScale(request.render_ctx.extent, window->logicalExtent())
            : 1.0f;
    RenderPassDispatchDependencies pass_dispatch_dependencies{
        request.modules.material_renderer,
        material_renderer_dependencies,
        request.modules.fullscreen_pass_renderer,
        fullscreen_pass_renderer_dependencies,
        request.modules.shadow_depth_pass_container,
        request.modules.velocity_pass_container,
        request.modules.ui_renderer,
        ui_renderer_dependencies ? &*ui_renderer_dependencies : nullptr,
        request.modules.debug_draw,
        request.modules.gizmo,
        request.modules.debug_text,
#if PELICAN_WITH_IMGUI
        request.modules.imgui_system,
#endif
        request.modules.frame_resources,
        request.modules.camera,
        default_snapshot.view_projection_jittered,
        default_snapshot.view_projection_non_jittered,
        gizmo_world_position,
        output_content_scale,
        request.frame_target_format};
    const RenderPassExecutorDependencies pass_executor_dependencies{request.modules.render_target_container, request.modules.vk_utils,
                                                                    pass_dispatch_dependencies};

    std::function<void(
        const LogicalFrameNodeInvocation &)>
        prepare_invocation;
    if (!request.authored_schedule.empty()) {
        prepare_invocation =
            [&](const LogicalFrameNodeInvocation
                    &invocation) {
                const auto state_view =
                    invocation.view_index;
                const auto &family_state =
                    requireRenderViewFamilyExecutionState(
                        view_family_states,
                        invocation
                            .view_family);
                if (state_view >=
                        family_state
                            .snapshots.size() ||
                    family_state.family ==
                        nullptr ||
                    state_view >=
                        family_state.family
                            ->views.size()) {
                    throw std::runtime_error(
                        "logical-frame execution selected an "
                        "unavailable family view state");
                }
                const auto &snapshot =
                    family_state
                        .snapshots[state_view];
                const auto &family_view =
                    family_state.family
                        ->views[state_view];
                material_renderer_dependencies
                    .view_projection =
                    snapshot
                        .view_projection_jittered;
                material_renderer_dependencies
                    .first_person_view =
                    family_view
                        .first_person_view;
                material_renderer_dependencies
                    .draw_sort_view_index =
                    per_view_sort &&
                            invocation
                                    .view_family ==
                                mainRenderViewFamilyId
                        ? state_view
                        : 0u;
                material_renderer_dependencies
                    .view_family =
                    invocation.view_family;
                material_renderer_dependencies
                    .secondary_view_index =
                    invocation.view_family !=
                            mainRenderViewFamilyId
                        ? std::optional{
                              state_view}
                        : std::nullopt;
                pass_dispatch_dependencies
                    .view_projection =
                    snapshot
                        .view_projection_jittered;
                pass_dispatch_dependencies
                    .view_projection_non_jittered =
                    snapshot
                        .view_projection_non_jittered;
                if (invocation.execution ==
                    VulkanScopeViewExecution::
                        multiview) {
                    if (invocation
                            .view_family !=
                        mainRenderViewFamilyId) {
                        throw std::runtime_error(
                            "secondary multiview family execution is not "
                            "implemented");
                    }
                    request.modules.frame_resources
                        .selectMultiview(
                            request.render_ctx
                                .in_flight_frame_index);
                } else {
                    request.modules.frame_resources
                        .selectSequentialView(
                            request.render_ctx
                                .in_flight_frame_index,
                            family_state
                                    .sequential_slot_base +
                                state_view);
                }
            };
    }

    executePlannedFrameGraph(request, pass_executor_dependencies,
                             prepare_invocation);
}

void rebindFullscreenInputs(RenderFrameModules &modules) {
    RenderTargetImageViewResolver rt_views{modules.render_target_container};
    const auto generation =
        modules.frame_graph_runtime.renderingSnapshot();
    if (generation == nullptr) {
        throw std::runtime_error(
            "Fullscreen input rebind requires a published render pipeline");
    }
    for (const auto pass_id :
         generation->rendering_pass_ids) {
        const auto *program = generation->find(pass_id);
        if (program == nullptr) {
            throw std::logic_error(
                "Published render pipeline pass table is inconsistent");
        }
        const auto &compiled_pass =
            program->rendering_pass;
        for (const auto &pass : compiled_pass.passes) {
            if ((pass.definition.isFullscreen() ||
                 pass.definition.isGenericRaster()) &&
                (!pass.definition.input_targets.empty() || !pass.definition.input_buffers.empty())) {
                modules.fullscreen_pass_container
                    .rebindInputResources(
                        pass.pass_id, rt_views,
                        modules.frame_graph_resources);
            }
        }
    }
    modules.material_container.rebindScreenInputs(rt_views);
    modules.compute_task_container.rebindRenderTargets(
        modules.render_target_container);
}

bool consumeShaderReloadPublication(ShaderHotReloadModules &modules) {
    return modules.reload_service != nullptr &&
           modules.reload_service
                   ->applyRuntimeBoundary(watch::RuntimeReloadBoundary::render_start)
                   .committed != 0;
}

class OutputRelowerRequired final
    : public std::runtime_error {
  public:
    OutputRelowerRequired()
        : std::runtime_error{
              "window output compile facts require render-pipeline re-lowering"} {}
};

class OutputTemporarilyUnavailable final {};

bool windowOutputFactsChanged(
    const ILogicalFrameTarget &target,
    const std::shared_ptr<
        const RendererRuntimeGeneration>
        &runtime_generation) {
    const auto output_facts =
        target.outputCompileFacts();
    if (!output_facts ||
        output_facts->target_kind !=
            OutputTargetKind::window) {
        return false;
    }
    const auto published_output =
        runtime_generation != nullptr
            ? runtime_generation->window_output
            : nullptr;
    return published_output == nullptr ||
           published_output->compile_fingerprint !=
               outputCompileFactsFingerprint(
                   *output_facts) ||
           published_output->compile_facts !=
               *output_facts;
}

bool handleFrameTargetResize(RenderFrameModules &modules,
                             RenderTargetLayoutTracker &layout_tracker,
                             ILogicalFrameTarget &target,
                             vk::Extent2D extent,
                             bool logical_target_extent_changed,
                             const std::shared_ptr<
                                 const RendererRuntimeGeneration>
                                 &runtime_generation) {
    // Window output resources, descriptors, pipelines, and compile facts are
    // one renderer generation. Even an extent-only change is re-lowered
    // through the normal all-or-nothing configuration transaction instead of
    // mutating live targets beneath the frame's immutable generation.
    if (windowOutputFactsChanged(
            target, runtime_generation)) {
        throw OutputRelowerRequired{};
    }
    if (!logical_target_extent_changed) {
        return false;
    }

    // Non-window logical targets (currently OpenXR and test targets) do not
    // participate in WSI compile facts. Their extent-only resources still use
    // an immutable candidate and a single container publication.
    auto prepared =
        modules.render_target_container
            .prepareForExtent(extent);
    modules.render_target_container
        .publishPreparedExtent(
            std::move(prepared));
    rebindFullscreenInputs(modules);
    layout_tracker.reset();
    return true;
}

#if PELICAN_WITH_OPENXR
void recordXrMirrorIntermediate(const FrameRenderContext &render_ctx,
                                const CompiledFrameGraphExecution &frame_graph,
                                RenderFrameModules &modules,
                                RenderTargetLayoutTracker &layout_tracker,
                                nlohmann::json *node_trace,
                                std::uint64_t logical_frame) {
    const auto source_id =
        boundRenderTarget(frame_graph, "display");
    const auto destination_id = boundRenderTarget(
        frame_graph,
        std::string{
            OpenXr::xr_mirror_intermediate_name});
    if (!isConcreteRenderTarget(source_id) || !isConcreteRenderTarget(destination_id)) {
        throw std::runtime_error(
            "OpenXR mirror requires engine-owned display and left-eye intermediates");
    }
    const auto source = modules.render_target_container.getMetadata(source_id);
    const auto destination = modules.render_target_container.getMetadata(destination_id);
    if (source.format != destination.format || source.extent != destination.extent) {
        throw std::runtime_error(
            "OpenXR mirror intermediate must match the engine-owned display source "
            "(source=" + vk::to_string(source.format) + " " +
            std::to_string(source.extent.width) + "x" +
            std::to_string(source.extent.height) + ", destination=" +
            vk::to_string(destination.format) + " " +
            std::to_string(destination.extent.width) + "x" +
            std::to_string(destination.extent.height) + ")");
    }
    constexpr std::uint32_t mirror_view_index = 2;
    constexpr std::uint32_t mirror_intermediate_range_slot = 2;
    if (modules.render_timing != nullptr) {
        modules.render_timing->beginGpuRange(
            render_ctx.cmd_buf, render_ctx.in_flight_frame_index,
            mirror_intermediate_range_slot,
            GpuTimingRangeIdentity{logical_frame, "xr", mirror_view_index},
            {GpuTimingNodeDescriptor{0, "mirror", "intermediate_copy", true}});
    }
    std::string node_debug_name;
    if (modules.debug_utils.commandLabelsEnabled()) {
        node_debug_name = makeFrameGraphDebugLabel(FrameGraphDebugLabelIdentity{
            logical_frame, "xr", mirror_view_index, 0, "mirror", "intermediate_copy"});
    }
    ScopedCommandDebugLabel node_label{modules.debug_utils, render_ctx.cmd_buf,
                                       node_debug_name.c_str()};
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeStart(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::barriers);
    }
    {
        ScopedCommandDebugLabel barrier_label{modules.debug_utils, render_ctx.cmd_buf,
                                               "barriers"};
        layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                                  modules.vk_utils, source_id,
                                  vk::ImageLayout::eTransferSrcOptimal);
        layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                                  modules.vk_utils, destination_id,
                                  vk::ImageLayout::eTransferDstOptimal);
    }
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeEnd(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::barriers);
    }
    ScopedCommandDebugLabel body_label{modules.debug_utils, render_ctx.cmd_buf, "body"};
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeStart(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::body);
    }
    vk::ImageCopy copy;
    copy.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.extent = vk::Extent3D{source.extent.width, source.extent.height, 1};
    render_ctx.cmd_buf.copyImage(
        modules.render_target_container.getImage(source_id).image.get(),
        vk::ImageLayout::eTransferSrcOptimal,
        modules.render_target_container.getImage(destination_id).image.get(),
        vk::ImageLayout::eTransferDstOptimal, copy);
    layout_tracker.transition(render_ctx.cmd_buf, modules.render_target_container,
                              modules.vk_utils, destination_id,
                              vk::ImageLayout::eShaderReadOnlyOptimal);
    if (modules.render_timing != nullptr) {
        modules.render_timing->writeNodeSubrangeEnd(
            render_ctx.cmd_buf, 0, GpuTimingSubrange::body);
        modules.render_timing->endGpuRange();
    }
    if (node_trace != nullptr) {
        node_trace->push_back({
            {"name", "xr_mirror_left_intermediate"},
            {"kind", "engine_owned_copy"},
            {"source", "display"},
            {"destination", OpenXr::xr_mirror_intermediate_name},
            {"source_is_xr_image", false},
        });
    }
}
#endif

std::optional<ProjectionJitterSettings>
projectionJitterSettingsFor(
    const CompiledFrameGraphExecution &frame_graph) {
    if (!frame_graph.render_pipeline ||
        !frame_graph.render_pipeline->projection_jitter) {
        return std::nullopt;
    }
    const auto &jitter =
        *frame_graph.render_pipeline->projection_jitter;
    ProjectionJitterSettings settings{
        jitter.provider,
        jitter.pattern,
        jitter.phases,
    };
    settings.offsets_px.reserve(jitter.offsets_px.size());
    for (const auto &offset : jitter.offsets_px) {
        settings.offsets_px.emplace_back(
            static_cast<float>(compiledRenderNumericValueAsDouble(offset[0])),
            static_cast<float>(compiledRenderNumericValueAsDouble(offset[1])));
    }
    return settings;
}

class FlatLogicalFrameTarget final : public ILogicalFrameTarget {
    RenderTarget &target;
    std::optional<FrameTargetFrame> frame;
    bool view_begun = false;
    bool submitted = false;

  public:
    explicit FlatLogicalFrameTarget(RenderTarget &target) : target{target} {}

    void beginLogicalFrame(
        std::uint32_t view_count,
        LogicalFrameRuntime runtime) override {
        if (view_count != 1) {
            throw std::runtime_error("flat IFrameTarget requires exactly one logical-frame view");
        }
        if (frame) {
            throw std::logic_error(
                "flat IFrameTarget began a logical frame while another token was active");
        }
        auto begun = target.beginFrame(
            std::move(runtime.renderer_generation),
            std::move(runtime.submission_lease),
            FrameBeginMode::blocking);
        if (begun.disposition ==
            FrameBeginDisposition::unavailable) {
            throw OutputTemporarilyUnavailable{};
        }
        if (begun.disposition !=
                FrameBeginDisposition::ready ||
            !begun.frame) {
            throw std::runtime_error(
                "flat IFrameTarget output entered a terminal state");
        }
        frame.emplace(std::move(*begun.frame));
        view_begun = false;
    }

    FrameRenderContext beginView(std::uint32_t view_index) override {
        if (view_index != 0 || view_begun) {
            throw std::runtime_error("flat IFrameTarget view was begun out of order");
        }
        view_begun = true;
        return frame->context();
    }

    void endView(
        std::uint32_t view_index,
        GpuSubmissionLease) override {
        if (view_index != 0 || !view_begun) {
            throw std::runtime_error("flat IFrameTarget view was ended out of order");
        }
    }

    void submitLogicalFrame(GpuSubmissionLease lease) override {
        if (!view_begun) {
            throw std::runtime_error("flat IFrameTarget logical frame ended without a view");
        }
        auto token = std::move(*frame);
        frame.reset();
        (void)lease;
        target.submitFrame(std::move(token));
        view_begun = false;
        submitted = true;
    }

    void publishLogicalFrame() override {
        if (!submitted) {
            throw std::logic_error(
                "flat IFrameTarget publish called before submit");
        }
        (void)target.publishFrame();
        submitted = false;
    }

    void endLogicalFrame(GpuSubmissionLease lease) override {
        submitLogicalFrame(std::move(lease));
        publishLogicalFrame();
    }

    void abortLogicalFrame() noexcept override {
        if (submitted) {
            target.recoverSubmittedFrame();
            submitted = false;
        }
        if (!frame) return;
        auto token = std::move(*frame);
        frame.reset();
        target.abandon(std::move(token));
        view_begun = false;
    }

    vk::Format colorFormat(std::uint32_t view_index) const override {
        if (view_index != 0) {
            throw std::runtime_error("flat IFrameTarget color format view is out of range");
        }
        return target.getSwapchainFormat();
    }

    std::optional<OutputCompileFacts>
    outputCompileFacts() const override {
        return target.caps().compile_facts;
    }
};

class LogicalFrameAbortGuard {
    ILogicalFrameTarget *target;

  public:
    explicit LogicalFrameAbortGuard(ILogicalFrameTarget &target)
        : target{&target} {}
    LogicalFrameAbortGuard(const LogicalFrameAbortGuard &) = delete;
    LogicalFrameAbortGuard &operator=(const LogicalFrameAbortGuard &) = delete;
    ~LogicalFrameAbortGuard() {
        if (target != nullptr) target->abortLogicalFrame();
    }

    void complete() noexcept { target = nullptr; }
};

class StagedRuntimeRollbackGuard {
    FrameGraphRuntimeContainer *runtime_ = nullptr;
    std::uint64_t generation_ = 0;

  public:
    StagedRuntimeRollbackGuard(
        FrameGraphRuntimeContainer &runtime,
        std::uint64_t generation) noexcept
        : runtime_{generation != 0 ? &runtime : nullptr},
          generation_{generation} {}
    StagedRuntimeRollbackGuard(
        const StagedRuntimeRollbackGuard &) = delete;
    StagedRuntimeRollbackGuard &operator=(
        const StagedRuntimeRollbackGuard &) = delete;
    ~StagedRuntimeRollbackGuard() {
        if (runtime_ != nullptr) {
            (void)runtime_->rollbackStagedGeneration(
                generation_);
        }
    }
    void complete() noexcept { runtime_ = nullptr; }
};

struct LogicalFrameSubmissionResources {
    std::shared_ptr<const RendererRuntimeGeneration> runtime_generation;
    std::vector<RayQueryGeometryInstanceSnapshot> deformed_geometry;
    // The descriptor bundle contains the raw TLAS handle. Keep the immutable
    // AS State that owns that handle in the exact same fence-backed lease.
    std::shared_ptr<const void> rt_state;
    std::vector<std::shared_ptr<const RtDescriptorBundle>> rt_bundles;
};

} // namespace

Renderer::Renderer() {
    const auto variants =
        loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays();
    flat_rendering_pass_id = variants.flat;
    xr_rendering_pass_id = variants.xr;
    xr_excluded_features = variants.xr_excluded_features;
    preview_graph_program = variants.preview;
    current_rendering_pass_id = flat_rendering_pass_id;
    internal_render_extent = GET_MODULE(RenderTarget).getExtent();
    installRenderPipelineReloadParticipant();
}

Renderer::Renderer(RendererFramePlanCpuSeam,
                   RenderingPassId rendering_pass_id)
    : current_rendering_pass_id{rendering_pass_id},
      flat_rendering_pass_id{rendering_pass_id} {}

Renderer::~Renderer() {
    if (render_pipeline_reload_state != nullptr) {
        const auto staged_generation =
            render_pipeline_reload_state->staged_generation;
        if (staged_generation != 0) {
            if (auto *runtime =
                    FastModuleContainer::tryGet<
                        FrameGraphRuntimeContainer>()) {
                (void)runtime->rollbackStagedGeneration(
                    staged_generation);
            }
        }
        if (auto *reload_service =
                FastModuleContainer::tryGet<
                    watch::ReloadService>()) {
            (void)reload_service->unregisterParticipant(
                watch::renderPipelineReloadParticipantName);
        }
    }
}

void Renderer::installRenderPipelineReloadParticipant() {
    auto state =
        std::make_unique<RenderPipelineReloadState>();
    state->source_reference =
        GET_MODULE(ProjectBasicConfig)
            .renderingConfigReference();
    const auto generation =
        GET_MODULE(FrameGraphRuntimeContainer)
            .renderingSnapshot();
    if (generation != nullptr) {
        state->watched_sources =
            renderPipelineWatchSources(
                state->source_reference, *generation,
                flat_rendering_pass_id);
        state->last_generation =
            generation->generation;
    }
    render_pipeline_reload_state =
        std::move(state);

    auto &reload_service =
        GET_MODULE(watch::ReloadService);
    reload_service.registerParticipant(
        watch::ReloadParticipant{
            .name = std::string{
                watch::renderPipelineReloadParticipantName},
            .file = watch::FileReloadRole{
                .claims =
                    [this](
                        const watch::ReloadRequest &request) {
                        return render_pipeline_reload_state !=
                                   nullptr &&
                               render_pipeline_reload_state
                                   ->watched_sources
                                   .contains(request.key);
                    },
                // The graph owns the coordinated transaction: shader, material
                // and model changes seen in the same watcher frame publish one
                // generation with it instead of one generation each.
                .apply = watch::CompanionBatchReloadApply{
                    .companion_participants = {
                        std::string{watch::modelReloadParticipantName},
                        std::string{
                            watch::shaderReloadParticipantName},
                        std::string{
                            watch::materialReloadParticipantName},
                    },
                    .companion_claims =
                        [](std::string_view participant,
                           const watch::ReloadRequest &request) {
                            if (participant == watch::modelReloadParticipantName) {
                                return true; // The model participant already claimed this request.
                            }
                            if (participant ==
                                watch::
                                    shaderReloadParticipantName) {
                                return true;
                            }
                            if (participant !=
                                watch::
                                    materialReloadParticipantName) {
                                return false;
                            }
                            const auto *materials =
                                FastModuleContainer::tryGet<
                                    MaterialContainer>();
                            return materials != nullptr &&
                                   materials
                                       ->handlesMaterialValuesReload(
                                           request.key);
                        },
                    .apply =
                        [this](
                            std::span<
                                const watch::ReloadRequest>,
                            std::span<
                                const watch::ReloadRequest>
                                companion_requests) {
                            return reloadRenderPipelineFromDisk(
                                       companion_requests)
                                .error.empty();
                        },
                },
            },
            .runtime =
                watch::RuntimeReloadParticipant{
                    .boundary =
                        watch::RuntimeReloadBoundary::
                            frame_start,
                    .apply =
                        [this](
                            watch::RuntimeReloadTrigger
                                trigger) {
                            if (trigger ==
                                watch::
                                    RuntimeReloadTrigger::
                                        poll) {
                                return watch::
                                    RuntimeReloadResult{};
                            }
                            auto applied =
                                reloadRenderPipelineFromDisk();
                            return watch::
                                RuntimeReloadResult{
                                    .attempted = true,
                                    .committed =
                                        applied.committed,
                                    .staged = applied.staged,
                                    .staged_generation =
                                        applied.staged_generation,
                                    .error = std::move(
                                        applied.error),
                                };
                        },
                    .describe =
                        [this](nlohmann::json &output) {
                            if (render_pipeline_reload_state ==
                                nullptr) {
                                output = {
                                    {"available", false}};
                                return;
                            }
                            const auto &state =
                                *render_pipeline_reload_state;
                            auto sources =
                                nlohmann::json::array();
                            for (const auto &source :
                                 state.watched_sources) {
                                sources.push_back(
                                    watch::assetKeyString(
                                        source));
                            }
                            output = {
                                {"available", true},
                                {"source", "file_watcher"},
                                {"root",
                                 state.source_reference},
                                {"watched_sources",
                                 std::move(sources)},
                                {"attempted",
                                 state.attempted},
                                {"applied", state.applied},
                                {"failed", state.failed},
                                {"generation",
                                 state.last_generation},
                                {"domain_error",
                                 state.last_error.empty()
                                     ? nlohmann::json(
                                           nullptr)
                                     : nlohmann::json(
                                           state.last_error)},
                            };
                        },
                },
            .authored = watch::AuthoredCandidateReloadRole{
                .apply =
                [this](
                    RenderConfigCandidateDocumentSet candidate,
                    std::function<void()> source_commit,
                    std::function<void(
                        watch::AuthoredRuntimeReloadResult)>
                        completion) {
                    const auto applied =
                        applyRenderPipelineAuthoringCandidate(
                            std::move(candidate),
                            std::move(source_commit),
                            [completion =
                                 std::move(completion)](
                                RenderPipelineAuthoringApplyResult
                                    completed) mutable {
                                if (!completion) return;
                                completion(
                                    watch::AuthoredRuntimeReloadResult{
                                        .attempted = true,
                                        .committed =
                                            completed.committed,
                                        .staged = completed.staged,
                                        .staged_generation =
                                            completed.staged_generation,
                                        .published_generation =
                                            completed.published_generation,
                                        .error =
                                            std::move(completed.error),
                                        .post_commit_error =
                                            std::move(completed.post_commit_error),
                                    });
                            });
                    return watch::AuthoredRuntimeReloadResult{
                        .attempted = true,
                        .committed = applied.committed,
                        .staged = applied.staged,
                        .staged_generation =
                            applied.staged_generation,
                        .published_generation =
                            applied.published_generation,
                        .error = applied.error,
                        .post_commit_error =
                            applied.post_commit_error,
                    };
                },
            },
        });
}

void Renderer::synchronizeGraphGpuOwnership() {
    auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
    if (runtime.stagedGeneration() != 0) return;
    const auto generation = runtime.snapshot();
    const auto *program = generation ? generation->find(flat_rendering_pass_id) : nullptr;
    if (!program || !program->frame_graph.value_graph) return;
    const auto owners = GET_MODULE(SceneLoader).graphOwnerObjects(*program->frame_graph.value_graph);
    ValueGraph::InactiveScopes inactive;
    for (const auto &[scope, owner] : program->frame_graph.value_graph->instanceOwners())
        if (!owners.contains(scope)) inactive.insert(scope);
    if (owners == program->frame_graph.graph_owner_objects &&
        inactive == program->frame_graph.inactive_graph_scopes) return;
    // A failed candidate is retried when ownership or the authored generation
    // changes, not on every frame with the same invalid configuration.
    if (graph_ownership_attempt_generation == generation->generation &&
        graph_ownership_attempt_objects == owners) return;
    graph_ownership_attempt_generation = generation->generation;
    graph_ownership_attempt_objects = owners;

    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &paths = GET_MODULE(PathResolver);
    const auto reference = config.renderingConfigReference();
    const auto normalized = paths.normalizedReference(reference);
    const auto bytes = config.renderingConfigJson();
    std::vector<RenderConfigCandidateDocument> documents;
    std::set<std::string> included_documents;
    const auto append_document = [&](const std::string &ref, const std::string &key, const std::string &text) {
        if (!included_documents.insert(key).second) return;
        const auto resolved = paths.resolveProjectRef(ref);
        std::filesystem::path path;
        if (const auto *file = std::get_if<std::filesystem::path>(&resolved)) path = *file;
        else if (const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved)) path = fragment->path;
        else return; // engine:// JSON is immutable within the running engine.
        documents.push_back({.reference = ref, .normalized_reference = key, .path = std::move(path),
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, renderConfigSourceDigest(text)}, .bytes = text});
    };
    append_document(reference, normalized, bytes);
    if (program->frame_graph.source_documents)
        for (const auto &[ref, text] : *program->frame_graph.source_documents)
            append_document(ref, paths.normalizedReference(ref), text);
    // Recompile the published root and dependency JSON, including documents
    // removed or changed on disk. This runtime update never writes sources.
    // Shader and binary assets continue through their own resource loaders.
    const auto result = applyRenderPipelineAuthoringCandidate(
        RenderConfigCandidateDocumentSet{normalized, std::move(documents)}, [] {});
    if (!result.committed && !result.staged && logger)
        LOG_WARNING(logger, "graph GPU ownership update failed: {}", result.error);
}

void Renderer::relowerRenderPipelineForCurrentOutput() {
    auto variants =
        loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays();
    const auto generation =
        GET_MODULE(FrameGraphRuntimeContainer)
            .renderingSnapshot();
    if (generation == nullptr) {
        throw std::runtime_error(
            "output re-lowering published no renderer generation");
    }

    const auto next_rendering_pass =
        active_graph_variant ==
                RenderGraphVariant::flat
            ? variants.flat
            : variants.xr.value_or(
                  invalidRenderingPassId());
    if (next_rendering_pass ==
        invalidRenderingPassId()) {
        throw std::runtime_error(
            "output re-lowering removed the active XR graph variant");
    }

    std::optional<std::set<watch::AssetKey>>
        watched_sources;
    if (render_pipeline_reload_state != nullptr) {
        watched_sources =
            renderPipelineWatchSources(
                render_pipeline_reload_state
                    ->source_reference,
                *generation, variants.flat);
    }

    flat_rendering_pass_id = variants.flat;
    xr_rendering_pass_id = variants.xr;
    xr_excluded_features =
        std::move(variants.xr_excluded_features);
    preview_graph_program =
        std::move(variants.preview);
    current_rendering_pass_id =
        next_rendering_pass;

    if (auto *targets =
            FastModuleContainer::tryGet<
                RenderTargetContainer>()) {
        targets->resetHistory();
    }
    if (auto *instances =
            FastModuleContainer::tryGet<
                PolygonInstanceContainer>()) {
        instances->resetTemporalHistory();
    }
    render_target_layout_tracker.reset();
    temporal_reset_requested = true;

    if (render_pipeline_reload_state != nullptr) {
        auto &state =
            *render_pipeline_reload_state;
        state.watched_sources =
            std::move(*watched_sources);
        state.last_generation =
            generation->generation;
        state.last_error.clear();
    }
}

RenderPipelineAuthoringApplyResult
Renderer::reloadRenderPipelineFromDisk(
    std::span<const watch::ReloadRequest>
        companion_requests) noexcept {
    RenderPipelineAuthoringApplyResult result;
    if (render_pipeline_reload_state == nullptr) {
        result.error =
            "render pipeline reload state is unavailable";
        return result;
    }
    auto &state = *render_pipeline_reload_state;
    ++state.attempted;
    try {
        auto &config =
            GET_MODULE(ProjectBasicConfig);
        auto json =
            GET_MODULE(PathResolver).loadText(
                state.source_reference);
        const auto default_pass_name =
            config.defaultRenderingPass();
        auto transaction = std::make_shared<
            RenderPipelinePublicationTransaction>();
        transaction->companion_requests.assign(
            companion_requests.begin(),
            companion_requests.end());
        transaction->config_json = json;
        transaction->rollback_failure_message =
            "pelican.ray_tracing.generation_rolled_back@1: staged render pipeline generation was not published";

        RenderGraphVariantLoadHooks hooks;
        if (auto *reload_service =
                FastModuleContainer::tryGet<
                    watch::ReloadService>()) {
            hooks.validate_live_materials = false;
            hooks.before_publish =
                [reload_service,
                 transaction,
                 this,
                 default_pass_name](
                    const RendererRuntimeGeneration
                        &generation) {
                    try {
                        transaction->model_reload = reload_service->prepareRenderPipelineModelReload(
                            transaction->companion_requests);
                        reload_service
                            ->applyRenderPipelineCompanionReload(
                                generation,
                                transaction
                                    ->companion_requests);
                        const auto flat =
                            generation.name_to_id.find(
                                default_pass_name);
                        if (flat ==
                                generation.name_to_id.end() ||
                            !isValidRenderingPassId(
                                flat->second)) {
                            throw std::runtime_error(
                                "Rendering pass not found: " +
                                default_pass_name);
                        }
                        transaction
                            ->current_rendering_pass_id =
                            flat->second;
                        if (active_graph_variant ==
                            RenderGraphVariant::xr) {
                            const auto xr =
                                generation.name_to_id.find(
                                    default_pass_name +
                                    "#xr");
                            if (xr ==
                                    generation.name_to_id.end() ||
                                !isValidRenderingPassId(
                                    xr->second)) {
                                throw std::runtime_error(
                                    "render pipeline reload removed the active XR graph variant");
                            }
                            transaction
                                ->current_rendering_pass_id =
                                xr->second;
                        }
                        transaction->watched_sources =
                            renderPipelineWatchSources(
                                render_pipeline_reload_state
                                    ->source_reference,
                                generation,
                                flat->second);
                        transaction->generation =
                            generation.generation;
                        if (transaction->model_reload) transaction->model_reload->preparePublication();
                    } catch (const std::exception &caught) {
                        transaction->failure = caught.what();
                        throw;
                    } catch (...) {
                        transaction->failure =
                            "unknown render pipeline pre-publication failure";
                        throw;
                    }
                };
        } else {
            hooks.before_publish =
                [transaction, this,
                 default_pass_name](
                    const RendererRuntimeGeneration
                        &generation) {
                    const auto flat =
                        generation.name_to_id.find(
                            default_pass_name);
                    if (flat == generation.name_to_id.end() ||
                        !isValidRenderingPassId(flat->second)) {
                        throw std::runtime_error(
                            "Rendering pass not found: " +
                            default_pass_name);
                    }
                    transaction->current_rendering_pass_id =
                        flat->second;
                    transaction->watched_sources =
                        renderPipelineWatchSources(
                            render_pipeline_reload_state
                                ->source_reference,
                            generation, flat->second);
                    transaction->generation =
                        generation.generation;
                };
        }
        hooks.after_publish =
            [transaction, this](
                const RendererRuntimeGeneration
                    &generation) noexcept {
                if (transaction->model_reload) {
                    transaction->model_reload->commit();
                    transaction->model_reload.reset();
                }
                transaction->generation = generation.generation;
                transaction->runtime_published = true;
                transaction->publish(*this);
            };
        hooks.after_rollback = [transaction, this]() noexcept {
            transaction->rollback(*this);
        };
        auto variants =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                json, std::move(hooks));
        transaction->variants.emplace(
            std::move(variants));
        transaction->ready = true;
        const auto staged =
            GET_MODULE(FrameGraphRuntimeContainer)
                .stagedGeneration();
        if (transaction->generation == 0) {
            transaction->generation = staged;
        }
        const bool staged_publication =
            staged != 0 && staged == transaction->generation;
        if (staged_publication) {
            state.staged_generation = staged;
        }
        transaction->publish(*this);
        if (staged == 0 && !transaction->finalized) {
            throw std::runtime_error(
                "render pipeline reload publication completion was not observed");
        }
        if (staged_publication) {
            result.staged = true;
            result.staged_generation = staged;
        } else {
            result.committed = true;
            if (transaction->outcome) {
                result.published_generation =
                    transaction->outcome->published_generation;
                result.post_commit_error =
                    transaction->outcome->post_commit_error;
            }
        }
        return result;
    } catch (const std::exception &caught) {
        result = RenderPipelineAuthoringApplyResult{
            .error = caught.what()};
    } catch (...) {
        result = RenderPipelineAuthoringApplyResult{
            .error =
                "unknown render pipeline reload failure"};
    }
    state.last_error = result.error;
    ++state.failed;
    if (logger) {
        LOG_WARNING(
            logger,
            "render pipeline reload failed: {}",
            result.error);
    }
    return result;
}

RenderPipelineAuthoringApplyResult
Renderer::applyRenderPipelineAuthoringCandidate(
    RenderConfigCandidateDocumentSet candidate_documents,
    std::function<void()> source_commit,
    RenderPipelineAuthoringCompletion completion) {
    RenderPipelineAuthoringApplyResult result;
    if (!source_commit) {
        result.error =
            "render pipeline authoring apply requires a source commit callback";
        return result;
    }
    if (render_pipeline_reload_state == nullptr) {
        result.error =
            "render pipeline reload state is unavailable";
        return result;
    }

    auto &state = *render_pipeline_reload_state;
    ++state.attempted;
    try {
        auto &config = GET_MODULE(ProjectBasicConfig);
        const auto default_pass_name =
            config.defaultRenderingPass();
        auto transaction = std::make_shared<
            RenderPipelinePublicationTransaction>();
        transaction->authored = true;
        transaction->source_commit =
            std::move(source_commit);
        transaction->completion =
            std::move(completion);
        transaction->config_json =
            candidate_documents.rootDocument().bytes;
        transaction->candidate_documents.emplace(
            std::move(candidate_documents));
        transaction->rollback_failure_message =
            "pelican.ray_tracing.generation_rolled_back@1: staged authored render pipeline generation was not published";

        RenderGraphVariantLoadHooks hooks;
        hooks.validate_live_materials = true;
        hooks.load_document =
            [transaction](std::string_view reference) {
                return loadRenderConfigCandidateText(
                    *transaction->candidate_documents,
                    GET_MODULE(PathResolver),
                    reference);
            };
        hooks.before_publish =
            [this, transaction,
             default_pass_name](
                const RendererRuntimeGeneration &generation) {
                try {
                    const auto flat =
                        generation.name_to_id.find(
                            default_pass_name);
                    if (flat == generation.name_to_id.end() ||
                        !isValidRenderingPassId(flat->second)) {
                        throw std::runtime_error(
                            "Rendering pass not found: " +
                            default_pass_name);
                    }
                    transaction->current_rendering_pass_id =
                        flat->second;
                    if (active_graph_variant ==
                        RenderGraphVariant::xr) {
                        const auto xr = generation.name_to_id.find(
                            default_pass_name + "#xr");
                        if (xr == generation.name_to_id.end() ||
                            !isValidRenderingPassId(xr->second)) {
                            throw std::runtime_error(
                                "render pipeline authoring candidate removed the active XR graph variant");
                        }
                        transaction->current_rendering_pass_id =
                        xr->second;
                    }
                    transaction->watched_sources =
                        renderPipelineWatchSources(
                            render_pipeline_reload_state
                                ->source_reference,
                            generation, flat->second);
                    transaction->generation =
                        generation.generation;
                    // Capture the latest CPU state at publication, including
                    // updates made while a ray-layout candidate was staged.
                    const auto previous = GET_MODULE(RenderingPassContainer).snapshot();
                    const auto new_program = generation.programs.find(flat->second);
                    std::shared_ptr<const ValueGraph::Graph> old_graph;
                    if (previous)
                        if (const auto old = previous->programs.find(flat_rendering_pass_id); old != previous->programs.end())
                            old_graph = old->second.frame_graph.value_graph;
                    if (!transaction->completion_required && new_program != generation.programs.end())
                        transaction->graph_runtime_transfer = GET_MODULE(SceneLoader).prepareGraphRuntimeTransfer(
                            old_graph, new_program->second.frame_graph.value_graph);
                    transaction->source_commit();
                } catch (const std::exception &caught) {
                    transaction->failure = caught.what();
                    throw;
                } catch (...) {
                    transaction->failure =
                        "unknown authored render pipeline pre-publication failure";
                    throw;
                }
            };
        hooks.after_publish =
            [transaction, this](
                const RendererRuntimeGeneration
                    &generation) noexcept {
                transaction->generation = generation.generation;
                transaction->runtime_published = true;
                transaction->publish(*this);
            };
        hooks.after_rollback = [transaction, this]() noexcept {
            transaction->rollback(*this);
        };

        auto variants =
            loadRenderGraphVariantsFromConfigDataWithStartupFeatureOverlays(
                transaction->candidate_documents
                    ->rootDocument().bytes,
                std::move(hooks));
        transaction->variants.emplace(
            std::move(variants));
        transaction->ready = true;
        const auto staged =
            GET_MODULE(FrameGraphRuntimeContainer)
                .stagedGeneration();
        if (transaction->generation == 0) {
            transaction->generation = staged;
        }
        if (staged != 0 && staged == transaction->generation) {
            transaction->completion_required = true;
            state.staged_generation = staged;
            result.staged = true;
            result.staged_generation = staged;
            return result;
        }
        transaction->publish(*this);
        if (!transaction->outcome) {
            throw std::runtime_error(
                "authored render pipeline publication completion was not observed");
        }
        return *transaction->outcome;
    } catch (const std::exception &caught) {
        result.error = caught.what();
    } catch (...) {
        result.error =
            "unknown render pipeline authoring apply failure";
    }

    state.last_error = result.error;
    ++state.failed;
    if (logger) {
        LOG_WARNING(
            logger,
            "render pipeline authoring apply failed: {}",
            result.error);
    }
    return result;
}

nlohmann::ordered_json Renderer::previewIsolationStateJson() const {
    using Json = nlohmann::ordered_json;
    const auto matrix = [](const glm::mat4 &value) {
        auto result = Json::array();
        for (glm::length_t row = 0; row < 4; ++row) {
            auto values = Json::array();
            for (glm::length_t column = 0; column < 4; ++column) {
                values.push_back(value[column][row]);
            }
            result.push_back(std::move(values));
        }
        return result;
    };
    const auto vec3 = [](glm::vec3 value) {
        return Json::array({value.x, value.y, value.z});
    };
    const auto vec2 = [](glm::vec2 value) {
        return Json::array({value.x, value.y});
    };
    const auto history = [&](const TemporalFrameHistory &value) {
        return Json{{"valid", value.valid},
                    {"projection_non_jittered", matrix(value.projection_non_jittered)},
                    {"view_projection_non_jittered", matrix(value.view_projection_non_jittered)},
                    {"projection_jittered", matrix(value.projection_jittered)},
                    {"view_projection_jittered", matrix(value.view_projection_jittered)},
                    {"view", matrix(value.view)},
                    {"camera_position", vec3(value.camera_position)},
                    {"jitter_ndc", vec2(value.jitter_ndc)},
                    {"temporal_reset_epoch", value.temporal_reset_epoch}};
    };
    const auto histories = [&](const TemporalViewFamilyHistory &family) {
        auto result = Json::array();
        for (const auto &[view_id, value] : family.views) {
            auto entry = history(value);
            entry["view_id"] = view_id;
            result.push_back(std::move(entry));
        }
        return result;
    };
    auto snapshots = Json::array();
    for (const auto &value : last_view_snapshots) {
        snapshots.push_back({
            {"projection_non_jittered", matrix(value.projection_non_jittered)},
            {"view_projection_non_jittered", matrix(value.view_projection_non_jittered)},
            {"previous_projection_non_jittered", matrix(value.previous_projection_non_jittered)},
            {"previous_view_projection_non_jittered", matrix(value.previous_view_projection_non_jittered)},
            {"projection_jittered", matrix(value.projection_jittered)},
            {"view_projection_jittered", matrix(value.view_projection_jittered)},
            {"previous_projection_jittered", matrix(value.previous_projection_jittered)},
            {"previous_view_projection_jittered", matrix(value.previous_view_projection_jittered)},
            {"view", matrix(value.view)}, {"previous_view", matrix(value.previous_view)},
            {"camera_position", vec3(value.camera_position)},
            {"previous_camera_position", vec3(value.previous_camera_position)},
            {"jitter_ndc", vec2(value.jitter_ndc)},
            {"previous_jitter_ndc", vec2(value.previous_jitter_ndc)},
            {"temporal_reset_epoch", value.temporal_reset_epoch},
            {"previous_temporal_reset_epoch", value.previous_temporal_reset_epoch},
        });
    }
    return {{"current_rendering_pass", current_rendering_pass_id.value},
            {"flat_rendering_pass", flat_rendering_pass_id.value},
            {"xr_rendering_pass", xr_rendering_pass_id
                                       ? Json(xr_rendering_pass_id->value) : Json(nullptr)},
            {"active_graph_variant", active_graph_variant == RenderGraphVariant::flat
                                         ? "flat" : "xr"},
            {"flat_history_family_id", flat_temporal_history.family_id},
            {"flat_histories", histories(flat_temporal_history)},
            {"xr_history_family_id", xr_temporal_history.family_id},
            {"xr_histories", histories(xr_temporal_history)},
            {"last_view_snapshots", std::move(snapshots)},
            {"temporal_reset_requested", temporal_reset_requested},
            {"observed_time_set_revision", observed_time_set_revision},
            {"observed_camera_discontinuity_revision", observed_camera_discontinuity_revision},
            {"internal_render_extent", internal_render_extent
                 ? Json{{"width", internal_render_extent->width},
                        {"height", internal_render_extent->height}} : Json(nullptr)},
            {"graph_transition_trace", graph_variant_transition_trace},
            {"pending_graph_transition", pending_graph_transition
                 ? Json(*pending_graph_transition) : Json(nullptr)},
            {"preview_graph_generation", preview_graph_program.generation}};
}

TemporalViewFamilyHistory &Renderer::activeTemporalHistory() {
    return active_graph_variant == RenderGraphVariant::flat
               ? flat_temporal_history
               : xr_temporal_history;
}

const TemporalViewFamilyHistory &Renderer::activeTemporalHistory() const {
    return active_graph_variant == RenderGraphVariant::flat
               ? flat_temporal_history
               : xr_temporal_history;
}

nlohmann::json Renderer::currentFramePlanJson() const {
    const auto *frame_graph_runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>();
    // The published plan. Before anything is published, a ray-tracing
    // startup generation waits staged for its first submitted frame; frames
    // already render from it, so answer with it rather than reporting no plan
    // (Studio asks as soon as the engine answers RPC).
    auto generation =
        frame_graph_runtime != nullptr
            ? frame_graph_runtime->snapshot()
            : nullptr;
    if (generation == nullptr && frame_graph_runtime != nullptr) {
        generation = frame_graph_runtime->renderingSnapshot();
    }
    const auto *program =
        generation != nullptr
            ? generation->find(current_rendering_pass_id)
            : nullptr;
    const auto *frame_graph =
        program != nullptr ? &program->frame_graph : nullptr;
    if (frame_graph == nullptr) {
        throw std::runtime_error("Current frame plan is not registered");
    }
    auto result = framePlanToJson(frame_graph->plan,
                                  frame_graph->render_pipeline.get());
    appendShaderResolution(
        result, program->rendering_pass,
        GET_MODULE(PathResolver));
    result["execution_plan"] =
        frameExecutionPlanToJson(
            frame_graph->execution_plan);
    result["runtime_generation"] = generation->generation;
    // CPU-side runtime settings at query time, not a completed GPU submission.
    // Use registered task definitions: compiled declarations retain initial values.
    // Only an already-created container is queried: this is a read-only
    // projection and must not construct GPU modules, so the CPU-only frame plan
    // seam stays free of them. Without the container no task is registered and
    // there are no runtime settings to overlay.
    if (const auto *compute_tasks = FastModuleContainer::tryGet<ComputeTaskContainer>()) {
        for (const auto &compiled : program->rendering_pass.compute_tasks) {
            const auto &definition = compute_tasks->definition(compiled.task_id);
            const auto &ray = definition.ray_tracing;
            if (!ray || !ray->callable_record_layout) continue;
            auto records = nlohmann::json::array();
            for (std::size_t index = 0; index < ray->callable_records.size(); ++index) {
                records.push_back({{"index", index}, {"fields", rayRecordFieldsJson(
                    ray->callable_record_layout->members, ray->callable_records[index])}});
            }
            for (auto &node : result["nodes"]) {
                if (node.at("kind") == "compute" && node.at("name") == compiled.definition.name) {
                    node["runtime_callable_records"] = {{"source", "runtime_cpu_settings"},
                        {"records", std::move(records)}};
                    break;
                }
            }
        }
    }
    result["gpu_owner_scope"] = program->owner_scope;
    result["retained_resource_lease_count"] =
        program->resource_leases.size();
    if (generation->window_output != nullptr) {
        const auto &output =
            *generation->window_output;
        const auto &facts =
            output.compile_facts;
        result["window_output"] = {
            {"generation", output.generation},
            {"compile_fingerprint",
             output.compile_fingerprint},
            {"target_kind",
             outputTargetKindName(
                 facts.target_kind)},
            {"extent",
             {{"width", facts.extent.width},
              {"height", facts.extent.height}}},
            {"color_format",
             vk::to_string(facts.color_format)},
            {"color_space",
             vk::to_string(facts.color_space)},
            {"encoding_path",
             outputEncodingPathName(
                 facts.encoding_path)},
            {"selected_usage",
             static_cast<std::uint32_t>(
                 static_cast<VkImageUsageFlags>(
                     facts.selected_usage))},
            {"capture_available",
             facts.capture_available},
            {"surface_transform",
             vk::to_string(
                 facts.surface_transform)},
            {"graphics_queue_family",
             facts.graphics_queue_family},
            {"presentation_queue_family",
             facts.presentation_queue_family},
        };
    }
    if (const auto *instances =
            FastModuleContainer::tryGet<
                PolygonInstanceContainer>();
        instances != nullptr &&
        result.contains("nodes") &&
        result.at("nodes").is_array()) {
        for (const auto &compiled_pass :
             program->rendering_pass.passes) {
            if (!compiled_pass.definition.isMaterial() ||
                !compiled_pass.definition
                     .materialInfo()
                     .material_filter) {
                continue;
            }
            const auto &filter =
                *compiled_pass.definition
                     .materialInfo()
                     .material_filter;
            const auto contract =
                compiled_pass.definition
                    .materialInfo()
                    .contract;
            const auto *resolution =
                contract ==
                        MaterialPassContract::
                            legacy_gbuffer_v1
                    ? instances
                          ->materialFilterResolution(
                              filter.id)
                    : instances
                          ->materialFilterResolution(
                              materialPassPhase(
                                  contract),
                              filter.id);
            if (resolution == nullptr) {
                resolution =
                    instances
                        ->materialFilterResolution(
                            filter.id);
            }
            if (resolution == nullptr) continue;
            applyMaterialDrawFilterResolutionToFramePlanJson(
                result,
                compiled_pass.definition.name,
                filter.id,
                resolution
                    ->resolved_draw_count,
                resolution
                    ->unmatched_include,
                resolution
                    ->unmatched_exclude);
        }
    }
    if (generation->gpu_arena != nullptr) {
        nlohmann::json scopes = nlohmann::json::array();
        for (const auto &scope :
             generation->gpu_arena->scopes) {
            nlohmann::json resources =
                nlohmann::json::array();
            for (const auto &resource : scope.resources) {
                resources.push_back(
                    {{"kind",
                      renderPipelineGpuResourceKindName(
                          resource.kind)},
                     {"handle", resource.handle},
                     {"name", resource.name},
                     {"declared_bytes",
                      resource.declared_bytes}});
            }
            scopes.push_back(
                {{"owner_scope", scope.owner_scope},
                 {"resource_lease_count",
                  scope.resource_leases.size()},
                 {"resources", std::move(resources)}});
        }
        result["gpu_resource_arena"] = {
            {"runtime_generation",
             generation->gpu_arena
                 ->runtime_generation},
            {"resource_count",
             generation->gpu_arena->resourceCount()},
            {"scopes", std::move(scopes)},
        };
    }
    result["ray_query_acceleration_structures"] =
        rayQueryAccelerationStructureDiagnosticsToJson(
            rayQueryAccelerationStructureDiagnosticsForTesting());
    if (frame_graph->target_plan != nullptr) {
        result["physical_target_plan"] =
            vulkanTargetPlanToJson(*frame_graph->target_plan);
        if (frame_graph->target_plan->resolution_plan) {
            const auto *render_targets =
                FastModuleContainer::tryGet<
                    RenderTargetContainer>();
            if (render_targets == nullptr) {
                throw std::runtime_error(
                    "Current frame runtime resolution has no render target container");
            }

            std::optional<vk::Extent2D> runtime_output_extent;
            if (generation->window_output != nullptr) {
                runtime_output_extent =
                    generation->window_output
                        ->compile_facts.extent;
            } else {
                runtime_output_extent =
                    internal_render_extent;
            }
            if (!runtime_output_extent) {
                throw std::runtime_error(
                    "Current frame runtime resolution has no output extent");
            }

            result["runtime_resolution"] =
                frameRuntimeResolutionWireToJson(
                    makeFrameRuntimeResolutionWire(
                        *frame_graph->target_plan,
                        [&](std::string_view resource) {
                            const auto extent =
                                resolveResolutionSourceExtent(
                                    resource, *frame_graph,
                                    *render_targets,
                                    *runtime_output_extent);
                            return ResolvedResourceExtent{
                                .width = extent.width,
                                .height = extent.height,
                            };
                        }));
        }
    }
    if (frame_graph->native_scopes != nullptr) {
        nlohmann::json scopes =
            nlohmann::json::array();
        for (const auto &scope :
             frame_graph->native_scopes->scopes()) {
            nlohmann::json resources =
                nlohmann::json::array();
            for (const auto &resource :
                 scope.resources()) {
                resources.push_back(
                    {
                        {"logical_resource",
                         resource.boundary
                             .logical_resource},
                        {"semantic_type",
                         resource.boundary
                             .semantic_type},
                        {"access",
                         logicalAccessModeName(
                             resource.boundary
                                 .access)},
                        {"ownership",
                         vulkanNativeScopeResourceOwnershipName(
                             resource.boundary
                                 .ownership)},
                        {"runtime_kind",
                         vulkanNativeScopePreparedResourceKindName(
                             resource.kind)},
                    });
            }
            const auto &selection =
                scope.selection();
            scopes.push_back(
                {
                    {"scope", scope.scopeId()},
                    {"implementation",
                     selection.implementation},
                    {"provider",
                     selection.provider},
                    {"provider_owner",
                     selection.owner},
                    {"registration_generation",
                     selection
                         .registration_generation},
                    {"provider_api_version",
                     selection
                         .provider_api_version},
                    {"capabilities",
                     selection.capabilities},
                    {"synchronization",
                     vulkanNativeScopeSynchronizationModeName(
                         scope.declaration()
                             .synchronization)},
                    {"queue_capability",
                     scope.declaration()
                         .queue_capability},
                    {"resources",
                     std::move(resources)},
                });
        }
        result["native_scope_executors"] = {
            {"graph",
             frame_graph->native_scopes
                 ->graph()},
            {"package_fingerprint",
             frame_graph->native_scopes
                 ->packageFingerprint()},
            {"scopes", std::move(scopes)},
        };
    }
    if (frame_graph->sample_count_plan != nullptr &&
        frame_graph->render_pipeline->sample_count_policy.authored) {
        result["sample_count_plan"] =
            resolvedSampleCountPlanToJson(*frame_graph->sample_count_plan);
    }
    if (frame_graph->render_pipeline != nullptr &&
        frame_graph->render_pipeline
            ->lighting_data) {
        const auto &lighting =
            *frame_graph->render_pipeline
                 ->lighting_data;
        const auto *resources =
            FastModuleContainer::tryGet<
                FrameGraphResourceContainer>();
        const auto buffer_status =
            [&](const std::string &name,
                bool include_population) {
                nlohmann::json status{
                    {"resource", name},
                    {"available", false},
                    {"bytes", 0},
                };
                const auto binding =
                    frame_graph->buffer_bindings.find(
                        name);
                if (resources == nullptr ||
                    binding ==
                        frame_graph->buffer_bindings
                            .end() ||
                    !resources->hasBuffer(
                        binding->second)) {
                    return status;
                }
                status["available"] = true;
                status["bytes"] =
                    resources->bufferSize(
                        binding->second);
                if (include_population) {
                    const auto population =
                        resources
                            ->hostBufferPopulation(
                                binding->second);
                    if (population) {
                        status["source_records"] =
                            population
                                ->source_records;
                        status["written_records"] =
                            population
                                ->written_records;
                        status["dropped_records"] =
                            population
                                ->source_records -
                            std::min(
                                population
                                    ->source_records,
                                population
                                    ->written_records);
                    } else {
                        status["source_records"] =
                            nullptr;
                        status["written_records"] =
                            nullptr;
                        status["dropped_records"] =
                            nullptr;
                    }
                }
                return status;
            };

        auto inventory =
            buffer_status(
                lighting.inventory_resource,
                true);
        auto selection =
            buffer_status(
                lighting.selection_resource,
                false);
        const auto inventory_bytes =
            inventory.at("bytes")
                .get<std::uint64_t>();
        const auto selection_bytes =
            selection.at("bytes")
                .get<std::uint64_t>();
        const auto buffers_available =
            inventory.at("available")
                    .get<bool>() &&
            selection.at("available")
                    .get<bool>();
        const auto *vkcore =
            FastModuleContainer::tryGet<
                VulkanManageCore>();
        const auto maximum_storage_range =
            vkcore != nullptr
                ? static_cast<std::uint64_t>(
                      vkcore->getPhysDevice()
                          .getProperties()
                          .limits
                          .maxStorageBufferRange)
                : 0;
        result["lighting_data_runtime"] = {
            {"provider_feature",
             lighting.provider_feature},
            {"selected_path",
             lightingTargetPathName(
                 active_graph_variant ==
                         RenderGraphVariant::xr
                     ? lighting.xr_path
                     : lighting.desktop_path)},
            {"inventory",
             std::move(inventory)},
            {"selection",
             std::move(selection)},
            {"declared_vram_bytes",
             inventory_bytes +
                 selection_bytes},
            {"storage_buffer_contract",
             {
                 {"supported",
                  buffers_available &&
                      maximum_storage_range != 0 &&
                      inventory_bytes <=
                          maximum_storage_range &&
                      selection_bytes <=
                          maximum_storage_range},
                 {"max_storage_buffer_range",
                  maximum_storage_range},
             }},
            {"overflow",
             {
                 {"policy",
                  lightingOverflowPolicyName(
                      lighting.overflow)},
                 {"selection_signal",
                  "tile_count_high_bit"},
             }},
            {"fallbacks",
             {
                 {"tile_gpu",
                  {
                      {"path",
                       lightingTargetPathName(
                           lighting
                               .tile_gpu_path)},
                      {"reason",
                       lighting
                           .tile_gpu_fallback_reason},
                  }},
                 {"xr",
                  {
                      {"path",
                       lightingTargetPathName(
                           lighting.xr_path)},
                      {"reason",
                       lighting
                           .xr_fallback_reason},
                  }},
             }},
        };
    }
    const auto *sprite_scene = FastModuleContainer::tryGet<SpriteScene>();
    result["sprite"] = sprite_scene != nullptr ? sprite_scene->statusJson()
                                                 : nlohmann::json{{"enabled", false}};
    return result;
}

PickingReadbackResult Renderer::readPickingPixel(std::uint32_t x,
                                                 std::uint32_t y) {
    auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
    const auto generation = runtime.snapshot();
    const auto *program = generation != nullptr
                              ? generation->find(current_rendering_pass_id)
                              : nullptr;
    if (program == nullptr) {
        throw std::runtime_error(
            "picking readback requires a compiled render pipeline");
    }

    const auto target_id =
        boundRenderTarget(program->frame_graph, "picking_id");
    const bool has_picking_pass =
        isConcreteRenderTarget(target_id) &&
        std::any_of(
            program->rendering_pass.passes.begin(),
            program->rendering_pass.passes.end(),
            [target_id](const auto &pass) {
                return pass.definition.isPicking() &&
                       std::find(pass.definition.output_color.begin(),
                                 pass.definition.output_color.end(),
                                 target_id) !=
                           pass.definition.output_color.end();
            });
    if (!has_picking_pass) {
        throw std::runtime_error(
            "picking readback requires engine://features/picking.json in the active render graph");
    }
    if (target_id != last_picking_target ||
        current_rendering_pass_id !=
            last_picking_rendering_pass_id ||
        generation->generation !=
            last_picking_runtime_generation ||
        !last_picking_extent) {
        throw std::runtime_error(
            "picking readback requires a completed frame from the active picking render graph");
    }

    auto &targets = GET_MODULE(RenderTargetContainer);
    const auto metadata = targets.getMetadata(target_id);
    if (metadata.format != vk::Format::eR32Uint || metadata.samples != 1 ||
        metadata.dimension != ImageResourceDimension::two_d ||
        metadata.array_layers != 1 ||
        (metadata.usage & vk::ImageUsageFlagBits::eTransferSrc) !=
            vk::ImageUsageFlagBits::eTransferSrc) {
        throw std::runtime_error(
            "picking_id must be a single-sample R32_UINT 2D transfer source");
    }
    if (metadata.extent != *last_picking_extent) {
        throw std::runtime_error(
            "picking render target changed after the last completed picking frame");
    }
    if (x >= metadata.extent.width || y >= metadata.extent.height) {
        throw std::out_of_range(
            "picking coordinate is outside the picking_id extent");
    }

    const auto previous_layout = render_target_layout_tracker.currentLayout(
        target_id, false, &targets);
    if (previous_layout == vk::ImageLayout::eUndefined) {
        throw std::runtime_error(
            "picking_id has no completed image contents to read");
    }

    auto &vulkan = GET_MODULE(VulkanManageCore);
    auto staging = vulkan.allocBuf(
        sizeof(std::uint32_t), vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eHostAccessRandom);
    auto &utils = GET_MODULE(VulkanUtils);
    utils.executeOneTimeCmd(
        [&](vk::CommandBuffer command) {
            render_target_layout_tracker.transition(
                command, targets, utils, target_id,
                vk::ImageLayout::eTransferSrcOptimal);
            vk::BufferImageCopy copy;
            copy.imageSubresource = {
                vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copy.imageOffset = vk::Offset3D{
                static_cast<std::int32_t>(x),
                static_cast<std::int32_t>(y), 0};
            copy.imageExtent = vk::Extent3D{1, 1, 1};
            command.copyImageToBuffer(
                targets.getImage(target_id).image.get(),
                vk::ImageLayout::eTransferSrcOptimal,
                staging.buffer.get(), copy);
            render_target_layout_tracker.transition(
                command, targets, utils, target_id,
                previous_layout);
        },
        true);

    const auto bytes = vulkan.readBuf(staging, sizeof(std::uint32_t));
    std::uint32_t encoded_id = 0;
    std::memcpy(&encoded_id, bytes.data(), sizeof(encoded_id));

    PickingReadbackResult result{
        .x = x,
        .y = y,
        .extent = metadata.extent,
        .frame_index = last_picking_frame_index,
    };
    if (encoded_id == 0) return result;

    const auto slot = encoded_id - 1;
    if (slot >= last_picking_model_instances.size() ||
        !last_picking_model_instances[slot]) {
        throw std::runtime_error(
            "picking_id references no live model instance in its rendered frame");
    }
    result.model_instance = last_picking_model_instances[slot];
    return result;
}

RenderTargetReadback Renderer::readRenderTargetForTesting(
    std::string_view name,
    const ImageSubresourceRange &authored_range) {
    const auto generation =
        GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    const auto *program = generation != nullptr
                              ? generation->find(
                                    current_rendering_pass_id)
                              : nullptr;
    if (program == nullptr) {
        throw std::runtime_error(
            "render-target readback requires a compiled render pipeline");
    }
    const auto target_id =
        boundRenderTarget(
            program->frame_graph, std::string{name});
    if (!isConcreteRenderTarget(target_id)) {
        throw std::runtime_error(
            "render-target readback target is not active: " +
            std::string{name});
    }

    auto &targets = GET_MODULE(RenderTargetContainer);
    const auto metadata = targets.getMetadata(target_id);
    const auto texel_bytes = formatTexelBytes(metadata.format);
    if (metadata.samples != 1 || texel_bytes == 0 ||
        snapshotAspect(metadata.format) !=
            vk::ImageAspectFlagBits::eColor ||
        (metadata.usage & vk::ImageUsageFlagBits::eTransferSrc) !=
            vk::ImageUsageFlagBits::eTransferSrc) {
        throw std::runtime_error(
            "render-target readback requires a supported single-sample "
            "color transfer source: " +
            std::string{name});
    }
    const auto range = resolveImageSubresourceRange(
        authored_range, metadata.mip_levels,
        metadata.array_layers);
    if (range.level_count != 1) {
        throw std::runtime_error(
            "render-target readback requires exactly one mip level: " +
            std::string{name});
    }
    const vk::Extent2D extent{
        std::max(
            1u,
            metadata.extent.width >>
                range.base_mip_level),
        std::max(
            1u,
            metadata.extent.height >>
                range.base_mip_level),
    };
    const auto byte_count =
        static_cast<std::uint64_t>(extent.width) *
        extent.height * range.layer_count * texel_bytes;
    if (byte_count == 0 ||
        byte_count >
            std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
            "render-target readback has an invalid byte size: " +
            std::string{name});
    }
    const auto previous_layout =
        render_target_layout_tracker.currentLayout(
            target_id, false, &targets);
    if (previous_layout == vk::ImageLayout::eUndefined) {
        throw std::runtime_error(
            "render-target readback target has no completed contents: " +
            std::string{name});
    }

    auto &vulkan = GET_MODULE(VulkanManageCore);
    auto staging = vulkan.allocBuf(
        byte_count,
        vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eHostAccessRandom);
    auto &utils = GET_MODULE(VulkanUtils);
    utils.executeOneTimeCmd(
        [&](vk::CommandBuffer command) {
            render_target_layout_tracker.transition(
                command, targets, utils, target_id,
                vk::ImageLayout::eTransferSrcOptimal);
            vk::BufferImageCopy copy;
            copy.imageSubresource = {
                vk::ImageAspectFlagBits::eColor,
                range.base_mip_level,
                range.base_array_layer,
                range.layer_count};
            copy.imageExtent = vk::Extent3D{
                extent.width, extent.height, 1};
            command.copyImageToBuffer(
                targets.getImage(target_id).image.get(),
                vk::ImageLayout::eTransferSrcOptimal,
                staging.buffer.get(), copy);
            render_target_layout_tracker.transition(
                command, targets, utils, target_id,
                previous_layout);
        },
        true);

    return RenderTargetReadback{
        .extent = extent,
        .format = metadata.format,
        .layer_count = range.layer_count,
        .image_layer_count = metadata.array_layers,
        .bytes = vulkan.readBuf(
            staging,
            static_cast<std::size_t>(byte_count)),
    };
}

R8RenderTargetReadback Renderer::readR8RenderTargetForTesting(
    std::string_view name) {
    // The selected range is always one layer, so it cannot speak for the
    // image. Ask the target before narrowing, or a six-layer cube would
    // quietly answer with its first face.
    auto readback = readRenderTargetForTesting(
        name, ImageSubresourceRange{});
    if (readback.format != vk::Format::eR8Unorm ||
        readback.image_layer_count != 1 ||
        readback.layer_count != 1) {
        throw std::runtime_error(
            "R8 readback requires a single-layer R8_UNORM target: " +
            std::string{name});
    }
    return R8RenderTargetReadback{
        .extent = readback.extent,
        .pixels = std::move(readback.bytes),
    };
}

std::optional<vk::Format>
Renderer::xrCompositionDepthFormat() const {
    if (!xr_rendering_pass_id) {
        return std::nullopt;
    }
    const auto *frame_graph_runtime =
        FastModuleContainer::tryGet<
            FrameGraphRuntimeContainer>();
    const auto generation =
        frame_graph_runtime != nullptr
            ? frame_graph_runtime->renderingSnapshot()
            : nullptr;
    const auto *program =
        generation != nullptr
            ? generation->find(
                  *xr_rendering_pass_id)
            : nullptr;
    if (program == nullptr ||
        program->frame_graph.target_plan == nullptr ||
        !program->frame_graph.target_plan
             ->external_depth_export) {
        return std::nullopt;
    }
    const auto &export_plan =
        *program->frame_graph.target_plan
             ->external_depth_export;
    const auto binding =
        program->frame_graph
            .render_target_bindings.find(
                export_plan.source_resource);
    if (binding ==
            program->frame_graph
                .render_target_bindings.end() ||
        !isConcreteRenderTarget(binding->second)) {
        throw std::runtime_error(
            "XR external depth plan is not bound to a concrete "
            "render target");
    }
    const auto *render_targets =
        FastModuleContainer::tryGet<
            RenderTargetContainer>();
    if (render_targets == nullptr) {
        throw std::runtime_error(
            "XR external depth format requires the render-target "
            "runtime");
    }
    const auto metadata =
        render_targets->getMetadata(binding->second);
    if (export_plan.format !=
            vk::to_string(metadata.format) ||
        !(metadata.usage &
          vk::ImageUsageFlagBits::eTransferSrc)) {
        throw std::runtime_error(
            "XR external depth plan does not match its physical "
            "render target");
    }
    return metadata.format;
}

std::vector<std::string> Renderer::currentFramePlanOrderForTesting() const {
    const auto *frame_graph_runtime = FastModuleContainer::tryGet<FrameGraphRuntimeContainer>();
    const auto generation =
        frame_graph_runtime != nullptr
            ? frame_graph_runtime->snapshot()
            : nullptr;
    const auto *program =
        generation != nullptr
            ? generation->find(current_rendering_pass_id)
            : nullptr;
    const auto *frame_graph =
        program != nullptr ? &program->frame_graph : nullptr;
    if (frame_graph == nullptr) {
        throw std::runtime_error("Current frame plan is not registered");
    }
    return framePlanOrder(frame_graph->plan);
}

RayQueryAccelerationStructureDiagnostics
Renderer::rayQueryAccelerationStructureDiagnosticsForTesting() const {
    const auto *frame_graph_runtime =
        FastModuleContainer::tryGet<
            FrameGraphRuntimeContainer>();
    const auto generation =
        frame_graph_runtime != nullptr
            ? frame_graph_runtime->snapshot()
            : nullptr;
    const auto *program =
        generation != nullptr
            ? generation->find(current_rendering_pass_id)
            : nullptr;
    if (generation == nullptr || program == nullptr) {
        return {};
    }
    const auto scope = rayQueryAccelerationStructureScope(
        *generation, *program);
    auto diagnostics =
        scope != nullptr ? scope->diagnostics()
                         : RayQueryAccelerationStructureDiagnostics{};
    if (const auto *frame_resources =
            FastModuleContainer::tryGet<FrameResources>()) {
        if (const auto bundle =
                frame_resources->activeRtDescriptorBundleForTesting()) {
            diagnostics.descriptor_bundle_generation =
                bundle->generation_id;
            diagnostics.descriptor_top_level_handle =
                reinterpret_cast<std::uint64_t>(
                    static_cast<VkAccelerationStructureKHR>(
                        bundle->top_level));
            diagnostics.descriptor_row_digest =
                bundle->row_digest;
        }
    }
    return diagnostics;
}

void Renderer::recreateRenderTargetsAndRebindForTesting(vk::Extent2D extent) {
    auto modules = resolveRenderFrameModules();
    auto prepared =
        modules.render_target_container
            .prepareForExtent(extent);
    modules.render_target_container
        .publishPreparedExtent(std::move(prepared));
    rebindFullscreenInputs(modules);
    modules.instance_container.resetTemporalHistory();
    temporal_reset_requested = true;
    render_target_layout_tracker.reset();
    internal_render_extent = extent;
}

void Renderer::resetTemporalHistory() {
    auto modules = resolveRenderFrameModules();
    modules.render_target_container.resetHistory();
    modules.instance_container.resetTemporalHistory();
    temporal_reset_requested = true;
    render_target_layout_tracker.reset();
}

void Renderer::selectGraphVariant(RenderGraphVariant variant) {
    if (variant == active_graph_variant) return;
    if (variant == RenderGraphVariant::xr && !xr_rendering_pass_id) {
        throw std::runtime_error(
            "OpenXR render graph was not precompiled at startup");
    }

    const auto from = active_graph_variant;
    auto modules = resolveRenderFrameModules();
    modules.render_target_container.resetHistory();
    modules.instance_container.resetTemporalHistory();
    render_target_layout_tracker.reset();
    temporal_reset_requested = true;

    active_graph_variant = variant;
    current_rendering_pass_id =
        variant == RenderGraphVariant::flat ? flat_rendering_pass_id
                                            : *xr_rendering_pass_id;
    graph_variant_transition_trace.push_back({
        {"from", from == RenderGraphVariant::flat ? "flat" : "xr"},
        {"to", variant == RenderGraphVariant::flat ? "flat" : "xr"},
        {"temporal_reset_requests", 1},
    });
    pending_graph_transition = graph_variant_transition_trace.size() - 1;
}

void Renderer::prepareRuntimeModules() {
    auto &rendering_passes = GET_MODULE(RenderingPassContainer);
    if (rendering_passes.isFeatureEnabled("sprite")) {
        (void)GET_MODULE(SpriteScene);
        (void)GET_MODULE(SpriteRenderer);
        (void)GET_MODULE(AtlasAssetResource);
    }

    (void)resolveFrameDeletionQueue();
    (void)resolveRenderFrameModules();
    (void)resolveShaderHotReloadModules();
    (void)resolveFrameEngineTime();
    (void)GET_MODULE(PipelineFactory);
    // Runtime RPC scene loading may introduce its first standard material after
    // startup, so keep that data lazy within boot but not beyond the freeze.
    (void)GET_MODULE(StandardMaterialResource);
}

void Renderer::renderLogicalFrame(
    ILogicalFrameTarget &target,
    const RenderViewFamily &view_family) {
    renderLogicalFrame(
        target,
        makeMainRenderViewFamilies(
            view_family));
}

// One logical frame's recording state, in one object. Every lambda that used
// to share `renderLogicalFrame`'s 57-local scope is a member function here, and
// every value those lambdas captured is a field whose comment names the step
// that makes it valid. The class is a member of Renderer, so it reaches the
// renderer state a frame mutates through `renderer`.
class Renderer::LogicalFrameRecorder {
  public:
    // Resolves the frame's module graph, its published runtime generation and
    // the compiled program that generation selects. Throws before any GPU work
    // when the program is missing or has no compiled render pipeline.
    LogicalFrameRecorder(Renderer &renderer, ILogicalFrameTarget &target);

    // The steps of one logical frame, in the order renderLogicalFrame calls
    // them.
    void requireUnchangedWindowOutput();
    void collectPassPlan();
    void resolveViewFamilies(const RenderViewFamilies &view_families);
    void configureGpuTimingRanges();
    void synchronizeTemporalHistories();
    void beginFrameResources();
    void consumeShaderReload();
    void observeHistoryInvalidations();
    void publishFrameValueGraph();
    void stagePoseAttributesAndLights();
    void buildDrawSortViews();
    void planLogicalFrameSchedule();
    void configureExternalDepthExport();
    void beginLogicalFrameTarget();
    void recordExecutions();
    void submitFrame();
    void publishFrame();
    void retainPickingSnapshot();
    void publishExecutionTrace();
    void advanceFrameHistories();

  private:
    // One view's (or one view family's) recording, from the begin object the
    // caller already obtained through to the trace entry it pushes.
    // `is_view_family` is the only thing that separated the two branches this
    // replaced.
    void recordOneExecution(
        const FrameRenderContext &render_ctx,
        std::uint32_t view_index,
        std::uint32_t logical_view_count,
        bool is_view_family,
        std::span<const LogicalFrameNodeInvocation> authored_schedule);

    void recordGraphDeformation(vk::CommandBuffer command);
    void recordAccelerationStructures(
        vk::CommandBuffer command_buffer,
        std::uint32_t in_flight_frame_index);
    void publishAccelerationStructures(
        std::uint32_t in_flight_frame_index);
    bool recordSceneAs(
        const FrameRenderContext &context, std::string_view node_name);
    void capturePickingModelInstances();
    void prepareViewFamilyStates(
        std::uint32_t in_flight_frame,
        const FrameResolutionExtents &resolution_extents);
    void sealRetirementBatch();
    void publishFrameGeneration() noexcept;

    // Fields are declared in the order the old locals were constructed, so the
    // destruction order at the end of the frame is unchanged.

    // Valid from construction.
    Renderer &renderer;
    ILogicalFrameTarget &target;
    DeletionQueue &deletion_queue;
    RenderFrameModules modules;
    // One immutable publication root for the whole logical frame: a frame
    // observes either the complete old generation or the complete new one,
    // never a pass/plan mixture.
    std::shared_ptr<const RendererRuntimeGeneration> runtime_generation;
    std::uint64_t staged_runtime_generation = 0;
    StagedRuntimeRollbackGuard staged_runtime_guard;
    const CompiledRenderProgram *program = nullptr;
    const std::vector<SceneAccelerationStructureDefinition> &named_scene_as;
    bool graph_builds_scene_as = false;
    std::shared_ptr<RayQueryAccelerationStructureScope>
        acceleration_structures;
    const RenderPipelineGpuResourceScope *gpu_owner_scope = nullptr;
    bool triangle_world_space = false;
    bool procedural_world_space = false;
    std::shared_ptr<LogicalFrameSubmissionResources> submission_resources;
    const CompiledRenderingPass &rendering_pass;
    const CompiledFrameGraphExecution &frame_graph;
    GlobalRenderTargetId picking_target;
    bool picking_pass_active = false;
    std::optional<ProjectionJitterSettings> frame_projection_jitter;
    const CompiledGraphVariantPolicy &graph_variant_policy;
    const CompiledDrawSorting &draw_sorting;
    EngineTime &engine_time;
    TemporalViewFamilyHistory &temporal_history;
    RenderViewFamilyProjectionModifiers view_family_modifiers;

    // Valid from recordAccelerationStructures, which recordSceneAs runs once
    // per frame at the base scene AS node; committed in publishFrameGeneration.
    std::optional<RayQueryAccelerationStructureScope::FrameBuild>
        acceleration_structure_build;
    // Valid from recordAccelerationStructures on a frame without ray tracing,
    // and from publishAccelerationStructures on a frame with it.
    std::optional<FrameResources::FramePublication> frame_publication;
    // Valid from recordAccelerationStructures, and only when the owner scope
    // carries a ray tracing project.
    std::shared_ptr<const RayScenePlanState> ray_plan_publication;
    // Filled by recordSceneAs, one entry per AS node already recorded.
    std::set<std::string> recorded_as_nodes;

    // Filled by capturePickingModelInstances (first view, after resize),
    // moved out by retainPickingSnapshot.
    std::vector<std::optional<PickingModelInstanceToken>>
        picking_model_instances;
    bool picking_model_instances_captured = false;
    // Valid from collectPassPlan.
    GpuSubmissionLease submission_lease;
    std::vector<MaterialDrawTagFilter> material_draw_filters;
    std::vector<std::string> locally_sorted_secondary_families;

    // Valid from resolveViewFamilies.
    RenderViewFamilies resolved_view_families;
    const RenderViewFamily *main_view_family = nullptr;
    std::uint32_t view_count = 0;
    std::uint64_t sequential_view_count = 0;

    // Valid from publishFrameValueGraph.
    ValueGraph::InactiveScopes inactive_graph_scopes;
    std::unique_ptr<SceneLoader::PreparedGraphRuntimeTransfer>
        staged_graph_transfer;

    // Valid from buildDrawSortViews.
    bool per_view_sort = false;
    std::vector<DrawQueueSortView> sort_views;

    // Valid from prepareViewFamilyStates, which recordOneExecution runs on the
    // first view only.
    std::vector<RenderViewFamilyExecutionState> view_family_states;

    // Valid from planLogicalFrameSchedule.
    nlohmann::json view_traces;
    bool use_view_family_execution = false;
    std::vector<LogicalFrameNodeInvocation> logical_frame_schedule;

    // Valid from configureExternalDepthExport.
    std::optional<RuntimeExternalDepthExport> external_depth_export;
    bool external_depth_submission_active = false;

    // Valid from recordOneExecution's first view; later views check themselves
    // against them.
    std::optional<std::uint32_t> logical_in_flight_frame;
    std::optional<vk::Extent2D> logical_extent;

    // Valid from beginLogicalFrameTarget until publishFrame completes it.
    std::optional<LogicalFrameAbortGuard> frame_abort_guard;
    bool retirement_batch_sealed = false;
};

namespace {

const CompiledRenderProgram *requireCompiledRenderProgram(
    const RendererRuntimeGeneration *generation,
    RenderingPassId rendering_pass_id) {
    const auto *program =
        generation != nullptr ? generation->find(rendering_pass_id) : nullptr;
    if (program == nullptr ||
        !program->frame_graph.render_pipeline) {
        throw std::runtime_error(
            "Renderer logical frame requires a compiled render pipeline");
    }
    return program;
}

} // namespace

Renderer::LogicalFrameRecorder::LogicalFrameRecorder(
    Renderer &renderer, ILogicalFrameTarget &target)
    : renderer{renderer},
      target{target},
      deletion_queue{resolveFrameDeletionQueue()},
      modules{resolveRenderFrameModules()},
      runtime_generation{modules.frame_graph_runtime.renderingSnapshot()},
      staged_runtime_generation{
          runtime_generation != nullptr &&
                  modules.frame_graph_runtime.stagedGeneration() ==
                      runtime_generation->generation
              ? runtime_generation->generation
              : 0},
      staged_runtime_guard{
          modules.frame_graph_runtime,
          staged_runtime_generation},
      program{requireCompiledRenderProgram(
          runtime_generation.get(), renderer.current_rendering_pass_id)},
      named_scene_as{
          program->frame_graph.render_pipeline->scene_acceleration_structures},
      graph_builds_scene_as{std::any_of(
          program->frame_graph.nodes.begin(), program->frame_graph.nodes.end(),
          [](const auto &node) { return node.kind == FramePlanNodeKind::acceleration_structure; })},
      acceleration_structures{
          graph_builds_scene_as
              ? rayQueryAccelerationStructureScope(*runtime_generation, *program)
              : nullptr},
      gpu_owner_scope{
          renderPipelineGpuOwnerScope(*runtime_generation, *program)},
      triangle_world_space{gpu_owner_scope && gpu_owner_scope->ray_tracing_project &&
          gpu_owner_scope->ray_tracing_project->layout.acceleration_structure &&
          gpu_owner_scope->ray_tracing_project->layout.acceleration_structure->triangle_world_space},
      procedural_world_space{gpu_owner_scope && gpu_owner_scope->ray_tracing_project &&
          gpu_owner_scope->ray_tracing_project->layout.acceleration_structure &&
          gpu_owner_scope->ray_tracing_project->layout.acceleration_structure->procedural_world_space},
      submission_resources{
          std::make_shared<LogicalFrameSubmissionResources>()},
      rendering_pass{program->rendering_pass},
      frame_graph{program->frame_graph},
      picking_target{boundRenderTarget(frame_graph, "picking_id")},
      picking_pass_active{
          isConcreteRenderTarget(picking_target) &&
          std::any_of(
              rendering_pass.passes.begin(), rendering_pass.passes.end(),
              [this](const auto &pass) {
                  return pass.definition.isPicking() &&
                         std::find(pass.definition.output_color.begin(),
                                   pass.definition.output_color.end(),
                                   picking_target) !=
                             pass.definition.output_color.end();
              })},
      frame_projection_jitter{projectionJitterSettingsFor(frame_graph)},
      graph_variant_policy{frame_graph.render_pipeline->graph_variant_policy},
      draw_sorting{frame_graph.render_pipeline->draw_sorting},
      engine_time{resolveFrameEngineTime()},
      temporal_history{renderer.activeTemporalHistory()},
      view_family_modifiers{
          .projection_jitter = frame_projection_jitter,
      } {
    submission_resources->runtime_generation = runtime_generation;
}

void Renderer::LogicalFrameRecorder::recordGraphDeformation(
    vk::CommandBuffer command) {
    if (program->frame_graph.deformed_attributes.empty()) return;
    auto &geometry = submission_resources->deformed_geometry;
    geometry = modules.instance_container.rayQueryGeometryInstances();
    recordGraphDeformedAttributes(command, program->frame_graph, geometry,
        acceleration_structures != nullptr);
}

void Renderer::LogicalFrameRecorder::recordAccelerationStructures(
    vk::CommandBuffer command_buffer,
    std::uint32_t in_flight_frame_index) {
    // Shared geometry and SBT are prepared once. Each named TLAS
    // extends the frame's AS state at its own execution node.
    if (acceleration_structure_build) return;
    if (acceleration_structures == nullptr) {
        frame_publication.emplace(
            modules.frame_resources
                .prepareNonRayTracingPublication());
        return;
    }
    auto triangle_instances = submission_resources->deformed_geometry.empty()
        ? modules.instance_container.rayQueryGeometryInstances()
        : submission_resources->deformed_geometry;
    modules.instance_container.recordGeometryDeformation(
        command_buffer, triangle_instances, true);
    auto procedural_instances =
        GET_MODULE(ProceduralGeometryContainer)
            .proceduralRayInstances();
    for (auto &instance : procedural_instances)
        instance.ray_instance_component_witness = modules.instance_container
            .rayComponentWitness(instance.authoring_object_id);
    const auto generation = renderer.next_ray_scene_generation++;
    auto inventory =
        std::make_shared<RaySceneInventorySnapshot>(
            makeRaySceneInventorySnapshot(
                triangle_instances, procedural_instances,
                generation));
    const auto &geometry_instances = inventory->instances;
    auto generation_root =
        std::make_shared<RaySceneGenerationRoot>();
    generation_root->generation = generation;
    generation_root->inventory = inventory;
    const auto &index_buffer =
        modules.vert_buf_container.indexBuffer();
    const auto &vertex_buffer =
        modules.vert_buf_container.vertexBuffer(false);
    const auto device =
        GET_MODULE(VulkanManageCore).getDevice();
    auto geometry_snapshot =
        std::make_shared<RayTracingGeometrySnapshot>(
            makeRayTracingGeometrySnapshot(
                geometry_instances,
                device.getBufferAddress(
                    vk::BufferDeviceAddressInfo{
                        index_buffer.buffer.get()}),
                device.getBufferAddress(
                    vk::BufferDeviceAddressInfo{
                        vertex_buffer.buffer.get()})));
    geometry_snapshot->generation = generation;
    generation_root->geometry_table = geometry_snapshot;
    std::shared_ptr<const RaySceneRowSnapshot>
        ray_scene_rows;
    std::shared_ptr<const RayTracingSceneShaderBindingTables>
        ray_tracing_scene_tables;
    std::uint32_t ray_type_count = 0;
    const auto ray_tracing_pipeline_generation =
        GET_MODULE(PipelineFactory)
            .rayTracingPipelineGeneration();
    if (gpu_owner_scope != nullptr &&
        gpu_owner_scope->ray_tracing_project != nullptr) {
        const auto &ray_project =
            *gpu_owner_scope->ray_tracing_project;
        if (ray_project.layout.ray_types.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                "pelican.ray_tracing.ray_type_count_limit@1: ray type count exceeds uint32");
        }
        ray_type_count = static_cast<std::uint32_t>(
            ray_project.layout.ray_types.size());
        ray_plan_publication = modules.instance_container.prepareFrameRayScenePlan(
            gpu_owner_scope->ray_tracing_project, inventory);
        ray_scene_rows = std::make_shared<RaySceneRowSnapshot>(
            *ray_plan_publication->resolved_rows);
        const_cast<RaySceneRowSnapshot &>(*ray_scene_rows)
            .generation = generation;
        generation_root->resolved_rows = ray_scene_rows;
        std::vector<PipelineHandle> owner_pipelines;
        for (const auto &resource :
             gpu_owner_scope->resources) {
            if (resource.kind !=
                RenderPipelineGpuResourceKind::pipeline) {
                continue;
            }
            if (resource.handle < 0 ||
                resource.handle >
                    std::numeric_limits<int>::max()) {
                throw std::runtime_error(
                    "pelican.ray_tracing.scene_generation_mismatch@1: owner pipeline handle is out of range");
            }
            owner_pipelines.push_back(PipelineHandle{
                static_cast<int>(resource.handle)});
        }
        auto reused =
            rayTracingTlasRecordOffsetOverrideActiveForTesting()
                ? std::optional<RayQueryAccelerationStructureScope::FrameBuild>{}
                : acceleration_structures->tryReuseFrame(
                modules.vert_buf_container
                    .geometryAddressGeneration(),
                modules.instance_container
                    .rayQueryGeometryGeneration(),
                *geometry_snapshot, *ray_scene_rows,
                ray_type_count,
                runtime_generation->generation,
                ray_tracing_pipeline_generation, named_scene_as, triangle_world_space, procedural_world_space);
        if (reused) {
            acceleration_structure_build.emplace(
                std::move(*reused));
            ray_tracing_scene_tables =
                acceleration_structure_build->sceneTables();
            ray_scene_rows =
                ray_tracing_scene_tables->row_snapshot;
        } else {
            ray_tracing_scene_tables =
                GET_MODULE(PipelineFactory)
                    .createRayTracingSceneShaderBindingTables(
                        owner_pipelines, ray_scene_rows,
                        ray_project, geometry_snapshot,
                        generation_root);
            ray_scene_rows =
                ray_tracing_scene_tables->row_snapshot;
            generation_root->resolved_rows = ray_scene_rows;
            generation_root->sbt = ray_tracing_scene_tables;
            const auto classification =
                classifyRayQueryGeometry(geometry_instances);
            generation_root->tlas_plan =
                lowerRayTracingTlasInstancePlan(
                    geometry_instances,
                    classification.eligible_instance_indices,
                    ray_scene_rows.get(), ray_type_count);
            generation_root->tlas_plan_generation = generation;
            injectRayTracingJointTransactionPoint(
                RayTracingJointTransactionPoint::after_sbt);
        }
    }
    if (!acceleration_structure_build) {
        acceleration_structure_build.emplace(
            acceleration_structures->recordFrame(
            command_buffer,
            GET_MODULE(VulkanManageCore),
            deletion_queue,
            index_buffer,
            vertex_buffer,
            modules.vert_buf_container
                .geometryAddressGeneration(),
            geometry_instances,
            geometry_snapshot,
            modules.instance_container
                .rayQueryGeometryGeneration(),
            ray_scene_rows, ray_type_count,
            ray_tracing_scene_tables,
            gpu_owner_scope != nullptr &&
                gpu_owner_scope->ray_tracing_project != nullptr &&
                gpu_owner_scope->ray_tracing_project->layout
                    .usesMaterialAlphaProfile(),
            runtime_generation->generation,
            ray_tracing_pipeline_generation,
            ray_scene_rows ? generation_root : nullptr, named_scene_as, triangle_world_space, procedural_world_space));
    }
}

void Renderer::LogicalFrameRecorder::publishAccelerationStructures(
    std::uint32_t in_flight_frame_index) {
    frame_publication.emplace(
        modules.frame_resources.prepareRayTracingPublication(
            in_flight_frame_index,
            acceleration_structure_build->topLevel(),
            acceleration_structure_build->sceneTables(),
            std::string{
                acceleration_structure_build->rowDigest()},
            acceleration_structure_build->namedTopLevels()));
    submission_resources->rt_state =
        acceleration_structure_build->stateLease();
    // Earlier commands may already bind an earlier immutable bundle.
    // Replacing the recording selection must retain all of those pools
    // through the same submission fence as the AS state.
    submission_resources->rt_bundles.push_back(frame_publication->bundleLease());
}

bool Renderer::LogicalFrameRecorder::recordSceneAs(
    const FrameRenderContext &context, std::string_view node_name) {
    // Later XR views reuse the AS operations already recorded this frame.
    if (recorded_as_nodes.contains(std::string{node_name})) return false;
    constexpr std::string_view base_node = "__scene_acceleration_structure";
    bool recorded = true;
    if (node_name == base_node) {
        recordAccelerationStructures(context.cmd_buf, context.in_flight_frame_index);
    } else {
        if (!acceleration_structure_build || !node_name.starts_with(std::string{base_node} + ":"))
            throw std::logic_error("Named AS operation requires the shared scene build: " + std::string{node_name});
        recorded = acceleration_structure_build->recordNamedTopLevel(
            context.cmd_buf, GET_MODULE(VulkanManageCore), deletion_queue,
            node_name.substr(base_node.size() + 1));
    }
    recorded_as_nodes.emplace(node_name);
    if (acceleration_structure_build)
        publishAccelerationStructures(context.in_flight_frame_index);
    if (recorded_as_nodes.size() == named_scene_as.size() + 1 &&
        gpu_owner_scope != nullptr && gpu_owner_scope->ray_tracing_project != nullptr)
        injectRayTracingJointTransactionPoint(RayTracingJointTransactionPoint::after_as_record);
    return recorded;
}

void Renderer::LogicalFrameRecorder::capturePickingModelInstances() {
    if (!picking_pass_active ||
        picking_model_instances_captured) {
        return;
    }
    const auto count =
        modules.instance_container.modelInstanceSlotCount();
    if (count >
        static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error(
            "picking model-instance slot table exceeds R32_UINT encoding");
    }
    picking_model_instances.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = modules.instance_container.modelInstanceIdAt(
            static_cast<std::uint32_t>(index));
        if (!id) continue;
        picking_model_instances[index] = PickingModelInstanceToken{
            .index = id->index,
            .generation = id->generation,
            .scene_epoch = id->scene_epoch,
        };
    }
    picking_model_instances_captured = true;
}

void Renderer::LogicalFrameRecorder::prepareViewFamilyStates(
    std::uint32_t in_flight_frame,
    const FrameResolutionExtents &resolution_extents) {
    if (!view_family_states.empty()) {
        throw std::logic_error(
            "render view families were prepared more than once");
    }
    std::uint32_t slot_base = 0;
    for (const auto &family :
         resolved_view_families
             .families) {
        auto family_resolution_extents =
            resolution_extents;
        if (family.family_id !=
            mainRenderViewFamilyId) {
            if (const auto raster_extent =
                    resolveViewFamilyRasterExtent(
                        family.family_id,
                        rendering_pass,
                        frame_graph,
                        modules
                            .render_target_container)) {
                family_resolution_extents =
                    FrameResolutionExtents{
                        .render =
                            *raster_extent,
                        .output =
                            *raster_extent,
                    };
            }
        }
        auto &history =
            family.family_id ==
                    mainRenderViewFamilyId
                ? temporal_history
                : renderer.secondary_temporal_histories
                      .at(
                          family.family_id);
        const auto modifiers =
            family.family_id ==
                    mainRenderViewFamilyId
                ? view_family_modifiers
                : RenderViewFamilyProjectionModifiers{};
        RenderViewFamilyExecutionState
            state{
                .family = &family,
                .snapshots =
                    buildRenderViewFamilySnapshots(
                        history,
                        family,
                        modifiers,
                        graph_variant_policy,
                        engine_time
                            .frameIndex(),
                        family_resolution_extents
                            .render.width,
                        family_resolution_extents
                            .render.height,
                        renderer.temporal_reset_requested),
                .sequential_slot_base =
                    slot_base,
            };
        const auto family_view_count =
            static_cast<std::uint32_t>(
                family.views.size());
        state.frame_uniforms.reserve(
            family_view_count);
        state.frame_resolutions.reserve(
            family_view_count);
        for (std::uint32_t
                 family_view_index = 0;
             family_view_index <
             family_view_count;
             ++family_view_index) {
            modules.frame_resources
                .selectSequentialView(
                    in_flight_frame,
                    slot_base +
                        family_view_index);
            state.frame_uniforms
                .push_back(
                    updateFrameResources(
                        modules,
                        engine_time,
                        family_resolution_extents
                            .render,
                        family_resolution_extents
                            .output,
                        state.snapshots
                            .at(
                                family_view_index),
                        family.views
                            .at(
                                family_view_index)
                            .clip_plane,
                        family_view_index,
                        family_view_count,
                        family.family_id));
            state.frame_resolutions
                .push_back(
                    frameResolutionData(
                        family_resolution_extents
                            .render,
                        family_resolution_extents
                            .output));
        }
        slot_base +=
            family_view_count;
        view_family_states.push_back(
            std::move(state));
    }
}

void Renderer::LogicalFrameRecorder::sealRetirementBatch() {
    if (retirement_batch_sealed) return;
    deletion_queue.confirmSubmission();
    retirement_batch_sealed = true;
}

void Renderer::LogicalFrameRecorder::publishFrameGeneration() noexcept {
    // Submission has succeeded. RT state, descriptor/SBT selection, and
    // the staged renderer root advance exactly once and are never rolled
    // back by output publication failure.
    if (acceleration_structure_build) {
        acceleration_structure_build->commit();
    }
    if (frame_publication) {
        frame_publication->commit();
    }
    if (ray_plan_publication)
        modules.instance_container.publishRayScenePlan(ray_plan_publication);
    if (staged_runtime_generation != 0) {
        if (staged_graph_transfer) staged_graph_transfer->commit();
        if (!modules.frame_graph_runtime
                 .publishStagedGeneration(
                     staged_runtime_generation)) {
            std::terminate();
        }
        staged_runtime_guard.complete();
    }
}

void Renderer::LogicalFrameRecorder::requireUnchangedWindowOutput() {
    // Most output revisions are caught before an image is acquired. The
    // post-begin check remains necessary because acquire itself may replace a
    // blocking legacy WP215 swapchain; WP216 removes that mutable cutover.
    if (windowOutputFactsChanged(
            target, runtime_generation)) {
        throw OutputRelowerRequired{};
    }
}

void Renderer::LogicalFrameRecorder::collectPassPlan() {
    submission_lease =
        deletion_queue.leaseForNextSubmission(
            submission_resources);
    std::map<std::uint64_t, std::size_t>
        material_draw_filter_indices;
    for (const auto &pass :
         rendering_pass.passes) {
        if (pass.definition.isMaterial() &&
            pass.definition.materialInfo()
                    .contract ==
                MaterialPassContract::
                    forward_transparent_v1 &&
            pass.definition.view_family !=
                mainRenderViewFamilyId &&
            std::find(
                locally_sorted_secondary_families
                    .begin(),
                locally_sorted_secondary_families
                    .end(),
                pass.definition.view_family) ==
                locally_sorted_secondary_families
                    .end()) {
            locally_sorted_secondary_families
                .push_back(
                    pass.definition
                        .view_family);
        }
        if (!pass.definition.isMaterial() ||
            !pass.definition.materialInfo()
                 .material_filter) {
            continue;
        }
        const auto &filter =
            *pass.definition.materialInfo()
                 .material_filter;
        const auto [entry, inserted] =
            material_draw_filter_indices.emplace(
                filter.id.value,
                material_draw_filters.size());
        if (inserted) {
            material_draw_filters.push_back(filter);
        } else if (
            material_draw_filters[entry->second] !=
            filter) {
            throw std::runtime_error(
                "material draw filter stable-id collision");
        }
    }
}

void Renderer::LogicalFrameRecorder::resolveViewFamilies(
    const RenderViewFamilies &view_families) {
    const auto expected_graph_variant =
        renderer.active_graph_variant == RenderGraphVariant::flat
            ? RenderPipelineGraphVariant::flat
            : RenderPipelineGraphVariant::xr;
    if (graph_variant_policy.variant !=
        expected_graph_variant) {
        throw std::runtime_error(
            "Renderer selected graph does not match its compiled graph variant policy");
    }
    validateRenderViewFamilies(
        view_families,
        graph_variant_policy);
    bool shadow_targets_changed = prepareDirectionalShadowRenderTarget(
            frame_graph,
            modules.light_container,
            modules.render_target_container);
#if PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
    shadow_targets_changed |= preparePunctualShadowRenderTargets(
        frame_graph, modules.light_container, modules.render_target_container);
#endif
    if (shadow_targets_changed) {
        rebindFullscreenInputs(modules);
        renderer.render_target_layout_tracker.reset();
    }
    const RenderViewFamilyProviderContext
        view_family_provider_context{
            .frame_graph = frame_graph,
            .lights =
                modules.light_container,
            .render_targets =
                modules
                    .render_target_container,
        };
    resolved_view_families =
        resolveRuntimeRenderViewFamilies(
            view_families,
            view_family_provider_context);
    validateRenderViewFamilies(
        resolved_view_families,
        graph_variant_policy);
    validateRuntimeRenderViewFamilyProviderContracts(
        resolved_view_families,
        view_family_provider_context);
    const auto &view_family =
        resolved_view_families.require(
            mainRenderViewFamilyId);
    const auto &views =
        view_family.views;
    main_view_family = &view_family;
    if (views.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "Renderer logical frame view count exceeds the public index range");
    }
    view_count =
        static_cast<std::uint32_t>(
            views.size());
    sequential_view_count = 0;
    for (const auto &family :
         resolved_view_families.families) {
        sequential_view_count +=
            family.views.size();
    }
    if (sequential_view_count >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "Renderer logical frame family views exceed the frame-resource "
            "slot range");
    }
}

void Renderer::LogicalFrameRecorder::configureGpuTimingRanges() {
    if (modules.render_timing != nullptr) {
        std::size_t max_nodes = 0;
        for (const auto pass_id : {renderer.flat_rendering_pass_id,
                                   renderer.xr_rendering_pass_id.value_or(renderer.flat_rendering_pass_id)}) {
            const auto *candidate =
                runtime_generation->find(pass_id);
            if (candidate != nullptr) {
                max_nodes = std::max(
                    max_nodes,
                    candidate->frame_graph.nodes.size());
            }
        }
        // XR range slots 0/1 are the eyes; 2 is the engine mirror copy and 3
        // is the independently submitted desktop mirror sink.
        const auto range_slots = renderer.xr_rendering_pass_id ? 4u : view_count;
        modules.render_timing->configureGpuQueries(
            static_cast<std::uint32_t>(in_flight_frames_num), range_slots,
            static_cast<std::uint32_t>(max_nodes));
    }
}

void Renderer::LogicalFrameRecorder::synchronizeTemporalHistories() {
    const auto &view_family = *main_view_family;
    const auto view_family_change =
        synchronizeTemporalViewFamilyHistory(
            temporal_history, view_family);
    if (view_family_change !=
        TemporalViewFamilyChange::none) {
        if (!renderer.temporal_reset_requested) {
            modules.render_target_container.resetHistory();
            modules.instance_container.resetTemporalHistory();
            renderer.render_target_layout_tracker.reset();
        }
        renderer.temporal_reset_requested = true;
    }
    for (const auto &family :
         resolved_view_families.families) {
        if (family.family_id ==
            mainRenderViewFamilyId) {
            continue;
        }
        auto &history =
            renderer.secondary_temporal_histories[
                family.family_id];
        (void)synchronizeTemporalViewFamilyHistory(
            history, family);
    }
}

void Renderer::LogicalFrameRecorder::beginFrameResources() {
    modules.frame_resources.beginLogicalFrame(
        view_count,
        static_cast<std::uint32_t>(
            sequential_view_count),
        acceleration_structures != nullptr);
}

void Renderer::LogicalFrameRecorder::consumeShaderReload() {
    auto shader_hot_reload = resolveShaderHotReloadModules();
    if (consumeShaderReloadPublication(shader_hot_reload)) {
        rebindFullscreenInputs(modules);
    }
}

void Renderer::LogicalFrameRecorder::observeHistoryInvalidations() {
    const auto time_set_revision = engine_time.timeSetRevision();
    const auto camera_discontinuity_revision = modules.camera.discontinuityRevision();
    const bool has_temporal_history =
        temporal_history.hasValidView();
    if (has_temporal_history &&
        (time_set_revision != renderer.observed_time_set_revision ||
         camera_discontinuity_revision != renderer.observed_camera_discontinuity_revision)) {
        modules.render_target_container.resetHistory();
        modules.instance_container.resetTemporalHistory();
        renderer.render_target_layout_tracker.reset();
        renderer.temporal_reset_requested = true;
    }
    renderer.observed_time_set_revision = time_set_revision;
    renderer.observed_camera_discontinuity_revision = camera_discontinuity_revision;
}

void Renderer::LogicalFrameRecorder::publishFrameValueGraph() {
    if (staged_runtime_generation != 0) {
        auto &scene = GET_MODULE(SceneLoader);
        const auto previous = GET_MODULE(RenderingPassContainer).snapshot();
        std::shared_ptr<const ValueGraph::Graph> old_graph;
        if (previous)
            if (const auto old = previous->programs.find(renderer.flat_rendering_pass_id); old != previous->programs.end())
                old_graph = old->second.frame_graph.value_graph;
        staged_graph_transfer = scene.prepareGraphRuntimeTransfer(old_graph, frame_graph.value_graph);
    }
    if (frame_graph.value_graph) {
        auto &scene = GET_MODULE(SceneLoader);
        inactive_graph_scopes = scene.inactiveGraphScopes(*frame_graph.value_graph);
        // Reactivation may still be awaiting a successful GPU publication.
        inactive_graph_scopes.insert(frame_graph.inactive_graph_scopes.begin(),
                                     frame_graph.inactive_graph_scopes.end());
        const auto inputs = collectGraphInputs(*frame_graph.value_graph, scene,
            inactive_graph_scopes, engine_time.now(), engine_time.dt());
        auto published = frame_graph.value_graph->publish(GameContext{}, inputs, {},
            [&](std::string_view name, std::span<const float> values) {
                const auto found = frame_graph.buffer_bindings.find(std::string{name});
                return found != frame_graph.buffer_bindings.end() &&
                    modules.frame_graph_resources.stageRuntimeValues(found->second, values);
            }, staged_graph_transfer ? staged_graph_transfer->parameters() : scene.graphParameterValues(frame_graph.value_graph),
            inactive_graph_scopes, staged_graph_transfer ? &staged_graph_transfer->evaluation() : &scene.graphEvaluationState(frame_graph.value_graph),
            [&](std::string_view node, Animation::CurvePlaybackCommand command) {
                if (staged_graph_transfer)
                    return staged_graph_transfer->stageAudioEvent(node, command);
                return scene.applyGraphAudioEvent(frame_graph.value_graph, node, command);
            }, [&](std::string_view node) {
                return scene.graphSound(frame_graph.value_graph, node);
            }, staged_graph_transfer ? ValueGraph::AudioValueWriter{
                [&](std::string_view node, bool voice, ValueGraph::AudioValueKind kind, float value) {
                    return staged_graph_transfer->stageAudioValue(node, voice, kind, value);
                }} : ValueGraph::AudioValueWriter{});
        if (!published.unavailable.empty())
            throw std::runtime_error("frame graph output is unavailable: " + published.unavailable.front());
        // Keep what this generation actually fed each instance input port. A
        // reader that evaluated the graph again would advance its clocks and
        // consume its events, so the values leave the draw path here or not at
        // all.
        scene.publishGraphValues({.generation = runtime_generation->generation,
                                  .frame = engine_time.frameIndex(),
                                  .inputs = std::move(published.instance_inputs),
                                  // AG-3a: the same evaluation's node values,
                                  // with the graph that names them. A reader
                                  // that wants one node's value resolves the
                                  // name itself rather than making the draw
                                  // path build a map of them every frame.
                                  .graph = frame_graph.value_graph,
                                  .node_values = std::move(published.node_values)});
    }
}

void Renderer::LogicalFrameRecorder::stagePoseAttributesAndLights() {
    stageGraphPoseAttributes(frame_graph);
    modules.frame_graph_resources.uploadRuntimeValues();
    updateFrameLights(
        modules.light_container,
        modules.frame_graph_resources,
        resolved_view_families,
        *frame_graph.render_pipeline);
}

void Renderer::LogicalFrameRecorder::buildDrawSortViews() {
    const auto &views = main_view_family->views;
    per_view_sort =
        graph_variant_policy.view_family ==
            GraphVariantViewFamily::stereo &&
        draw_sorting.xr_view_policy == DrawSortXrViewPolicy::per_view;
    if (per_view_sort && view_count != 2) {
        throw std::runtime_error(
            "XR per_view draw sorting requires exactly two views");
    }
    if (per_view_sort) {
        sort_views.reserve(view_count);
        for (const auto &view : views)
            sort_views.push_back(
                drawQueueSortView(view));
    } else {
        glm::dvec3 origin{0.0};
        glm::dvec3 forward{0.0};
        bool all_first_person = true;
        bool all_third_person = true;
        for (const auto &view : views) {
            const auto snapshot =
                drawQueueSortView(view);
            origin += glm::dvec3{snapshot.origin[0], snapshot.origin[1],
                                 snapshot.origin[2]};
            forward += glm::dvec3{snapshot.forward[0], snapshot.forward[1],
                                  snapshot.forward[2]};
            all_first_person = all_first_person && view.first_person_view;
            all_third_person = all_third_person && !view.first_person_view;
        }
        origin /= static_cast<double>(view_count);
        const auto length = glm::length(forward);
        if (!std::isfinite(length) || length <= 0.0) {
            throw std::runtime_error(
                "logical view center has an invalid forward direction");
        }
        forward /= length;
        sort_views.push_back(DrawQueueSortView{
            .logical_view = all_first_person
                                ? RenderPolicy::DrawSortLogicalViewV1::first_person
                            : all_third_person
                                ? RenderPolicy::DrawSortLogicalViewV1::third_person
                                : RenderPolicy::DrawSortLogicalViewV1::shared,
            .origin = {static_cast<float>(origin.x),
                       static_cast<float>(origin.y),
                       static_cast<float>(origin.z)},
            .forward = {static_cast<float>(forward.x),
                        static_cast<float>(forward.y),
                        static_cast<float>(forward.z)},
        });
    }
}

void Renderer::LogicalFrameRecorder::planLogicalFrameSchedule() {
    view_family_states.reserve(
        resolved_view_families
            .families.size());
    view_traces = nlohmann::json::array();
    use_view_family_execution =
        frame_graph.target_plan != nullptr &&
        frame_graph.target_plan
            ->view_execution_plan.uses_multiview;
    if (use_view_family_execution &&
        !target.supportsViewFamilyExecution()) {
        throw std::runtime_error(
            "compiled frame graph requires multiview, but the logical-frame "
            "target does not expose a view-family command context");
    }
    if (frame_graph.target_plan != nullptr) {
        std::vector<
            LogicalFrameViewFamilyCardinality>
            family_cardinalities;
        family_cardinalities.reserve(
            resolved_view_families
                .families.size());
        for (const auto &family :
             resolved_view_families
                 .families) {
            family_cardinalities.push_back(
                {
                    family.family_id,
                    static_cast<std::uint32_t>(
                        family.views.size()),
                });
        }
        logical_frame_schedule =
            buildLogicalFrameViewFamilySchedule(
                frame_graph.nodes,
                *frame_graph.target_plan,
                family_cardinalities);
    }
}

void Renderer::LogicalFrameRecorder::configureExternalDepthExport() {
    external_depth_export =
        resolveRuntimeExternalDepthExport(
            frame_graph,
            modules.render_target_container);
    external_depth_submission_active =
        target.configureExternalDepthSubmission(
            external_depth_export
                ? external_depth_export
                      ->source.format
                : vk::Format::eUndefined,
            external_depth_export
                ? external_depth_export
                      ->source.extent
                : vk::Extent2D{});
    if (external_depth_submission_active &&
        !external_depth_export) {
        throw std::runtime_error(
            "logical-frame target activated external depth without "
            "a compiled export source");
    }
}

void Renderer::LogicalFrameRecorder::beginLogicalFrameTarget() {
    try {
        target.beginLogicalFrame(
            view_count,
            LogicalFrameRuntime{
                .renderer_generation =
                    runtime_generation,
                .submission_lease =
                    submission_lease,
            });
    } catch (const OutputTemporarilyUnavailable &) {
        // Skipping a frame because the output has no image yet records no
        // GPU work and commits nothing, so a staged ray-tracing generation
        // stays staged for the next frame. Rolling it back here left a ray
        // tracing project with no renderer generation at all when its first
        // frame came before the window output existed (a project opened in
        // Studio with a warm shader cache), and the next frame then failed
        // with "Renderer logical frame requires a compiled render pipeline".
        // An output that changed still rolls back: OutputRelowerRequired
        // re-lowers and stages a new generation.
        staged_runtime_guard.complete();
        throw;
    }
    frame_abort_guard.emplace(target);
    retirement_batch_sealed = false;
}

void Renderer::LogicalFrameRecorder::recordExecutions() {
    // The only thing the two execution shapes still disagree about is which
    // begin object the target hands out. Everything after that is a parameter.
    if (use_view_family_execution) {
        const auto render_ctx =
            target.beginViewFamily(view_count);
        recordOneExecution(
            render_ctx, 0u, view_count, true, logical_frame_schedule);
        return;
    }
    for (std::uint32_t view_index = 0; view_index < view_count; ++view_index) {
        std::vector<LogicalFrameNodeInvocation>
            per_view_schedule;
        if (frame_graph.target_plan != nullptr) {
            per_view_schedule =
                selectLogicalFrameSequentialViewSchedule(
                    logical_frame_schedule,
                    view_index, view_count);
        }
        const auto render_ctx = target.beginView(view_index);
        recordOneExecution(
            render_ctx, view_index, view_count, false, per_view_schedule);
    }
}

void Renderer::LogicalFrameRecorder::recordOneExecution(
    const FrameRenderContext &render_ctx,
    std::uint32_t view_index,
    std::uint32_t logical_view_count,
    bool is_view_family,
    std::span<const LogicalFrameNodeInvocation> authored_schedule) {
    // Only a family context promises the full-array attachment plus one
    // per-layer attachment per view.
    if (is_view_family) {
        if (render_ctx.color_array_layers <
                logical_view_count ||
            render_ctx.color_layer_attachments.size() <
                logical_view_count) {
            throw std::runtime_error(
                "view-family target does not expose the required full-array "
                "and per-layer color attachment contract");
        }
    }
    if (view_index == 0) {
        logical_in_flight_frame =
            render_ctx.in_flight_frame_index;
        logical_extent = render_ctx.extent;
        const bool extent_changed =
            !renderer.internal_render_extent ||
            *renderer.internal_render_extent !=
                render_ctx.extent;
        if (handleFrameTargetResize(
                modules,
                renderer.render_target_layout_tracker,
                target, render_ctx.extent,
                extent_changed,
                runtime_generation)) {
            modules.instance_container
                .resetTemporalHistory();
            renderer.temporal_reset_requested = true;
            renderer.internal_render_extent =
                render_ctx.extent;
        }
        // Object, skin, morph, and material-override GPU state is frozen
        // after target acquisition and before the first view records.
        modules.instance_container.triggerUpdate(
            DrawQueueFramePlan{
                .opaque_provider =
                    draw_sorting.opaque.provider,
                .transparent_provider =
                    draw_sorting.transparent.provider,
                .sort_views = sort_views,
                .material_filters =
                    material_draw_filters,
            });
        recordGraphDeformation(render_ctx.cmd_buf);
        if (acceleration_structures == nullptr)
            recordSceneAs(render_ctx, "__scene_acceleration_structure");
        capturePickingModelInstances();
        prepareSecondaryViewFamilyDraws(
            modules.instance_container,
            resolved_view_families,
            locally_sorted_secondary_families);
        updateFrameDrawCandidates(
            modules.instance_container,
            modules.frame_graph_resources);
    } else {
        if (render_ctx.in_flight_frame_index != *logical_in_flight_frame) {
            throw std::runtime_error(
                "Renderer logical-frame views must share one in-flight frame index");
        }
        if (render_ctx.extent != *logical_extent) {
            throw std::runtime_error(
                "Renderer logical-frame v1 requires equal per-view extents");
        }
    }

    const auto frame_target_format =
        target.colorFormat(view_index);
    if (is_view_family) {
        for (std::uint32_t layer_index = 1;
             layer_index < logical_view_count;
             ++layer_index) {
            if (target.colorFormat(layer_index) !=
                frame_target_format) {
                throw std::runtime_error(
                    "view-family target requires one color format across "
                    "all layers");
            }
        }
    }
    if (frame_target_format !=
        modules.render_target
            .getSwapchainFormat()) {
        throw std::runtime_error(
            is_view_family
                ? "Renderer logical-frame target format does not match the "
                  "compiled graph"
                : "Renderer logical-frame target format does not match the compiled flat graph");
    }

    const auto resolution_extents =
        resolveFrameResolutionExtents(
            frame_graph,
            modules.render_target_container,
            render_ctx.extent);
    if (view_index == 0) {
        prepareViewFamilyStates(
            render_ctx
                .in_flight_frame_index,
            resolution_extents);
    }
    const auto &main_state =
        requireRenderViewFamilyExecutionState(
            view_family_states,
            mainRenderViewFamilyId);
    if (is_view_family) {
        modules.frame_resources
            .selectMultiview(
                render_ctx
                    .in_flight_frame_index);
        modules.frame_resources
            .updateMultiview(
                main_state.frame_uniforms);
        modules.frame_resources
            .updateMultiviewResolutions(
                main_state.frame_resolutions);
    } else {
        modules.frame_resources.selectSequentialView(
            render_ctx.in_flight_frame_index,
            main_state.sequential_slot_base +
                view_index);
    }

    nlohmann::json node_trace;
    nlohmann::json *node_trace_ptr = nullptr;
    if (renderer.execution_tracing_for_testing) {
        node_trace = nlohmann::json::array();
        node_trace_ptr = &node_trace;
    }
    executeRenderingPasses(
        FrameGraphExecutionRequest{
            .render_ctx = render_ctx,
            .rendering_pass = rendering_pass,
            .frame_graph = frame_graph,
            .modules = modules,
            .layout_tracker = renderer.render_target_layout_tracker,
            .record_scene_as =
                [this](const FrameRenderContext &context,
                       std::string_view node_name) {
                    return recordSceneAs(context, node_name);
                },
            .inactive_graph_scopes = inactive_graph_scopes,
            .frame_target_format = frame_target_format,
            .logical_frame = engine_time.frameIndex(),
            .graph_variant = renderPipelineGraphVariantName(
                graph_variant_policy.variant),
            .view_index = view_index,
            .logical_view_count = logical_view_count,
            .authored_schedule = authored_schedule,
            .trace_out = node_trace_ptr,
        },
        view_family_states, per_view_sort);
    if (external_depth_submission_active) {
        recordExternalDepthExport(
            render_ctx,
            *external_depth_export,
            modules.render_target_container,
            modules.vk_utils,
            renderer.render_target_layout_tracker,
            view_index, logical_view_count, is_view_family,
            node_trace_ptr);
    }
#if PELICAN_WITH_OPENXR
    if (graph_variant_policy.mirror_output ==
            GraphVariantMirrorOutput::left_eye &&
        view_index == 0) {
        recordXrMirrorIntermediate(
            render_ctx, frame_graph, modules,
            renderer.render_target_layout_tracker,
            node_trace_ptr,
            engine_time.frameIndex());
    }
#endif
    if (is_view_family) {
        target.endViewFamily(
            submission_lease);
    } else {
        target.endView(view_index, submission_lease);
    }
    if (target.submitsOnViewEnd()) {
        sealRetirementBatch();
    }
    if (renderer.execution_tracing_for_testing) {
        // nlohmann::json orders object keys itself, so assembling the entry by
        // assignment produces the document both old branches produced.
        auto view_trace =
            is_view_family
                ? nlohmann::json{{"execution", "view_family"},
                                 {"view_count", logical_view_count}}
                : nlohmann::json{{"view_index", view_index}};
        view_trace["nodes"] = std::move(node_trace);
        view_trace["final_layouts"] =
            finalLayoutsTrace(
                rendering_pass, frame_graph,
                modules
                    .render_target_container,
                renderer.render_target_layout_tracker,
                render_ctx.required_layout);
        view_traces.push_back(std::move(view_trace));
    }
}

void Renderer::LogicalFrameRecorder::submitFrame() {
    if (staged_runtime_generation != 0) {
        modules.frame_graph_runtime
            .prepareStagedGenerationCommit(
                staged_runtime_generation);
        injectRayTracingJointTransactionPoint(
            RayTracingJointTransactionPoint::submit);
    }
    target.submitLogicalFrame(submission_lease);
    sealRetirementBatch();
}

void Renderer::LogicalFrameRecorder::publishFrame() {
    static_assert(noexcept(publishFrameGeneration()));
    publishFrameGeneration();
    if (staged_graph_transfer) staged_graph_transfer->publishAudio(frame_graph.value_graph);
    if (staged_runtime_generation != 0) {
        injectRayTracingJointTransactionPoint(
            RayTracingJointTransactionPoint::after_publication);
    }
    target.publishLogicalFrame();
    frame_abort_guard->complete();
}

void Renderer::LogicalFrameRecorder::retainPickingSnapshot() {
    if (picking_pass_active &&
        picking_model_instances_captured) {
        renderer.last_picking_target = picking_target;
        renderer.last_picking_rendering_pass_id =
            renderer.current_rendering_pass_id;
        renderer.last_picking_runtime_generation =
            runtime_generation->generation;
        renderer.last_picking_extent =
            modules.render_target_container
                .getMetadata(picking_target)
                .extent;
        renderer.last_picking_frame_index =
            engine_time.frameIndex();
        renderer.last_picking_model_instances =
            std::move(picking_model_instances);
    } else {
        renderer.last_picking_target = noRenderTargetId();
        renderer.last_picking_rendering_pass_id =
            invalidRenderingPassId();
        renderer.last_picking_runtime_generation = 0;
        renderer.last_picking_extent.reset();
        renderer.last_picking_frame_index = 0;
        std::vector<std::optional<PickingModelInstanceToken>>{}
            .swap(renderer.last_picking_model_instances);
    }
}

void Renderer::LogicalFrameRecorder::publishExecutionTrace() {
    if (renderer.execution_tracing_for_testing) {
        if (use_view_family_execution) {
            renderer.last_execution_trace =
                nlohmann::json{
                    {"view_family",
                     std::move(
                         view_traces.at(0))},
                };
        } else if (view_count == 1) {
            renderer.last_execution_trace = nlohmann::json{
                {"nodes", std::move(view_traces.at(0).at("nodes"))},
                {"final_layouts", std::move(view_traces.at(0).at("final_layouts"))},
            };
        } else {
            renderer.last_execution_trace = nlohmann::json{{"views", std::move(view_traces)}};
        }
    }
}

void Renderer::LogicalFrameRecorder::advanceFrameHistories() {
    modules.render_target_container.advanceHistoryFrame();
    modules.instance_container.advanceTemporalHistoryAfterRender();
    for (const auto &state :
         view_family_states) {
        auto &history =
            state.family->family_id ==
                    mainRenderViewFamilyId
                ? temporal_history
                : renderer.secondary_temporal_histories
                      .at(
                          state.family
                              ->family_id);
        commitRenderViewFamilySnapshots(
            history, *state.family,
            state.snapshots);
    }
    renderer.last_view_snapshots =
        requireRenderViewFamilyExecutionState(
            view_family_states,
            mainRenderViewFamilyId)
            .snapshots;
    renderer.temporal_reset_requested = false;
    if (renderer.pending_graph_transition) {
        auto &transition = renderer.graph_variant_transition_trace.at(*renderer.pending_graph_transition);
        transition["reset_epochs"] = nlohmann::json::array();
        for (const auto &snapshot : renderer.last_view_snapshots) {
            transition["reset_epochs"].push_back(snapshot.temporal_reset_epoch);
        }
        transition["projection_jitter"] =
            frame_projection_jitter.has_value();
        transition["frame_plan"] = renderer.currentFramePlanOrderForTesting();
        renderer.pending_graph_transition.reset();
    }
}

void Renderer::renderLogicalFrame(
    ILogicalFrameTarget &target,
    const RenderViewFamilies &view_families) {
    synchronizeGraphGpuOwnership();
    LogicalFrameRecorder recorder{*this, target};
    recorder.requireUnchangedWindowOutput();
    recorder.collectPassPlan();
    recorder.resolveViewFamilies(view_families);
    recorder.configureGpuTimingRanges();
    recorder.synchronizeTemporalHistories();
    recorder.beginFrameResources();
    recorder.consumeShaderReload();
    recorder.observeHistoryInvalidations();
    recorder.publishFrameValueGraph();
    recorder.stagePoseAttributesAndLights();
    recorder.buildDrawSortViews();
    recorder.planLogicalFrameSchedule();
    recorder.configureExternalDepthExport();
    recorder.beginLogicalFrameTarget();
    recorder.recordExecutions();
    recorder.submitFrame();
    recorder.publishFrame();
    recorder.retainPickingSnapshot();
    recorder.publishExecutionTrace();
    recorder.advanceFrameHistories();
}

void Renderer::render(
    const RenderViewFamilies
        &view_families) {
    selectGraphVariant(RenderGraphVariant::flat);
    FlatLogicalFrameTarget target{GET_MODULE(RenderTarget)};
    for (std::uint32_t attempt = 0; attempt < 2;
         ++attempt) {
        try {
            renderLogicalFrame(
                target, view_families);
            return;
        } catch (
            const OutputTemporarilyUnavailable &) {
            // Zero extent and asynchronous WSI preparation skip only this
            // presentation frame. The app loop continues ticking input, RPC,
            // reload, audio, and ECS work.
#if PELICAN_WITH_IMGUI
            // freeze_actions already began the interactive ImGui frame.
            // A WSI skip has no ImGui render pass to close it, so do not carry
            // that frame into the next NewFrame call.
            if (auto *imgui =
                    FastModuleContainer::tryGet<
                        ImGuiSystem>()) {
                imgui->endFrameIfStarted();
            }
#endif
            return;
        } catch (const OutputRelowerRequired &) {
            try {
                relowerRenderPipelineForCurrentOutput();
            } catch (const std::exception &error) {
                throw std::runtime_error(
                    "window output re-lowering failed: " +
                    std::string{error.what()});
            }
            internal_render_extent =
                GET_MODULE(RenderTarget).getExtent();
        }
    }
    throw std::runtime_error(
        "window output compile facts changed during re-lowering");
}

void Renderer::render() {
    const auto &camera = GET_MODULE(Camera);
    render(
        makeMainRenderViewFamilies(
            makeMainRenderViewFamily(
                RenderViewParameters{
                    .view =
                        camera.getViewMatrix(),
                    .projection =
                        camera.getProjectionMatrix(),
                    .camera_position =
                        camera.getPos(),
                })));
}

} // namespace Pelican
