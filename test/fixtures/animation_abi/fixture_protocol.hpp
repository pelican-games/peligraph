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

struct AnimationEvaluatorFixtureResult {
    std::uint32_t status;
    std::uint32_t phase_callback_count;
    float single_clip_translation_x;
    float blended_translation_x;
    float palette_translation_x;
    std::uint32_t cursor_advance_status;
    std::uint64_t sink_identity;
    std::uint32_t sink_generation;
    std::uint32_t reserved0;
    std::uint64_t source_identity;
    std::uint32_t source_generation;
    std::uint32_t reserved1;
};

#if defined(_WIN32)
#define ANIM_FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define ANIM_FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif
