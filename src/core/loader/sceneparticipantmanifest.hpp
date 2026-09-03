#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace Pelican {

enum class SceneParticipantPresence : std::uint8_t {
    absent,
    stub,
    real,
    uninstalled,
};

enum class SceneManifestRole : std::uint8_t {
    participant,
    invariant,
    cache,
    production_root,
    excluded_wp380b,
    effect,
    non_rollback,
    reject,
    frame_lifetime,
    read_only,
};

// One row describes one independently observable field (or one build root).
// Keeping the mutation id beside the field makes the WP380 drop/bypass
// controls enumerable instead of relying on one aggregate transaction test.
struct SceneParticipantManifestEntry {
    std::string_view owner;
    std::string_view field;
    SceneParticipantPresence presence =
        SceneParticipantPresence::absent;
    SceneManifestRole role = SceneManifestRole::participant;
    std::string_view prepare_owner;
    std::string_view publication;
    std::string_view rollback;
    std::string_view retirement;
    std::string_view snapshot_schema;
    std::string_view mutation_id;
};

struct ScenePublicApiManifestEntry {
    std::string_view class_name;
    std::string_view signature;
    std::string_view classification;
};

std::span<const SceneParticipantManifestEntry>
sceneParticipantManifest() noexcept;
std::span<const ScenePublicApiManifestEntry>
scenePublicApiManifest() noexcept;

std::string_view sceneParticipantPresenceName(
    SceneParticipantPresence presence) noexcept;
std::string_view sceneManifestRoleName(SceneManifestRole role) noexcept;

} // namespace Pelican
