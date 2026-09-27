#include "api.hpp"

#include <behavior.hpp>
#include <gamesystem.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Pelican::Gp2Fixture::Probe;
using Pelican::Gp2Fixture::RayProbe;
using Pelican::Gp2Fixture::Request;
using Pelican::Gp2Fixture::TraceEntry;
using Pelican::Gp2Fixture::TraceKind;

template <std::size_t N> void copyText(std::string_view text, char (&out)[N]) {
    const auto count = std::min(text.size(), N - 1U);
    std::copy_n(text.data(), count, out);
    out[count] = '\0';
}

std::vector<TraceEntry> trace;

void record(TraceKind kind, Pelican::BehaviorContext &context,
            std::string_view tag) {
    TraceEntry entry;
    entry.kind = kind;
    copyText(tag, entry.tag);
    entry.attachment_seq = context.attachmentSeq();
    entry.entity_index = context.self().index;
    entry.entity_generation = context.self().generation;
    trace.push_back(entry);
}

Pelican::SpawnParams spawnParams(const char *tag, float x, float y, float z,
                                 const char *name) {
    Pelican::SpawnParams params;
    params.transform.pos = Pelican::vec3{x, y, z};
    params.parameters = nlohmann::json{{"tag", std::string{tag}}};
    if (name != nullptr) params.name = std::string{name};
    return params;
}

Request spawnNow(const char *prefab, const Pelican::SpawnParams &params) {
    Request result;
    try {
        result.ref = Pelican::GameContext{}.spawn(prefab, params);
        result.ok = 1U;
    } catch (const Pelican::SpawnError &error) {
        copyText(error.code(), result.error);
    }
    return result;
}

// The object a spawn makes carries this behavior: it reports its lifecycle.
struct ProbeParams {
    std::string tag;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&ProbeParams::tag>("tag"), ""));
};

class ProbeBehavior final : public Pelican::Behavior {
  public:
    using Params = ProbeParams;

    void onInit(Pelican::BehaviorContext &context) override {
        record(TraceKind::init, context, context.params<Params>().tag);
    }
    void onDestroy(Pelican::BehaviorContext &context) noexcept override {
        record(TraceKind::destroy, context, context.params<Params>().tag);
    }
};

struct InsideSpawn {
    std::string prefab;
    std::string tag;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

std::vector<InsideSpawn> inside_pending;
std::vector<Request> inside_results;
// PM-1a: what game code ran. A paused frame runs neither.
std::uint64_t spawner_updates = 0;
std::uint64_t system_updates = 0;
std::uint64_t system_last_frame = 0;
double system_last_dt = -1.0;

struct SpawnerParams {
    std::string label;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&SpawnerParams::label>("label"), ""));
};

// A scene object: spawns what the host queued, from inside its onUpdate.
class SpawnerBehavior final : public Pelican::Behavior {
  public:
    using Params = SpawnerParams;

    void onUpdate(Pelican::BehaviorContext &) override {
        ++spawner_updates;
        auto pending = std::move(inside_pending);
        inside_pending.clear();
        for (const auto &spawn : pending) {
            inside_results.push_back(spawnNow(
                spawn.prefab.c_str(),
                spawnParams(spawn.tag.c_str(), spawn.x, spawn.y, spawn.z, nullptr)));
        }
    }
};

// PM-1a: a game system of the DLL, with what it last saw of the game clock.
struct Gp2TickSystem {
    void update(Pelican::GameContext &ctx) {
        ++system_updates;
        system_last_frame = ctx.frameIndex();
        system_last_dt = ctx.deltaTime();
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(Gp2TickSystem, 60);
PELICAN_REGISTER_BEHAVIOR(ProbeBehavior, "gp2_probe", 1);
PELICAN_REGISTER_BEHAVIOR(SpawnerBehavior, "gp2_spawner", 1);

PELICAN_GP2_FIXTURE_API void pelican_gp2_spawn(const char *prefab,
                                              const char *tag, float x, float y,
                                              float z, const char *name,
                                              Request *result) {
    *result = spawnNow(prefab, spawnParams(tag, x, y, z, name));
}

PELICAN_GP2_FIXTURE_API void pelican_gp2_destroy(
    const Pelican::TransientObjectRef *ref, Request *result) {
    *result = Request{};
    const auto answer = Pelican::GameContext{}.destroy(*ref);
    result->ok = answer.ok() ? 1U : 0U;
    copyText(answer.errorName(), result->error);
    if (const auto *value = answer.get()) result->ref = *value;
}

PELICAN_GP2_FIXTURE_API void pelican_gp2_materialize(
    const Pelican::TransientObjectRef *ref, Probe *probe) {
    *probe = Probe{};
    const auto answer = Pelican::GameContext{}.objects().materialize(*ref);
    probe->ok = answer.ok() ? 1U : 0U;
    copyText(answer.errorName(), probe->error);
    if (const auto *object = answer.get()) {
        probe->object_index = object->index;
        probe->object_generation = object->generation;
    }
}

PELICAN_GP2_FIXTURE_API void pelican_gp2_world(
    const Pelican::TransientObjectRef *ref, Probe *probe) {
    *probe = Probe{};
    const auto answer = Pelican::GameContext{}.objects().worldTransform(*ref);
    probe->ok = answer.ok() ? 1U : 0U;
    copyText(answer.errorName(), probe->error);
    if (const auto *world = answer.get()) probe->world = *world;
}

PELICAN_GP2_FIXTURE_API void pelican_gp2_raycast(float x, float y, float z,
                                                float dx, float dy, float dz,
                                                RayProbe *probe) {
    *probe = RayProbe{};
    const auto hit = Pelican::GameContext{}.physics().raycastClosest(
        Pelican::phys::Ray{.origin = Pelican::vec3{x, y, z},
                           .direction = Pelican::vec3{dx, dy, dz}});
    if (!hit) return;
    probe->hit = 1U;
    if (hit->identity.entity) {
        probe->entity_index = hit->identity.entity->index;
        probe->entity_generation = hit->identity.entity->generation;
    }
    copyText(hit->identity.name, probe->name);
}

PELICAN_GP2_FIXTURE_API std::size_t pelican_gp2_trace(TraceEntry *out,
                                                     std::size_t capacity) {
    const auto count = std::min(capacity, trace.size());
    std::copy_n(trace.begin(), count, out);
    return trace.size();
}

PELICAN_GP2_FIXTURE_API void pelican_gp2_clear() {
    trace.clear();
    inside_pending.clear();
    inside_results.clear();
}

PELICAN_GP2_FIXTURE_API void pelican_gp2_game_code(Pelican::Gp2Fixture::GameCodeProbe *probe) {
    probe->spawner_updates = spawner_updates;
    probe->system_updates = system_updates;
    probe->system_last_frame = system_last_frame;
    probe->system_last_dt = system_last_dt;
}

PELICAN_GP2_FIXTURE_API void pelican_gp2_queue_inside(const char *prefab,
                                                     const char *tag, float x,
                                                     float y, float z) {
    inside_pending.push_back(InsideSpawn{prefab, tag, x, y, z});
}

PELICAN_GP2_FIXTURE_API std::size_t pelican_gp2_inside_results(
    Request *out, std::size_t capacity) {
    const auto count = std::min(capacity, inside_results.size());
    std::copy_n(inside_results.begin(), count, out);
    return inside_results.size();
}
