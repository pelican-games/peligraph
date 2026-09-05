#pragma once

#include "polygonvertdata.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// A project-table logical name. Physical paths, URIs, and ModelAssetId are
// deliberately different types and cannot accidentally enter occurrence v1.
struct CanonicalModelAssetKey {
    std::string logical_name;
    bool operator==(const CanonicalModelAssetKey &) const = default;
};

enum class ImporterVariantV1 : std::uint8_t {
    none = 0,
    vrm_split_both = 1,
    vrm_split_third_person_only = 2,
    lod = 3,
    skinned = 4,
    morph_expanded = 5,
    vat_deformed = 6,
};

struct ImporterVariant {
    ImporterVariantV1 kind = ImporterVariantV1::none;
    std::uint8_t lod = 0;
    bool operator==(const ImporterVariant &) const = default;
};

enum class OccurrenceResolutionErrorCode : std::uint8_t {
    absent,
    ambiguous,
    not_ray_traced,
    id_unrepresentable,
};

std::string_view occurrenceResolutionErrorCodeName(
    OccurrenceResolutionErrorCode code) noexcept;

struct OccurrenceIdentityError {
    OccurrenceResolutionErrorCode code =
        OccurrenceResolutionErrorCode::id_unrepresentable;
    std::string reason;
    bool operator==(const OccurrenceIdentityError &) const = default;
};

using OccurrenceDigest = std::array<std::uint8_t, 32>;

struct OccurrenceIdentityInput {
    std::optional<CanonicalModelAssetKey> asset;
    std::optional<std::string> node_explicit_id;
    std::vector<std::string> node_name_path;
    bool node_name_path_unique = false;
    OccurrenceDigest node_content_digest{};
    std::optional<std::string> primitive_explicit_id;
    OccurrenceDigest primitive_content_digest{};
    ImporterVariant variant{};
};

struct OccurrenceIdentityResult {
    std::string occurrence_id;
    std::optional<OccurrenceIdentityError> error;
};

struct ModelOccurrenceIdentityInfo {
    std::optional<CanonicalModelAssetKey> asset;
    std::vector<std::string> node_name_path;
    std::vector<std::uint32_t> diagnostic_parent_path;
    std::optional<std::string> node_explicit_id;
    std::optional<std::string> primitive_explicit_id;
    OccurrenceDigest primitive_content_digest{};
    OccurrenceDigest node_content_digest{};
    ImporterVariant variant{};
    std::string occurrence_id;
    std::optional<OccurrenceIdentityError> error;
    bool ambiguous = false;
};

enum class RayOccurrenceEligibilityKind : std::uint8_t {
    ok,
    not_ray_traced,
    ambiguous,
    unrepresentable,
};

struct RayOccurrenceCatalogEntry {
    std::string occurrence_id;
    std::string display_name;
    std::string asset;
    std::vector<std::string> node_path;
    std::uint32_t mesh = 0;
    std::uint32_t primitive = 0;
    std::string material;
    std::string variant;
    RayOccurrenceEligibilityKind eligibility =
        RayOccurrenceEligibilityKind::ok;
    std::string reason;
    std::size_t q = 0;
    std::uint64_t authoring_object_id = 0;
};

enum class RayOccurrenceResolveState : std::uint8_t {
    found,
    absent,
    ambiguous,
};

struct RayOccurrenceResolution {
    RayOccurrenceResolveState state = RayOccurrenceResolveState::absent;
    std::vector<RayOccurrenceCatalogEntry> candidates;
};

// The only occurrence_id_v1 encoder. Every scalar below is field framed as
// F(x)=u32le(byte_count)||bytes and every float is canonical IEEE f32.
OccurrenceIdentityResult occurrenceIdV1(
    const OccurrenceIdentityInput &input);

OccurrenceDigest primitiveContentDigestV1(
    std::uint8_t topology_mode, const CommonPolygonVertData &data);
OccurrenceDigest nodeContentDigestV1(
    std::span<const OccurrenceDigest> primitive_digests);

// Returns normalized UTF-8 or id_unrepresentable. Kept public so glTF extras
// and logical asset names use exactly the same normalization gate.
std::optional<std::string> occurrenceNfcUtf8(
    std::string_view value, OccurrenceIdentityError &error);

std::string importerVariantV1Name(const ImporterVariant &variant);
void detectOccurrenceIdCollisions(
    std::span<const std::shared_ptr<ModelOccurrenceIdentityInfo>> entries);

} // namespace Pelican
