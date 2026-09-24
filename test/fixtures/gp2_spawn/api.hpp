#pragma once

// GP-2: what the spawn fixture DLL reports back to its host test. Every
// ctx.spawn / ctx.destroy / ctx.objects() / ctx.physics() call is made inside
// the DLL, through the SDK the host exports, so the host observes the public
// surface from the far side of the DLL boundary.

#include <object_ref.hpp>

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define PELICAN_GP2_FIXTURE_API extern "C" __declspec(dllexport)
#else
#define PELICAN_GP2_FIXTURE_API extern "C" __attribute__((visibility("default")))
#endif

namespace Pelican::Gp2Fixture {

// A spawn or destroy request as the game saw its answer.
struct Request {
    // 1 when the call produced a ref; otherwise `error` names why not.
    std::uint32_t ok = 0;
    char error[96]{};
    TransientObjectRef ref;
};

// materialize() / worldTransform() of a transient ref.
struct Probe {
    std::uint32_t ok = 0;
    char error[64]{};
    std::uint32_t object_index = 0;
    std::uint32_t object_generation = 0;
    WorldTransform world;
};

struct RayProbe {
    std::uint32_t hit = 0;
    std::uint32_t entity_index = 0;
    std::uint32_t entity_generation = 0;
    char name[64]{};
};

enum class TraceKind : std::uint32_t { init = 1, destroy = 2 };

// One lifecycle callback of the probe behavior a spawned object carries.
struct TraceEntry {
    TraceKind kind = TraceKind::init;
    char tag[32]{};
    std::uint64_t attachment_seq = 0;
    std::uint32_t entity_index = 0;
    std::uint32_t entity_generation = 0;
};

} // namespace Pelican::Gp2Fixture

// Spawns "prefab" at (x, y, z) with parameter tag = `tag`, named `name`
// unless it is null, from outside every behavior callback.
using PelicanGp2SpawnFn = void (*)(const char *prefab, const char *tag, float x,
                                   float y, float z, const char *name,
                                   Pelican::Gp2Fixture::Request *result);
using PelicanGp2DestroyFn = void (*)(const Pelican::TransientObjectRef *ref,
                                     Pelican::Gp2Fixture::Request *result);
using PelicanGp2ProbeFn = void (*)(const Pelican::TransientObjectRef *ref,
                                   Pelican::Gp2Fixture::Probe *probe);
using PelicanGp2RaycastFn = void (*)(float x, float y, float z, float dx,
                                     float dy, float dz,
                                     Pelican::Gp2Fixture::RayProbe *probe);
using PelicanGp2TraceFn = std::size_t (*)(Pelican::Gp2Fixture::TraceEntry *out,
                                          std::size_t capacity);
using PelicanGp2ClearFn = void (*)();
// Queues a spawn the spawner behavior makes from its next onUpdate, inside
// the behavior callback. Same arguments as PelicanGp2SpawnFn without a name.
using PelicanGp2QueueInsideFn = void (*)(const char *prefab, const char *tag,
                                         float x, float y, float z);
// The answers the spawner behavior's spawns got, in the order it made them.
using PelicanGp2InsideResultsFn =
    std::size_t (*)(Pelican::Gp2Fixture::Request *out, std::size_t capacity);
