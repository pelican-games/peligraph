#include "fixture_protocol.hpp"
#include "animation/abi_v1.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

using namespace Pelican::Animation;

ANIM_FIXTURE_EXPORT void pelican_animation_fixture_layout(AnimationAbiFixtureLayout *out) {
    *out = {2, sizeof(AdvanceDescV1), sizeof(IntervalResultV1), sizeof(ApiV1),
            offsetof(AdvanceDescV1, cursor), offsetof(IntervalResultV1, crossings), offsetof(ApiV1, context), 0};
}

ANIM_FIXTURE_EXPORT void pelican_animation_fixture_write_advance(void *out, std::uint32_t capacity) {
    AdvanceDescV1 value{};
    value.struct_size = sizeof(value);
    value.version = descriptorVersionV1;
    value.cursor = {77, 2, 0};
    value.delta_seconds = 0.5;
    std::memcpy(out, &value, std::min<std::size_t>(capacity, sizeof(value)));
}

ANIM_FIXTURE_EXPORT std::uint32_t pelican_animation_fixture_old_engine(void *, std::uint32_t) { return 0; }

ANIM_FIXTURE_EXPORT std::uint32_t pelican_animation_fixture_accept_api(const void *data, std::uint32_t available) {
    if (available < 16) return 0;
    const auto *api = static_cast<const ApiV1 *>(data);
    return api->version == 1 && api->struct_size >= 16 ? 1u : 0u;
}

ANIM_FIXTURE_EXPORT std::uint32_t pelican_animation_fixture_service_available(const void *data,
                                                                                std::uint32_t available) {
    constexpr auto required = offsetof(ApiV1, get_animation_service) + sizeof(GetAnimationServiceV1Fn);
    if (available < required) return 0;
    const auto *api = static_cast<const ApiV1 *>(data);
    return api->struct_size >= required && api->get_animation_service != nullptr ? 1u : 0u;
}
