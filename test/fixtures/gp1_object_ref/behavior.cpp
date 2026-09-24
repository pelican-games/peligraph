#include "api.hpp"

#include <behavior.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace {

using Pelican::Gp1Fixture::Probe;

template <class T>
void record(const Pelican::Resolved<T> &result, Probe &probe) {
    probe.ok = result.ok() ? 1U : 0U;
    const auto name = result.errorName();
    const auto count = std::min(name.size(), sizeof(probe.error) - 1U);
    std::copy_n(name.data(), count, probe.error);
    probe.error[count] = '\0';
}

void resolveInto(std::string_view name, Probe &probe) {
    const auto result = Pelican::GameContext{}.objects().resolve(name);
    record(result, probe);
    if (const auto *ref = result.get()) probe.ref = *ref;
}

void materializeInto(const Pelican::ObjectRef &ref, Probe &probe) {
    const auto result = Pelican::GameContext{}.objects().materialize(ref);
    record(result, probe);
    probe.ref = ref;
    if (const auto *object = result.get()) {
        probe.object_index = object->index;
        probe.object_generation = object->generation;
    }
}

void worldInto(const Pelican::ObjectRef &ref, Probe &probe) {
    const auto result = Pelican::GameContext{}.objects().worldTransform(ref);
    record(result, probe);
    probe.ref = ref;
    if (const auto *world = result.get()) probe.world = *world;
}

// What the behavior saw while its scene was still being loaded.
Probe init_resolved;
Probe init_world;
bool init_seen = false;

struct Params {
    std::string target;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&Params::target>("target"), ""));
};

// Resolves its target during onInit, while the scene that declares it is the
// load candidate and not yet the live scene.
class ObjectRefBehavior final : public Pelican::Behavior {
  public:
    using Params = ::Params;

    void onInit(Pelican::BehaviorContext &context) override {
        init_resolved = Probe{};
        init_world = Probe{};
        resolveInto(context.params<Params>().target, init_resolved);
        if (init_resolved.ok != 0U) worldInto(init_resolved.ref, init_world);
        init_seen = true;
    }
};

} // namespace

PELICAN_REGISTER_BEHAVIOR(ObjectRefBehavior, "gp1_object_ref_behavior", 1);

PELICAN_GP1_FIXTURE_API void pelican_gp1_fixture_resolve(const char *name,
                                                        Probe *probe) {
    *probe = Probe{};
    resolveInto(name, *probe);
}

PELICAN_GP1_FIXTURE_API void pelican_gp1_fixture_materialize(
    const Pelican::ObjectRef *ref, Probe *probe) {
    *probe = Probe{};
    materializeInto(*ref, *probe);
}

PELICAN_GP1_FIXTURE_API void pelican_gp1_fixture_world(
    const Pelican::ObjectRef *ref, Probe *probe) {
    *probe = Probe{};
    worldInto(*ref, *probe);
}

PELICAN_GP1_FIXTURE_API bool pelican_gp1_fixture_init_probe(Probe *resolved,
                                                           Probe *world) {
    if (!init_seen) return false;
    *resolved = init_resolved;
    *world = init_world;
    return true;
}
