#pragma once

#include "authoringscenedocument.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../userpublic/components/localtransform.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

class ProjectBasicConfig;

enum class EditorProjectionAdapterKind : std::uint8_t {
    EcsExistingValue,
    EcsArchetype,
    TransformClosure,
    RendererModel,
    Camera,
    Light,
    PhysWorld,
    BehaviorAttachment,
};

std::string_view editorProjectionAdapterKindName(
    EditorProjectionAdapterKind kind) noexcept;

enum class EditorProjectionPublicationMode : std::uint8_t {
    StagedNoexcept,
    InverseToken,
};

enum class EditorProjectionErrorCode : std::uint8_t {
    StaleRevision,
    CommandInvalid,
    ObjectNotFound,
    ComponentNotFound,
    CodecMissing,
    TransformCycle,
    TransformZeroParentScale,
    TransformNonFinite,
    TransformUnrepresentable,
    AdapterPrepareFailed,
    AdapterPublishFailed,
};

std::string_view editorProjectionErrorCodeName(
    EditorProjectionErrorCode code) noexcept;

class EditorProjectionException : public std::runtime_error {
    EditorProjectionErrorCode code_;
    std::string object_path_;

  public:
    EditorProjectionException(EditorProjectionErrorCode code,
                              std::string object_path,
                              std::string message);

    EditorProjectionErrorCode code() const noexcept { return code_; }
    const std::string &objectPath() const noexcept { return object_path_; }
};

struct EditorProjectionError {
    EditorProjectionErrorCode code = EditorProjectionErrorCode::CommandInvalid;
    std::string object_path;
    std::string message;
};

enum class EditorProjectionStatus : std::uint8_t {
    Committed,
    Rejected,
    Failed,
};

struct EditorProjectionResult {
    EditorProjectionStatus status = EditorProjectionStatus::Rejected;
    SceneRevision base_revision{};
    SceneRevision committed_revision{};
    std::optional<EditorProjectionError> error;

    bool committed() const noexcept {
        return status == EditorProjectionStatus::Committed;
    }
};

// ProjectBasicConfig and tests provide this small publication boundary. The
// staged document has already been fully validated when publish is called.
class EditorProjectionDocumentTarget {
  public:
    virtual ~EditorProjectionDocumentTarget() = default;
    virtual const AuthoringSceneDocument &projectionDocument() const = 0;
    virtual SceneRevision nextProjectionRevision() const = 0;
    virtual void publishProjectionDocument(
        AuthoringSceneDocument &&document) noexcept = 0;
};

// Keeps ProjectBasicConfig's widely included public header free of editor
// transaction dependencies while still publishing through its single scene
// cache authority.
class ProjectBasicConfigProjectionTarget final
    : public EditorProjectionDocumentTarget {
    ProjectBasicConfig &config_;

  public:
    explicit ProjectBasicConfigProjectionTarget(ProjectBasicConfig &config) noexcept
        : config_(config) {}

    const AuthoringSceneDocument &projectionDocument() const override;
    SceneRevision nextProjectionRevision() const override;
    void publishProjectionDocument(
        AuthoringSceneDocument &&document) noexcept override;
};

struct EditorProjectionCommand {
    using Apply = std::function<void(nlohmann::json &)>;

    std::string object_path;
    Apply apply;
};

enum class ReparentPreserve : std::uint8_t {
    Local,
    World,
};

EditorProjectionCommand makeSetComponentValueCommand(
    std::string scene_id, std::string object_name,
    std::string component_name, nlohmann::json authored_component);

EditorProjectionCommand makeReparentCommand(
    std::string scene_id, std::string object_name,
    std::optional<std::string> new_parent, ReparentPreserve preserve);

struct EditorProjectionPrepareContext {
    const AuthoringSceneDocument &base_document;
    const AuthoringSceneDocument &next_document;
};

class EditorProjectionAdapter {
  public:
    virtual ~EditorProjectionAdapter() = default;
    virtual EditorProjectionAdapterKind kind() const noexcept = 0;
    virtual std::string_view name() const noexcept = 0;
    virtual EditorProjectionPublicationMode publicationMode() const noexcept = 0;
    virtual void prepare(const EditorProjectionPrepareContext &context) = 0;
    virtual void publish() noexcept = 0;
    // Before publish this discards prepared state. After publish it restores
    // the complete inverse token. Both paths must be allocation-free.
    virtual void rollback() noexcept = 0;
    virtual void finish() noexcept = 0;
};

enum class EditorProjectionFaultPoint : std::uint8_t {
    Prepare,
    Publish,
};

class EditorProjectionFaultInjector {
  public:
    virtual ~EditorProjectionFaultInjector() = default;
    virtual bool shouldFail(EditorProjectionFaultPoint point,
                            std::string_view adapter_name) noexcept = 0;
};

// A typed callback junction for renderer/model, camera, behavior and the
// WP152 ECS migration participant. The no-throw callbacks make it impossible
// for those publication edges to weaken the aggregate protocol.
class EditorProjectionCallbackAdapter final : public EditorProjectionAdapter {
  public:
    using Prepare = void (*)(void *, const EditorProjectionPrepareContext &);
    using NoexceptAction = void (*)(void *) noexcept;

  private:
    EditorProjectionAdapterKind kind_;
    std::string name_;
    EditorProjectionPublicationMode mode_;
    void *context_ = nullptr;
    Prepare prepare_ = nullptr;
    NoexceptAction publish_ = nullptr;
    NoexceptAction rollback_ = nullptr;
    NoexceptAction finish_ = nullptr;

  public:
    EditorProjectionCallbackAdapter(
        EditorProjectionAdapterKind kind, std::string name,
        EditorProjectionPublicationMode mode, void *context, Prepare prepare,
        NoexceptAction publish, NoexceptAction rollback,
        NoexceptAction finish) noexcept;

    EditorProjectionAdapterKind kind() const noexcept override { return kind_; }
    std::string_view name() const noexcept override { return name_; }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return mode_;
    }
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

struct TransformProjectionBinding {
    std::string scene_id;
    std::string object_name;
    EntityId entity = invalidEntityId;
    TransformComponent *world = nullptr;
    LocalTransformComponent *local = nullptr;
};

// The concrete E-C2 participant. It resolves authored local TRS for the whole
// hierarchy during prepare, computes descendant world closure with the same
// recurrence as LocalTransformSystem, then copies only prepared values during
// the observer-free publication interval.
class TransformProjectionAdapter final : public EditorProjectionAdapter {
    struct PreparedValue {
        TransformProjectionBinding *binding = nullptr;
        TransformComponent old_world{};
        LocalTransformComponent old_local{};
        TransformComponent next_world{};
        LocalTransformComponent next_local{};
    };

    std::vector<TransformProjectionBinding> bindings_;
    std::vector<PreparedValue> prepared_;
    bool published_ = false;

  public:
    explicit TransformProjectionAdapter(
        std::vector<TransformProjectionBinding> bindings);

    EditorProjectionAdapterKind kind() const noexcept override {
        return EditorProjectionAdapterKind::TransformClosure;
    }
    std::string_view name() const noexcept override {
        return "transform_closure";
    }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return EditorProjectionPublicationMode::StagedNoexcept;
    }
    void prepare(const EditorProjectionPrepareContext &context) override;
    void publish() noexcept override;
    void rollback() noexcept override;
    void finish() noexcept override;
};

class EditorProjectionTransaction {
    EditorProjectionDocumentTarget &document_target_;
    SceneRevision expected_base_revision_;
    EditorProjectionFaultInjector *fault_injector_ = nullptr;

  public:
    EditorProjectionTransaction(
        EditorProjectionDocumentTarget &document_target,
        SceneRevision expected_base_revision,
        EditorProjectionFaultInjector *fault_injector = nullptr) noexcept;

    EditorProjectionResult commit(
        std::span<const EditorProjectionCommand> commands,
        std::span<EditorProjectionAdapter *const> adapters) noexcept;
};

} // namespace Pelican
