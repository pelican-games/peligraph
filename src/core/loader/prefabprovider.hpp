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

struct BehaviorDependencyDescriptor {
    std::string stable_name;
    std::uint32_t schema_version = 0;
    std::string params_schema_fingerprint;

    friend bool operator==(const BehaviorDependencyDescriptor &,
                           const BehaviorDependencyDescriptor &) = default;
};

class BindableProviderSnapshot {
    struct Data;
    std::shared_ptr<const Data> data_;

  public:
    BindableProviderSnapshot();
    explicit BindableProviderSnapshot(std::uint64_t generation,
                                      std::vector<BindableDescriptor> descriptors,
                                      std::vector<BehaviorDependencyDescriptor>
                                          behavior_dependencies = {});
    std::uint64_t generation() const noexcept;
    std::string_view fingerprint() const noexcept;
    std::span<const BindableDescriptor> descriptors() const noexcept;
    std::span<const BehaviorDependencyDescriptor> behaviorDependencies() const
        noexcept;

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

} // namespace Pelican
