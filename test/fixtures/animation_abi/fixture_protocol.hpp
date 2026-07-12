#pragma once
#include <cstdint>

struct AnimationAbiFixtureLayout {
    std::uint32_t fixture_generation;
    std::uint32_t advance_size;
    std::uint32_t interval_size;
    std::uint32_t api_size;
    std::uint32_t advance_cursor_offset;
    std::uint32_t interval_crossings_offset;
    std::uint32_t api_context_offset;
    std::uint32_t reserved0;
};

#if defined(_WIN32)
#define ANIM_FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define ANIM_FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif
