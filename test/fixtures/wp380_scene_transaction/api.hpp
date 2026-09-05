#pragma once

#include <cstddef>
#include <cstdint>

namespace Pelican::Wp380Fixture {

inline constexpr std::uint32_t probe_abi_v1 = 1;

struct Event {
    std::int32_t scene_tag = 0;
    std::int32_t payload_sentinel = 0;

    template <class Archive> void ref(Archive &archive) {
        archive.prop("scene_tag", scene_tag);
        archive.prop("payload_sentinel", payload_sentinel);
    }
};

// POD crosses the test-host/DLL boundary.  The expected copy is compiled into
// the fixture and is therefore independent of whatever scene B happened to
// publish in the engine under test.
struct ProbeV1 {
    std::uint32_t abi = probe_abi_v1;
    std::int32_t scene_tag = 0;
    std::int32_t light_name_tag = 0;
    float light_intensity = 0.0F;
    float camera_yfov = 0.0F;
    float ecs_x = 0.0F;
    std::uint64_t collider_count = 0;
    std::uint64_t active_trigger_pair_count = 0;
    std::uint64_t renderer_instance_count = 0;
    std::uint64_t renderer_ordinal = 0;
    std::uint64_t binding_count = 0;
    std::uint64_t behavior_count = 0;
    double random_value = 0.0;
    std::int32_t pending_request_issued = 0;
};

} // namespace Pelican::Wp380Fixture

#if defined(_WIN32)
#if defined(PELICAN_WP380_FIXTURE_BUILD)
#define PELICAN_WP380_HOST_API extern "C" __declspec(dllimport)
#define PELICAN_WP380_FIXTURE_API extern "C" __declspec(dllexport)
#else
#define PELICAN_WP380_HOST_API extern "C" __declspec(dllexport)
#define PELICAN_WP380_FIXTURE_API extern "C"
#endif
#else
#define PELICAN_WP380_HOST_API extern "C"
#define PELICAN_WP380_FIXTURE_API extern "C"
#endif

PELICAN_WP380_HOST_API void pelican_wp380_host_read_probe(
    Pelican::Wp380Fixture::ProbeV1 *probe);

using PelicanWp380ReadProbeFn = bool (*)(
    std::int32_t scene_tag, Pelican::Wp380Fixture::ProbeV1 *probe);
using PelicanWp380ReadExpectedFn = bool (*)(
    std::int32_t scene_tag, Pelican::Wp380Fixture::ProbeV1 *probe);
using PelicanWp380SetCallableRecordFn = void (*)(
    const char *task_name, std::uint32_t index, std::uint32_t increment);
