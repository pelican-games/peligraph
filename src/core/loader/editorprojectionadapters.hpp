#pragma once

#include "editorprojectiontransaction.hpp"

#include <details/ecs/entity.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Pelican {

class Camera;
class SceneLoader;
class BehaviorAttachmentArena;
enum class BehaviorRegistryAvailability;
class ComponentInfoManager;
class ECSCoreTemplatePublic;
class LightContainer;
class ModelAssetContainer;
class PhysWorld;
class PolygonInstanceContainer;
struct ComponentCodec;

struct EditorProjectionRuntimeObjectBinding {
    AuthoringObjectId authoring_object_id{};
    EntityId entity{};
};

struct SceneObjectProjectionParticipants {
    bool renderer = true;
    bool camera = true;
    bool light = true;
    bool collider = true;

    friend bool operator==(const SceneObjectProjectionParticipants &,
                           const SceneObjectProjectionParticipants &) = default;
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
    SceneObjectProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        ModelAssetContainer &models, PolygonInstanceContainer &renderer,
        Camera &camera, LightContainer &lights, PhysWorld &physics,
        std::string scene_id, AuthoringObjectId target_object,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        SceneObjectProjectionParticipants participants);
    SceneObjectProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        ModelAssetContainer *models, PolygonInstanceContainer *renderer,
        Camera *camera, LightContainer *lights, PhysWorld *physics,
        std::string scene_id, AuthoringObjectId target_object,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        SceneObjectProjectionParticipants participants);
    // Production lifecycle core for headless/runtime tests whose scene has no
    // renderer, camera, light, model, or collider projection.  It executes the
    // same staged ECS mutation used by the full adapter and deliberately fails
    // if a render/physics-owned component is encountered.
    SceneObjectProjectionAdapter(
        ECSCoreTemplatePublic &ecs, ComponentInfoManager &components,
        std::string scene_id, AuthoringObjectId target_object,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings);
    ~SceneObjectProjectionAdapter();
    void connectRuntimeBindings(SceneLoader &loader) noexcept;

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
        std::vector<SceneObjectProjectionAdapter *> lifecycle_adapters);
    BehaviorSceneProjectionAdapter(
        BehaviorAttachmentArena &arena, std::string scene_id,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        std::vector<SceneObjectProjectionAdapter *> lifecycle_adapters,
        OriginTransitionMap origin_transition,
        BehaviorRegistryAvailability availability);
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
        std::vector<EditorProjectionRuntimeObjectBinding> bindings,
        std::vector<ComponentCodec> codecs);
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

// Shared skeleton for the projections whose whole publication is one prepared
// state swap on a single module: prepare snapshots the live state and stages
// the next one, publish swaps the next one in, rollback swaps the snapshot
// back when publication already happened, and finish drops both. Policy is
// defined in the adapter translation unit and supplies the module type, the
// prepared state type, the adapter kind, the publication mode, the adapter
// name prefix, prepare() and any extra per-adapter state; the prepared state
// therefore stays behind Impl and this header stays free of the camera,
// light and physics module definitions.
template <class Policy>
class SnapshotProjectionAdapter : public EditorProjectionAdapter {
  public:
    struct Impl;

    ~SnapshotProjectionAdapter() override;

    EditorProjectionAdapterKind kind() const noexcept override;
    std::string_view name() const noexcept override;
    EditorProjectionPublicationMode publicationMode() const noexcept override;
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;

  protected:
    explicit SnapshotProjectionAdapter(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

struct CameraProjectionPolicy;
struct LightProjectionPolicy;
struct ColliderProjectionPolicy;

extern template class SnapshotProjectionAdapter<CameraProjectionPolicy>;
extern template class SnapshotProjectionAdapter<LightProjectionPolicy>;
extern template class SnapshotProjectionAdapter<ColliderProjectionPolicy>;

class CameraProjectionAdapter final
    : public SnapshotProjectionAdapter<CameraProjectionPolicy> {
  public:
    CameraProjectionAdapter(Camera &camera, std::string scene_id);
};

class LightProjectionAdapter final
    : public SnapshotProjectionAdapter<LightProjectionPolicy> {
  public:
    LightProjectionAdapter(LightContainer &lights, std::string scene_id);
};

class ColliderProjectionAdapter final
    : public SnapshotProjectionAdapter<ColliderProjectionPolicy> {
  public:
    ColliderProjectionAdapter(
        ECSCoreTemplatePublic &ecs, PhysWorld &physics, std::string scene_id,
        std::vector<EditorProjectionRuntimeObjectBinding> bindings);
};

EditorProjectionCallbackAdapter makeBehaviorAttachmentProjectionJunction(
    std::string name, void *context,
    EditorProjectionCallbackAdapter::Prepare prepare,
    EditorProjectionCallbackAdapter::NoexceptAction publish,
    EditorProjectionCallbackAdapter::NoexceptAction rollback,
    EditorProjectionCallbackAdapter::NoexceptAction finish) noexcept;

} // namespace Pelican
