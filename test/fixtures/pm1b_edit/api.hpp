#pragma once

// PM-1b: what the edit-state fixture DLL reports back to its host test. Every
// count is kept inside the DLL, so a reload (a fresh copy of the DLL) starts
// them at 0 again.

#include <cstdint>

#if defined(_WIN32)
#define PELICAN_PM1B_FIXTURE_API extern "C" __declspec(dllexport)
#else
#define PELICAN_PM1B_FIXTURE_API extern "C" __attribute__((visibility("default")))
#endif

namespace Pelican::Pm1bFixture {

struct Counts {
    // pm1b_mover: its constructor, onInit, onUpdate and onDestroy.
    std::uint64_t constructed = 0;
    std::uint64_t inits = 0;
    std::uint64_t updates = 0;
    std::uint64_t destroys = 0;
    // pm1b_spark, which the object a mover spawns carries.
    std::uint64_t spark_inits = 0;
    std::uint64_t spark_destroys = 0;
    // The spawns a mover asked for, and the ones refused.
    std::uint64_t spawns = 0;
    std::uint64_t spawn_errors = 0;
};

} // namespace Pelican::Pm1bFixture

using PelicanPm1bCountsFn = void (*)(Pelican::Pm1bFixture::Counts *counts);
using PelicanPm1bResetFn = void (*)();
