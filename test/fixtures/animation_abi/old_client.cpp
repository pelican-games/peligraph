#include "fixture_protocol.hpp"
#include "v0/animation_abi_v1.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

using namespace Pelican::AnimationV0;

ANIM_FIXTURE_EXPORT void pelican_animation_fixture_layout(AnimationAbiFixtureLayout *out) {
    *out = {1, sizeof(AdvanceDescV1), sizeof(IntervalResultV1), sizeof(ApiV1),
            offsetof(AdvanceDescV1, cursor), offsetof(IntervalResultV1, crossings), offsetof(ApiV1, context), 0};
}

ANIM_FIXTURE_EXPORT void pelican_animation_fixture_write_advance(void *out, std::uint32_t capacity) {
    AdvanceDescV1 value{};
    value.struct_size = sizeof(value);
    value.version = descriptorVersionV1;
    value.cursor = {77, 1, 0};
    value.delta_seconds = 0.5;
    std::memcpy(out, &value, std::min<std::size_t>(capacity, sizeof(value)));
}

ANIM_FIXTURE_EXPORT std::uint32_t pelican_animation_fixture_old_engine(void *out, std::uint32_t caller_size) {
    ApiV1 value{};
    value.struct_size = sizeof(value);
    value.version = descriptorVersionV1;
    value.engine_abi_version = 1;
    value.minimum_client_abi_version = 1;
    value.capability_bits = 0x1;
    value.context = reinterpret_cast<void *>(std::uintptr_t{0x1234});
    const auto written = std::min<std::size_t>(caller_size, sizeof(value));
    std::memcpy(out, &value, written);
    return static_cast<std::uint32_t>(written);
}

ANIM_FIXTURE_EXPORT std::uint32_t pelican_animation_fixture_accept_api(const void *data, std::uint32_t available) {
    if (available < 16) return 0;
    const auto *api = static_cast<const ApiV1 *>(data);
    return api->version == 1 && api->struct_size >= 16 ? 1u : 0u;
}
