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
    void swap(AuthoringSceneDocument &other) noexcept;

  private:
    friend class ProjectBasicConfig;
    friend class ProjectBasicConfigProjectionTarget;
};

} // namespace Pelican
