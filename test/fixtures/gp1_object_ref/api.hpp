#pragma once

// GP-1: what the object-ref fixture DLL reports back to its host test. The
// DLL makes every ctx.objects() call itself, through the SDK the host
// exports, so the host observes the public surface from the far side of the
// DLL boundary.

#include <object_ref.hpp>

#include <cstdint>

#if defined(_WIN32)
#define PELICAN_GP1_FIXTURE_API extern "C" __declspec(dllexport)
#else
#define PELICAN_GP1_FIXTURE_API extern "C" __attribute__((visibility("default")))
#endif

namespace Pelican::Gp1Fixture {

struct Probe {
    // 1 when the call produced a value; otherwise `error` names why not.
    std::uint32_t ok = 0;
    char error[64]{};
    ObjectRef ref;
    std::uint32_t object_index = 0;
    std::uint32_t object_generation = 0;
    WorldTransform world;
};

} // namespace Pelican::Gp1Fixture

using PelicanGp1ResolveFn = void (*)(const char *name,
                                     Pelican::Gp1Fixture::Probe *probe);
using PelicanGp1RefFn = void (*)(const Pelican::ObjectRef *ref,
                                 Pelican::Gp1Fixture::Probe *probe);
using PelicanGp1InitProbeFn = bool (*)(Pelican::Gp1Fixture::Probe *resolved,
                                       Pelican::Gp1Fixture::Probe *world);
