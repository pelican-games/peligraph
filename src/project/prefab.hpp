#pragma once

#include "../schema/schema.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class PrefabErrorCode : std::uint8_t {
    PrefabNotFound,
    PrefabVersionUnsupported,
    PrefabRequiresSceneV2,
    PrefabDuplicateName,
    PrefabParameterUnknown,
    PrefabParameterDuplicate,
    PrefabParameterType,
    PrefabParameterUnused,
    PrefabBindingNotBindable,
    PrefabBindingInactive,
    PrefabObjectRefUnresolved,
    PrefabObjectRefMissingComponent,
    PrefabTransformForbidden,
    PrefabDependencyMismatch,
    PrefabNestedUnsupported,
    PrefabProviderStale,
    PrefabParameterRequired,
    PrefabInstanceIdCollision,
    PrefabGeneratedReadOnly,
    PrefabGeneratedIdCollision,
    PrefabInstanceTransformMissing,
    PrefabInstanceTransformDuplicate,
    PrefabComponentDuplicate,
    PrefabNameMismatch,
    PrefabPersistenceUnsupported,
    PrefabPathInvalid,
    PrefabRegistryInvalid,
    PrefabDocumentInvalid,
    PrefabInstanceInvalid,
    PrefabInstanceIdInvalid,
    PrefabComponentKeyDuplicate,
};

std::string_view prefabErrorCodeName(PrefabErrorCode code) noexcept;

struct PrefabErrorContext {
    std::optional<std::string> scene_id;
    std::optional<std::string> instance_id;
    std::optional<std::string> prefab;
    std::optional<std::string> parameter;
    std::optional<std::string> json_pointer;
    std::optional<std::size_t> line_index;
};

class PrefabError : public std::runtime_error {
    PrefabErrorCode code_;
    PrefabErrorContext context_;

  public:
    PrefabError(PrefabErrorCode code, std::string message,
                PrefabErrorContext context = {});
    PrefabErrorCode code() const noexcept { return code_; }
    const PrefabErrorContext &context() const noexcept { return context_; }
};

struct PrefabRegistryEntry {
    std::string name;
    std::string path;
};

std::vector<PrefabRegistryEntry>
parsePrefabRegistryEntries(const nlohmann::json &value);
bool isPrefabIdentifier(std::string_view value) noexcept;

enum class PrefabParameterKind : std::uint8_t { Value, Asset, Object };

struct PrefabParameterDeclaration {
    std::string name;
    PrefabParameterKind kind = PrefabParameterKind::Value;
    Schema::FieldDeclaration value_schema;
    std::string asset_kind;
    std::optional<nlohmann::json> default_value;
    std::vector<std::string> required_components;
};

struct PrefabTaggedBinding {
    std::string json_pointer;
    std::string parameter;
};

struct PrefabComponentDocument {
    std::string name;
    std::string key;
    nlohmann::json body;
    std::vector<PrefabTaggedBinding> bindings;
};

struct PrefabDocument {
    std::string name;
    std::vector<PrefabParameterDeclaration> parameters;
    std::vector<PrefabComponentDocument> components;
};

PrefabDocument parsePrefabDocumentJson(const nlohmann::json &value,
                                       std::string_view expected_name = {});
PrefabDocument parsePrefabDocumentText(std::string_view bytes,
                                       std::string_view expected_name = {});

struct PrefabRegistryRecord {
    std::string name;
    std::string path;
    std::string digest_sha256;
    std::shared_ptr<const PrefabDocument> document;
};

class PrefabRegistrySnapshot {
    struct Data;
    std::shared_ptr<const Data> data_;

    explicit PrefabRegistrySnapshot(std::shared_ptr<const Data> data);
    friend PrefabRegistrySnapshot buildPrefabRegistrySnapshot(
        std::span<const PrefabRegistryEntry>,
        const std::function<std::string(std::string_view)> &, std::uint64_t);

  public:
    PrefabRegistrySnapshot();
    std::uint64_t generation() const noexcept;
    std::span<const PrefabRegistryRecord> records() const noexcept;
    const PrefabRegistryRecord *find(std::string_view name) const noexcept;
    bool empty() const noexcept;
};

PrefabRegistrySnapshot buildPrefabRegistrySnapshot(
    std::span<const PrefabRegistryEntry> entries,
    const std::function<std::string(std::string_view)> &load_raw_bytes,
    std::uint64_t generation = 1);

struct PrefabResolvedParameter {
    std::string name;
    nlohmann::ordered_json value_resolved;
    bool is_override = false;
    std::optional<nlohmann::json> value_authored;
    PrefabParameterKind kind = PrefabParameterKind::Value;
    std::vector<std::string> required_components;
};

struct PrefabInstanceDeclaration {
    std::string ref;
    std::string instance_id;
    std::vector<PrefabResolvedParameter> parameters;
};

PrefabInstanceDeclaration resolvePrefabInstance(
    const nlohmann::json &prefab_block, const PrefabRegistrySnapshot &registry,
    PrefabErrorContext context = {});

nlohmann::ordered_json substitutePrefabComponent(
    const PrefabComponentDocument &component,
    std::span<const PrefabResolvedParameter> parameters);

// Stable generated ids are byte-defined by WP361 v4. The component key is
// component.id when present, otherwise component.name.
std::string prefabGeneratedId(std::string_view instance_id,
                              std::string_view component_key);

} // namespace Pelican
