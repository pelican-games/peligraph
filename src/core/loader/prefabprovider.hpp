#pragma once

#include "../../project/prefab.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

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
    bool bindable = true;
    bool required = false;
    std::optional<nlohmann::ordered_json> canonical_default;
    Schema::FieldRange range;
    std::vector<std::string> enum_values;
    std::vector<BindableDiscriminant> applicability;
};

class BindableProviderSnapshot {
    struct Data;
    std::shared_ptr<const Data> data_;

  public:
    BindableProviderSnapshot();
    explicit BindableProviderSnapshot(std::uint64_t generation,
                                      std::vector<BindableDescriptor> descriptors);
    std::uint64_t generation() const noexcept;
    std::string_view fingerprint() const noexcept;
    std::span<const BindableDescriptor> descriptors() const noexcept;

    // A missing path is not bindable. A present path whose discriminants do
    // not match is inactive. Type/kind compatibility is checked separately so
    // callers can report the parameter in the error context.
    const BindableDescriptor *find(std::string_view provider_name,
                                   std::string_view json_pointer) const noexcept;
    const BindableDescriptor *findActive(
        std::string_view provider_name, std::string_view json_pointer,
        const nlohmann::json &component) const;
    bool containsPath(std::string_view provider_name,
                      std::string_view json_pointer) const noexcept;
    bool active(const BindableDescriptor &descriptor,
                const nlohmann::json &component) const;
};

BindableProviderSnapshot buildProductionBindableProviderSnapshot(
    std::uint64_t generation = 1);

bool prefabParameterMatchesDescriptor(
    const PrefabParameterDeclaration &parameter,
    const BindableDescriptor &descriptor) noexcept;

} // namespace Pelican
