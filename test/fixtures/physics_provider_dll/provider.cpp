#include <physics/abi_v1.hpp>

#include <atomic>
#include <cstdint>

#ifdef _WIN32
#define PELICAN_FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define PELICAN_FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#ifndef PELICAN_PHYSICS_FIXTURE_VERSION
#define PELICAN_PHYSICS_FIXTURE_VERSION 1
#endif

namespace {

using namespace Pelican;

#if PELICAN_PHYSICS_FIXTURE_VERSION == 1
constexpr char provider_name[] = "fixture.physics.v1";
constexpr float provider_distance = 0.625F;
#elif PELICAN_PHYSICS_FIXTURE_VERSION == 2
constexpr char provider_name[] = "fixture.physics.v2";
constexpr float provider_distance = 1.375F;
#else
#error Unsupported PELICAN_PHYSICS_FIXTURE_VERSION
#endif

std::atomic_bool block_next_raycast{false};
std::atomic_bool raycast_entered{false};
std::atomic_bool resume_raycast{false};
std::atomic<std::uint32_t> registration_status{
    static_cast<std::uint32_t>(Physics::Status::unavailable)};

Physics::Status raycastAll(void *, const Physics::ProviderRaycastQueryV1 *query,
                           Physics::ProviderRaycastHitV1 *hits,
                           std::uint32_t capacity,
                           std::uint32_t *out_count) noexcept {
    if (query == nullptr || out_count == nullptr || query->reserved != 0 ||
        query->ray.reserved != 0 ||
        (query->collider_count != 0 && query->colliders == nullptr) ||
        (capacity != 0 && hits == nullptr)) {
        return Physics::Status::invalid_argument;
    }

    if (query->collider_count == 0) {
        *out_count = 0;
        return Physics::Status::ok;
    }

    if (block_next_raycast.exchange(false, std::memory_order_acq_rel)) {
        raycast_entered.store(true, std::memory_order_release);
        raycast_entered.notify_all();
        while (!resume_raycast.load(std::memory_order_acquire)) {
            resume_raycast.wait(false, std::memory_order_relaxed);
        }
    }

    *out_count = 1;
    if (capacity < 1) return Physics::Status::buffer_too_small;
    hits[0] = Physics::ProviderRaycastHitV1{
        .collider_index = 0,
        .distance = provider_distance,
        // Pelican must normalize provider normals before exposing a hit.
        .normal = {-2.0F, 0.0F, 0.0F},
    };
    return Physics::Status::ok;
}

struct Registration {
    Registration() noexcept {
        auto api = Physics::descriptor<Physics::ApiV1>();
        auto status = Physics::getApiV1(Physics::abiVersionV1, &api);
        if (status == Physics::Status::ok) {
            auto provider = Physics::descriptor<Physics::ProviderV1>();
            provider.capability_bits = Physics::query_raycast_all;
            provider.name_utf8 = provider_name;
            provider.name_size = static_cast<std::uint32_t>(sizeof(provider_name) - 1);
            provider.raycast_all = raycastAll;
            Physics::ProviderHandleV1 handle{};
            status = api.register_provider(api.context, &provider, &handle);
        }
        registration_status.store(static_cast<std::uint32_t>(status),
                                  std::memory_order_release);
    }
};

Registration registration;

} // namespace

PELICAN_FIXTURE_EXPORT std::uint32_t pelican_physics_fixture_registration_status() {
    return registration_status.load(std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT void pelican_physics_fixture_arm_next_raycast() {
    raycast_entered.store(false, std::memory_order_release);
    resume_raycast.store(false, std::memory_order_release);
    block_next_raycast.store(true, std::memory_order_release);
}

PELICAN_FIXTURE_EXPORT bool pelican_physics_fixture_raycast_entered() {
    return raycast_entered.load(std::memory_order_acquire);
}

PELICAN_FIXTURE_EXPORT void pelican_physics_fixture_resume_raycast() {
    resume_raycast.store(true, std::memory_order_release);
    resume_raycast.notify_all();
}
