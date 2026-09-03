#pragma once

#include <cstdint>

#ifdef _WIN32
#ifdef PELICAN_WP380B_FIXTURE_BUILD
#define PELICAN_WP380B_FIXTURE_API extern "C" __declspec(dllexport)
#else
#define PELICAN_WP380B_FIXTURE_API extern "C" __declspec(dllimport)
#endif
#else
#define PELICAN_WP380B_FIXTURE_API extern "C"
#endif

struct Wp380bEffectProbe {
    std::uint64_t handle = 0;
    std::uint8_t playing_after_play = 0;
    std::uint8_t playing_after_stop = 0;
    std::uint8_t load_overlay_equal = 0;
    std::uint8_t list_overlay_timestamp_zero = 0;
    std::uint8_t component_callback_ran = 0;
    std::uint8_t base_create_deferred = 0;
};

PELICAN_WP380B_FIXTURE_API bool
pelican_wp380b_effect_probe(Wp380bEffectProbe *probe);
