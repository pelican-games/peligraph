#pragma once

#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/render/draw_sort_abi_v1.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view builtinStateBatchedDrawSortProvider =
    "state_batched_v1";

struct DrawSortProviderInfo {
    std::string name;
    RenderPolicy::ProviderHandleV1 handle{};
    internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    std::uint32_t provider_version = 0;
    std::uint64_t capability_bits = 0;

    bool operator==(const DrawSortProviderInfo &) const = default;
};

class DrawSortProviderLease {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    DrawSortProviderInfo info_;

    DrawSortProviderLease(std::unique_ptr<Impl> impl,
                          DrawSortProviderInfo info) noexcept;
    friend class RenderPolicyRegistry;

  public:
    DrawSortProviderLease() noexcept;
    ~DrawSortProviderLease();
    DrawSortProviderLease(DrawSortProviderLease &&) noexcept;
    DrawSortProviderLease &operator=(DrawSortProviderLease &&) noexcept;
    DrawSortProviderLease(const DrawSortProviderLease &) = delete;
    DrawSortProviderLease &operator=(const DrawSortProviderLease &) = delete;

    explicit operator bool() const noexcept { return impl_ != nullptr; }
    const DrawSortProviderInfo &info() const noexcept { return info_; }

    RenderPolicy::Status
    sort(const RenderPolicy::DrawSortInputV1 &input,
         std::span<RenderPolicy::DrawSortKeyV1> output,
         std::uint32_t &out_count) const noexcept;
};

class RenderPolicyRegistry {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    RenderPolicyRegistry();
    ~RenderPolicyRegistry();
    RenderPolicyRegistry(const RenderPolicyRegistry &) = delete;
    RenderPolicyRegistry &operator=(const RenderPolicyRegistry &) = delete;

    RenderPolicy::Status
    registerDrawSortProvider(const RenderPolicy::ProviderV1 &provider,
                             internal::RegistrationOwner owner,
                             RenderPolicy::ProviderHandleV1 &out_handle) noexcept;
    RenderPolicy::Status
    unregisterDrawSortProvider(RenderPolicy::ProviderHandleV1 handle,
                               internal::RegistrationOwner owner) noexcept;

    DrawSortProviderLease
    resolveDrawSortProvider(std::string_view name) const;

    // Candidate registrations stay invisible until their game-DLL owner is
    // committed. Releasing an owner takes the exclusive registry lock and
    // therefore waits for every outstanding provider lease/callback.
    void activateOwner(internal::RegistrationOwner owner) noexcept;
    void releaseOwner(internal::RegistrationOwner owner) noexcept;
};

RenderPolicyRegistry &renderPolicyRegistry();

namespace render_policy_internal {
void activateProviderOwner(internal::RegistrationOwner owner) noexcept;
void releaseProviderOwner(internal::RegistrationOwner owner) noexcept;
}

} // namespace Pelican
