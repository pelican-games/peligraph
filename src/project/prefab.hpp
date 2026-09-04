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

// Provider descriptors live in pelican_project so prefab expansion has one
// side-effect-free leaf shared by the engine and offline Studio. The runtime
// provider snapshot adds generation/fingerprint and registered behavior
// descriptors, but does not own binding semantics.
enum class BindableSemanticKind : std::uint8_t { Value, Asset, Object };

struct BindableDiscriminant {
    std::string json_pointer;
    std::vector<nlohmann::ordered_json> active_values;
};

struct BindableDescriptor {
    std::string provider_name;
    std::string json_pointer;
    Schema::FieldType type = Schema::FieldType::String;
    BindableSemanticKind semantic_kind = BindableSemanticKind::Value;
    // Asset bindings are compatible only when both sides name the same
    // subtype (for example "model" or "texture").
    std::string asset_kind;
    bool bindable = true;
    bool required = false;
    std::optional<nlohmann::ordered_json> canonical_default;
    Schema::FieldRange range;
    std::vector<std::string> enum_values;
    std::vector<BindableDiscriminant> applicability;
    std::optional<Schema::Node> schema_node;
};

PrefabInstanceDeclaration resolvePrefabInstance(
    const nlohmann::json &prefab_block, const PrefabRegistrySnapshot &registry,
    PrefabErrorContext context = {});

nlohmann::ordered_json substitutePrefabComponent(
    const PrefabComponentDocument &component,
    std::span<const PrefabResolvedParameter> parameters);

// Canonical built-in codec descriptors used by both production provider
// construction and Studio's offline expansion. Runtime codec schemas may
// enrich type/range/enum metadata without changing the binding paths.
std::vector<BindableDescriptor> builtinPrefabBindableDescriptors();

bool prefabParameterMatchesDescriptor(
    const PrefabParameterDeclaration &parameter,
    const BindableDescriptor &descriptor) noexcept;
bool prefabDescriptorActive(const BindableDescriptor &descriptor,
                            const nlohmann::json &component);

// Validate provider binding rules and return the concrete component JSON.
// This is the only prefab substitution path used by engine and Studio.
nlohmann::ordered_json resolvePrefabComponentBody(
    const PrefabDocument &document,
    const PrefabComponentDocument &component,
    const PrefabInstanceDeclaration &instance,
    std::span<const BindableDescriptor> descriptors,
    PrefabErrorContext context = {});

// The single production-owned classifier for root fields which request a
// generated component.  The resolver rejects those fields on authored
// components; unpack removes exactly the fields classified here.
bool prefabGeneratedMarkerRule(std::string_view field,
                               const nlohmann::json &value);

// Stable generated ids are byte-defined by WP361 v4. The component key is
// component.id when present, otherwise component.name.
std::string prefabGeneratedId(std::string_view instance_id,
                              std::string_view component_key);

} // namespace Pelican
