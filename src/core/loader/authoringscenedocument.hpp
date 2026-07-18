#pragma once

#include "../handle.hpp"
#include "../userpublic/details/ecs/entity.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "componentcodec.hpp"

namespace Pelican {

class ProjectBasicConfigProjectionTarget;

PELICAN_DEFINE_HANDLE(SceneRevision, std::uint64_t)
PELICAN_DEFINE_HANDLE(AuthoringObjectId, std::uint64_t)

struct AuthoringComponentView {
    std::size_t declaration_index = 0;
    const nlohmann::json *authored_json = nullptr;
    ComponentCodecQueryMetadata codec;

    const nlohmann::json &authoredJson() const { return *authored_json; }
};

struct AuthoringObjectView {
    AuthoringObjectId authoring_object_id{};
    std::size_t declaration_index = 0;
    std::optional<std::string> name;
    std::optional<std::string> parent;
    std::optional<GameObjectId> runtime_entity_id;
    const nlohmann::json *authored_json = nullptr;
    std::vector<AuthoringComponentView> components;

    const nlohmann::json &authoredJson() const { return *authored_json; }
};

struct AuthoringSceneView {
    std::string scene_id;
    const nlohmann::json *authored_json = nullptr;
    std::vector<AuthoringObjectView> objects;

    const nlohmann::json &authoredJson() const { return *authored_json; }
};

struct AuthoringObjectClosure {
    AuthoringObjectId authoring_object_id{};
    std::string scene_id;
    std::size_t declaration_index = 0;
    std::optional<AuthoringObjectId> previous_object_id;
    std::optional<AuthoringObjectId> next_object_id;
    nlohmann::json authored_json;
};

enum class AuthoringStructuralChangeKind : std::uint8_t {
    Insert,
    Remove,
    Restore,
    Rename,
    Reorder,
};

struct AuthoringStructuralChange {
    AuthoringStructuralChangeKind kind = AuthoringStructuralChangeKind::Insert;
    AuthoringObjectId authoring_object_id{};
    std::string scene_id;
    std::size_t previous_declaration_index = 0;
    std::size_t declaration_index = 0;
};

class AuthoringSceneDocumentStage;

class AuthoringSceneDocument {
    struct ObjectMetadata {
        AuthoringObjectId id{};
    };

    struct SceneMetadata {
        std::string id;
        std::vector<ObjectMetadata> objects;
    };

    SceneRevision revision_{};
    nlohmann::json raw_document_;
    std::vector<std::string> warnings_;
    std::vector<SceneMetadata> scene_metadata_;
    std::uint64_t next_authoring_object_id_value_ = 1;

  public:
    static AuthoringSceneDocument load(std::string_view scene_v1_bytes, SceneRevision revision,
                                       std::uint64_t first_authoring_object_id = 1);

    SceneRevision revision() const noexcept { return revision_; }
    std::size_t objectCount() const noexcept;
    const nlohmann::json &rawJson() const noexcept { return raw_document_; }
    const nlohmann::json &scenesJson() const { return raw_document_.at("scenes"); }
    std::span<const std::string> warnings() const noexcept { return warnings_; }

    std::vector<AuthoringSceneView> query() const;
    std::string encodeSemantic() const;

    // Build an unpublished revision while retaining the session-stable object
    // identities of this document. Projection commands may change component
    // arrays and parent edges, but object declaration identity is deliberately
    // fixed until the later spawn/destroy work package.
    AuthoringSceneDocument stage(nlohmann::json raw_document,
                                 SceneRevision revision) const;
    AuthoringSceneDocumentStage structuralStage() const;
    void swap(AuthoringSceneDocument &other) noexcept;

  private:
    friend class ProjectBasicConfig;
    friend class ProjectBasicConfigProjectionTarget;
    friend class AuthoringSceneDocumentStage;
};

// An unpublished structural edit of one AuthoringSceneDocument. Object-array
// mutations are deliberately available only through these operations so the
// metadata identity sequence cannot drift from the authored JSON sequence.
// Destruction returns a lossless closure whose restore operation rebinds the
// same AuthoringObjectId inside the saved declaration interval.
class AuthoringSceneDocumentStage {
    AuthoringSceneDocument base_;
    std::vector<AuthoringStructuralChange> changes_;
    std::vector<AuthoringObjectClosure> removed_objects_;

    explicit AuthoringSceneDocumentStage(const AuthoringSceneDocument &base);
    std::pair<std::size_t, std::size_t>
    requireObjectLocation(AuthoringObjectId object_id) const;
    std::size_t requireSceneIndex(std::string_view scene_id) const;
    bool containsObjectId(AuthoringObjectId object_id) const noexcept;
    std::optional<std::size_t>
    findObjectIndex(std::size_t scene_index,
                    AuthoringObjectId object_id) const noexcept;
    friend class AuthoringSceneDocument;

  public:
    nlohmann::json &rawJson() noexcept { return base_.raw_document_; }
    const nlohmann::json &rawJson() const noexcept { return base_.raw_document_; }
    std::span<const AuthoringStructuralChange> changes() const noexcept {
        return changes_;
    }
    std::span<const AuthoringObjectClosure> removedObjects() const noexcept {
        return removed_objects_;
    }

    AuthoringObjectId insertObject(std::string_view scene_id,
                                   std::size_t declaration_index,
                                   nlohmann::json authored_object);
    AuthoringObjectClosure removeObject(AuthoringObjectId object_id);
    void restoreObject(const AuthoringObjectClosure &closure);
    void renameObject(AuthoringObjectId object_id,
                      std::optional<std::string> name);
    void reorderObject(AuthoringObjectId object_id,
                       std::size_t declaration_index);

    AuthoringSceneDocument finish(SceneRevision revision) &&;
};

} // namespace Pelican
