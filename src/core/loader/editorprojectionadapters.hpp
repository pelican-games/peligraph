#pragma once

#include "editorprojectiontransaction.hpp"

#include <details/ecs/entity.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Pelican {

class Camera;
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
