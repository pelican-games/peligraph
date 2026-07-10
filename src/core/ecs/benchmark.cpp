#include "core.hpp"
#include "componentinfo.hpp"
#include "predefined/transform.hpp"
#include "../phys/physworld.hpp"
#include "../profiler.hpp"

#include <components/predefined.hpp>
#include <details/component/registerer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct BenchmarkPodComponent {
    std::uint64_t sequence;
    float values[6];
};

struct BenchmarkStringComponent {
    std::string value;
};

struct alignas(64) BenchmarkOverAlignedComponent {
    std::array<std::uint64_t, 8> values{};
};

DECLARE_COMPONENT(BenchmarkPodComponent, 10);
DECLARE_COMPONENT(BenchmarkStringComponent, 11);
DECLARE_COMPONENT(BenchmarkOverAlignedComponent, 12);

} // namespace Pelican

namespace {

using Clock = std::chrono::steady_clock;
using namespace Pelican;

constexpr int WARMUP_RUNS = 2;
constexpr int SAMPLE_RUNS = 11;
constexpr std::uint32_t FIXED_SEED = 0x62ECA11U;
volatile std::uint64_t benchmark_sink = 0;

template <class Component, class Populate>
std::vector<EntityId> createTyped(ECSCore &ecs, std::size_t count, Populate &&populate) {
    constexpr std::array component_ids{ComponentIdByType<Component>::value};
    std::uint64_t sequence = 0;
    return ecs.createEntities(
        component_ids, count,
        [&](std::span<const EntityId>, std::span<void *> ptrs, size_t batch_count) {
            auto *values = static_cast<Component *>(ptrs[0]);
            populate(values, batch_count, sequence);
            sequence += batch_count;
        });
}

std::vector<EntityId> createPodEntities(ECSCore &ecs, std::size_t count) {
    return createTyped<BenchmarkPodComponent>(
        ecs, count, [](BenchmarkPodComponent *pods, size_t batch_count, std::uint64_t sequence) {
            for (size_t i = 0; i < batch_count; ++i) {
                const auto value = sequence + i;
                pods[i] = BenchmarkPodComponent{
                    .sequence = value,
                    .values = {static_cast<float>(value), 1.0F, 2.0F, 3.0F, 4.0F, 5.0F},
                };
            }
        });
}

std::vector<EntityId> createStringEntities(ECSCore &ecs, std::size_t count) {
    return createTyped<BenchmarkStringComponent>(
        ecs, count, [](BenchmarkStringComponent *values, size_t batch_count, std::uint64_t sequence) {
            for (size_t i = 0; i < batch_count; ++i) {
                values[i].value = "entity-" + std::to_string(sequence + i);
            }
        });
}

std::vector<EntityId> createOverAlignedEntities(ECSCore &ecs, std::size_t count) {
    return createTyped<BenchmarkOverAlignedComponent>(
        ecs, count, [](BenchmarkOverAlignedComponent *values, size_t batch_count, std::uint64_t sequence) {
            for (size_t i = 0; i < batch_count; ++i) {
                values[i].values[0] = sequence + i;
            }
        });
}

template <class Function>
double elapsedMilliseconds(Function &&function) {
    const auto begin = Clock::now();
    function();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <class TimedRun>
void reportCase(std::string_view name, TimedRun &&run) {
    for (int i = 0; i < WARMUP_RUNS; ++i) {
        (void)run();
    }

    std::vector<double> samples;
    samples.reserve(SAMPLE_RUNS);
    for (int i = 0; i < SAMPLE_RUNS; ++i) {
        samples.push_back(run());
    }
    std::sort(samples.begin(), samples.end());
    const auto median = samples[samples.size() / 2];
    const auto p95_index = static_cast<std::size_t>((samples.size() - 1) * 0.95);
    const auto p95 = samples[p95_index];
    std::cout << std::left << std::setw(37) << name << " median_ms=" << std::right << std::fixed
              << std::setprecision(3) << median << " p95_ms=" << p95 << '\n';
}

template <class Create>
double createEntities(std::size_t count, Create &&create) {
    return elapsedMilliseconds([&] {
        ECSCore ecs;
        const auto ids = create(ecs, count);
        const auto last = ids.back();
        benchmark_sink = benchmark_sink ^
                         ((static_cast<std::uint64_t>(last.index) << 32U) | last.generation);
    });
}

template <class Create>
double relocateEntities(Create &&create) {
    ECSCore ecs;
    const auto ids = create(ecs, ECSComponentChunk::CHUNK_CAPACITY);
    return elapsedMilliseconds([&] {
        for (std::size_t i = 0; i < ECSComponentChunk::CHUNK_CAPACITY / 2; ++i) {
            (void)ecs.remove(ids[i]);
        }
    });
}

double freeListMixed() {
    ECSCore ecs;
    auto ids = createPodEntities(ecs, ECSComponentChunk::CHUNK_CAPACITY);
    std::mt19937 random{FIXED_SEED};
    std::shuffle(ids.begin(), ids.end(), random);
    for (size_t i = 0; i < ids.size() / 2; ++i) {
        (void)ecs.remove(ids[i]);
    }
    return elapsedMilliseconds([&] {
        const auto reused = createPodEntities(ecs, ids.size() / 2);
        benchmark_sink = benchmark_sink ^ reused.back().generation;
    });
}

double resolvePod(bool hit) {
    ECSCore ecs;
    const auto ids = createPodEntities(ecs, ECSComponentChunk::CHUNK_CAPACITY);
    constexpr std::size_t iterations = 1'000'000;
    return elapsedMilliseconds([&] {
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < iterations; ++i) {
            const auto id = hit ? ids[i % ids.size()]
                                : EntityId{static_cast<std::uint32_t>(ids.size() + 17), 0};
            if (const auto *pod = ecs.getTemplatePublicModule().tryComponent<BenchmarkPodComponent>(id)) {
                sum += pod->sequence;
            }
        }
        benchmark_sink = benchmark_sink ^ sum;
    });
}

double physWorldQuery() {
    auto &ecs = GET_MODULE(ECSCore);
    auto &world = GET_MODULE(PhysWorld);
    world.clear();
    ecs.clearEntities();

    constexpr std::array component_ids{ComponentIdByType<TransformComponent>::value};
    const auto ids = ecs.createEntities(
        component_ids, 512,
        [](std::span<const EntityId>, std::span<void *> ptrs, size_t count) {
            auto *transforms = static_cast<TransformComponent *>(ptrs[0]);
            for (size_t i = 0; i < count; ++i) {
                transforms[i].pos = glm::vec3{2.0F + static_cast<float>(i) * 2.0F, 0.0F, 0.0F};
                transforms[i].rotation = glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
                transforms[i].scale = glm::vec3{1.0F};
            }
        });
    ColliderComponent collider;
    collider.shape = "sphere";
    collider.radius = 0.5F;
    for (size_t i = 0; i < ids.size(); ++i) {
        world.bindCollider("benchmark-" + std::to_string(i), collider, ids[i]);
    }

    const auto elapsed = elapsedMilliseconds([&] {
        for (size_t i = 0; i < 200; ++i) {
            const auto hit = world.raycastClosest(
                phys::Ray{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, 2048.0F});
            benchmark_sink = benchmark_sink ^ static_cast<std::uint64_t>(hit ? hit->distance * 1000.0F : 0.0F);
        }
    });
    world.clear();
    ecs.clearEntities();
    return elapsed;
}

} // namespace

int main() {
    Pelican::setupLogger();
    Pelican::FastModuleContainer modules;
    auto &registerer = Pelican::internal::getComponentRegisterer();
    registerer.registerComponent<Pelican::EntityId>("EntityId");
    registerer.registerComponent<Pelican::TransformComponent>("TransformComponent");
    registerer.registerComponent<Pelican::BenchmarkPodComponent>("BenchmarkPodComponent");
    registerer.registerComponent<Pelican::BenchmarkStringComponent>("BenchmarkStringComponent");
    registerer.registerComponent<Pelican::BenchmarkOverAlignedComponent>("BenchmarkOverAlignedComponent");

    std::cout << "ECS benchmark (Release, fixed_seed=" << FIXED_SEED << ", warmup=" << WARMUP_RUNS
              << ", samples=" << SAMPLE_RUNS << ")\n";
    for (const auto count : {std::size_t{1}, std::size_t{4096}, std::size_t{4097}, std::size_t{100'000}}) {
        reportCase("create_pod_" + std::to_string(count),
                   [=] { return createEntities(count, createPodEntities); });
    }
    for (const auto count : {std::size_t{4096}, std::size_t{100'000}}) {
        reportCase("create_string_" + std::to_string(count),
                   [=] { return createEntities(count, createStringEntities); });
        reportCase("create_overaligned_" + std::to_string(count),
                   [=] { return createEntities(count, createOverAlignedEntities); });
    }
    reportCase("relocate_pod_2048", [] { return relocateEntities(createPodEntities); });
    reportCase("relocate_string_2048", [] { return relocateEntities(createStringEntities); });
    reportCase("create_freelist_mixed_2048", freeListMixed);
    reportCase("resolve_hit_pod_1m", [] { return resolvePod(true); });
    reportCase("resolve_miss_pod_1m", [] { return resolvePod(false); });
    reportCase("physworld_query_512x200", physWorldQuery);
    return benchmark_sink == std::numeric_limits<std::uint64_t>::max() ? 1 : 0;
}
