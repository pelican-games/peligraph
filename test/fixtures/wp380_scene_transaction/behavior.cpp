#include "api.hpp"

#include <behavior.hpp>

#include <array>
#include <stdexcept>
#include <string>

namespace {

using Pelican::Wp380Fixture::ProbeV1;

struct Params {
    std::int32_t scene_tag = 0;
    bool fail_after_effects = false;
    std::string pending_scene;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(
            Pelican::field<&Params::scene_tag>("scene_tag"), 0),
        Pelican::defaulted(
            Pelican::field<&Params::fail_after_effects>(
                "fail_after_effects"),
            false),
        Pelican::defaulted(
            Pelican::field<&Params::pending_scene>("pending_scene"), ""));
};

std::array<ProbeV1, 5> observed{};
std::array<bool, 5> observed_valid{};

ProbeV1 expected(std::int32_t tag) {
    ProbeV1 result{
        .scene_tag = tag,
        .light_name_tag = tag,
        .light_intensity = static_cast<float>(tag) + 0.25F,
        .camera_yfov = 0.3F + static_cast<float>(tag) * 0.1F,
        .ecs_x = static_cast<float>(tag * 10),
        .collider_count = static_cast<std::uint64_t>(tag),
        .active_trigger_pair_count = 0,
        .renderer_instance_count = static_cast<std::uint64_t>(tag),
        // The production chair fixture contains two one-primitive meshes, so
        // each live model instance consumes two declaration ordinals.
        .renderer_ordinal = static_cast<std::uint64_t>(tag * 2),
        .binding_count = static_cast<std::uint64_t>(tag + 1),
        .behavior_count = 1,
        .pending_request_issued = 1,
    };
    switch (tag) {
    case 1: result.random_value = 0.43165494083702083; break;
    case 2: result.random_value = 0.17866711812860847; break;
    case 3: result.random_value = 0.18487200224818057; break;
    case 4: result.random_value = 0.18487200224818057; break;
    default: break;
    }
    return result;
}

class SceneTransactionBehavior final : public Pelican::Behavior {
  public:
    using Params = ::Params;

    void onInit(Pelican::BehaviorContext &context) override {
        const auto &params = context.params<Params>();
        if (params.scene_tag <= 0 ||
            static_cast<std::size_t>(params.scene_tag) >= observed.size()) {
            throw std::runtime_error("WP380 fixture scene tag is out of range");
        }
        ProbeV1 probe{.scene_tag = params.scene_tag};
        pelican_wp380_host_read_probe(&probe);
        probe.random_value = context.random();
        context.emit(Pelican::Wp380Fixture::Event{
            .scene_tag = params.scene_tag,
            .payload_sentinel = params.scene_tag * 1000 + 380,
        });
        context.loadScene(params.pending_scene);
        probe.pending_request_issued = 1;
        observed[static_cast<std::size_t>(params.scene_tag)] = probe;
        observed_valid[static_cast<std::size_t>(params.scene_tag)] = true;
        if (params.fail_after_effects) {
            throw std::runtime_error("WP380 fixture activation failure");
        }
    }
};

} // namespace

PELICAN_REGISTER_EVENT(Pelican::Wp380Fixture::Event);
PELICAN_REGISTER_BEHAVIOR(SceneTransactionBehavior,
                          "wp380_scene_transaction_behavior", 1);

PELICAN_WP380_FIXTURE_API bool pelican_wp380_fixture_read_probe(
    std::int32_t scene_tag, ProbeV1 *probe) {
    if (probe == nullptr || scene_tag <= 0 ||
        static_cast<std::size_t>(scene_tag) >= observed.size() ||
        !observed_valid[static_cast<std::size_t>(scene_tag)]) {
        return false;
    }
    *probe = observed[static_cast<std::size_t>(scene_tag)];
    return true;
}

PELICAN_WP380_FIXTURE_API bool pelican_wp380_fixture_read_expected(
    std::int32_t scene_tag, ProbeV1 *probe) {
    if (probe == nullptr || scene_tag <= 0 || scene_tag > 4) return false;
    *probe = expected(scene_tag);
    return true;
}
