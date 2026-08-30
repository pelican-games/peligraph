#pragma once

#include "editorprojectiontransaction.hpp"

#include <details/ecs/entity.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

class Camera;
class BehaviorAttachmentArena;
class ComponentInfoManager;
class ECSCoreTemplatePublic;
class LightContainer;
class ModelAssetContainer;
class PhysWorld;
class PolygonInstanceContainer;

struct EditorProjectionRuntimeObjectBinding {
    AuthoringObjectId authoring_object_id{};
    EntityId entity{};
};

enum class EditorBehaviorAttachmentIdentityClaimKind : std::uint8_t {
    ExistingCas,
    DesiredIdentity,
};

// Operations contribute only identity claims. The runtime plan itself is
// enumerated from next_resolved and keyed by final authored origin.
struct EditorBehaviorAttachmentIdentityClaim {
    AuthoringObjectId object_id{};
    std::size_t authoring_component_index = 0;
    // ExistingCas validates against the arena snapshot from transaction
    // entry. This differs from the operation-time/final origin when an
    // earlier canonical operation inserted or removed a component.
    std::optional<std::size_t> pre_execution_component_index;
    std::uint64_t attachment_handle = 0;
    EditorBehaviorAttachmentIdentityClaimKind kind =
        EditorBehaviorAttachmentIdentityClaimKind::ExistingCas;
};

// Aggregate lifecycle participant for one structural object insert/restore or
// remove. It prepares the ECS entity and the renderer/camera/light/physics
// projections as one inverse token; publication contains only noexcept state
// swaps. target_object may be zero when the transaction contains exactly one
// object lifetime change; the stable ID is then resolved from the staged
// document during prepare.
class SceneObjectProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    SceneObjectProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        ModelAssetContainer &models, PolygonInstanceContainer &renderer,
        Camera &camera, LightContainer &lights, PhysWorld &physics,
        std::string scene_id, AuthoringObjectId target_object,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings);
    // Production lifecycle core for headless/runtime tests whose scene has no
    // renderer, camera, light, model, or collider projection.  It executes the
    // same staged ECS mutation used by the full adapter and deliberately fails
    // if a render/physics-owned component is encountered.
    SceneObjectProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        std::string scene_id, AuthoringObjectId target_object,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings);
    ~SceneObjectProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;

    std::span<const EditorProjectionRuntimeObjectBinding>
    runtimeBindings() const noexcept;
    std::span<const EditorProjectionRuntimeObjectBinding>
    preparedRuntimeBindings() const noexcept;
    AuthoringObjectId preparedTargetObject() const noexcept;
    void preflightDestroyForPointOfNoReturn();
};

// One current-scene lifecycle participant. Spawn tokens may be published
// reversibly during prepare so same-batch children can bind their parents.
// Destroy tokens are publish/restored as a no-allocation preflight and are
// republished only after the behavior pre-destroy barrier.
class SceneLifecycleProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    SceneLifecycleProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        ModelAssetContainer &models, PolygonInstanceContainer &renderer,
        Camera &camera, LightContainer &lights, PhysWorld &physics,
        BehaviorAttachmentArena &behavior_arena, std::string scene_id,
        std::vector<AuthoringObjectId> target_objects,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        std::vector<EditorBehaviorAttachmentIdentityClaim> identity_claims = {});
    ~SceneLifecycleProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;

    std::span<const EditorProjectionRuntimeObjectBinding>
    runtimeBindings() const noexcept;
};

// Reconciles every scene-ordered behavior attachment from next_resolved after
// lifecycle adapters have staged their entity bindings. This is a production
// inverse-token adapter; no callback-only fake target participates.
class BehaviorSceneProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    BehaviorSceneProjectionAdapter(
        BehaviorAttachmentArena &arena, std::string scene_id,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        std::vector<SceneObjectProjectionAdapter *> lifecycle_adapters,
        std::vector<EditorBehaviorAttachmentIdentityClaim> identity_claims = {});
    ~BehaviorSceneProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

// Production adapters for the non-transform registered codecs. Each adapter
// derives its next state from the staged AuthoringSceneDocument and only
// copies/swaps prepared state during publication.
class EcsCodecProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    EcsCodecProjectionAdapter(
        ECSCoreTemplatePublic &ecs, std::string scene_id,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings);
    ~EcsCodecProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

class RendererModelProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    RendererModelProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ModelAssetContainer &models,
        PolygonInstanceContainer &renderer, std::string scene_id,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings);
    ~RendererModelProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

class CameraProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    CameraProjectionAdapter(Camera &camera, std::string scene_id);
    ~CameraProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

class LightProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    LightProjectionAdapter(LightContainer &lights, std::string scene_id);
    ~LightProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

class ColliderProjectionAdapter final : public EditorProjectionAdapter {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ColliderProjectionAdapter(
        ECSCoreTemplatePublic &ecs, PhysWorld &physics, std::string scene_id,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings);
    ~ColliderProjectionAdapter();

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

EditorProjectionCallbackAdapter makeBehaviorAttachmentProjectionJunction(
    std::string name, void *context,
    EditorProjectionCallbackAdapter::Prepare prepare,
    EditorProjectionCallbackAdapter::NoexceptAction publish,
    EditorProjectionCallbackAdapter::NoexceptAction rollback,
    EditorProjectionCallbackAdapter::NoexceptAction finish) noexcept;

} // namespace Pelican
