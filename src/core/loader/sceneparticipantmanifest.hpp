#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace Pelican {

// How a participant exists in this build. `real` is a module the build has.
// For a module the build left out, sceneFeatureDivisions() decides between
// the other two by one rule: `stub` when something in a scene can still ask
// for the module (the OFF build keeps the part that refuses it by name),
// `absent` when nothing can.
enum class SceneParticipantPresence : std::uint8_t {
    absent,
    stub,
    real,
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

// PG-2: one row per build module that can own part of a scene load, with the
// presence its participant rows take in a build without it.
struct SceneFeatureDivision {
    // The module's name: its `requires` word where it has one.
    std::string_view feature;
    // The build option, e.g. PELICAN_WITH_PHYSICS.
    std::string_view option;
    bool enabled = false;
    // What can still reach a scene in a build without the module, and what
    // refuses it there. Empty when nothing can.
    std::string_view off_declaration;
    // stub exactly when off_declaration is not empty; absent otherwise.
    SceneParticipantPresence off_presence = SceneParticipantPresence::absent;

    constexpr SceneParticipantPresence presence() const noexcept {
        return enabled ? SceneParticipantPresence::real : off_presence;
    }
};

std::span<const SceneParticipantManifestEntry>
sceneParticipantManifest() noexcept;
std::span<const SceneFeatureDivision> sceneFeatureDivisions() noexcept;
std::span<const ScenePublicApiManifestEntry>
scenePublicApiManifest() noexcept;

std::string_view sceneParticipantPresenceName(
    SceneParticipantPresence presence) noexcept;
std::string_view sceneManifestRoleName(SceneManifestRole role) noexcept;

} // namespace Pelican
