#include "api.hpp"

#include <behavior.hpp>
#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>

#include <string>

namespace {
struct EffectEvent {
    std::int32_t value = 0;
    template <class Archive> void ref(Archive &archive) {
        archive.prop("value", value);
    }
};

struct Params {
    std::string sound;
    std::string slot;
    std::string init_scene;
    std::string retire_slot;
    std::string retire_scene;
    std::string retire_reject_scene;
    bool stop_provisional = false;
    bool fail_after_effects = false;
    bool invalid_retire_sound = false;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&Params::sound>("sound"), ""),
        Pelican::defaulted(Pelican::field<&Params::slot>("slot"), ""),
        Pelican::defaulted(Pelican::field<&Params::init_scene>("init_scene"), ""),
        Pelican::defaulted(Pelican::field<&Params::retire_slot>("retire_slot"), ""),
        Pelican::defaulted(Pelican::field<&Params::retire_scene>("retire_scene"), ""),
        Pelican::defaulted(Pelican::field<&Params::retire_reject_scene>("retire_reject_scene"), ""),
        Pelican::defaulted(Pelican::field<&Params::stop_provisional>("stop_provisional"), false),
        Pelican::defaulted(Pelican::field<&Params::fail_after_effects>("fail_after_effects"), false),
        Pelican::defaulted(Pelican::field<&Params::invalid_retire_sound>("invalid_retire_sound"), false));
};

Wp380bEffectProbe probe{};

} // namespace

struct EarlyEffectComponent {
    std::string sound;
    std::string slot;

    template <class Archive> void ref(Archive &archive) {
        archive.prop("sound", sound);
        archive.prop("slot", slot);
        Pelican::GameContext context;
        if (!sound.empty()) (void)context.playSound(sound);
        if (!slot.empty()) {
            context.saveData(slot, {{"source", "component_callback"}});
        }
        probe.component_callback_ran = 1;
    }
};

DECLARE_COMPONENT(EarlyEffectComponent, 62);

namespace {

class EarlyEffectRegistration final {
  public:
    EarlyEffectRegistration() {
        (void)Pelican::internal::getComponentRegisterer()
            .registerComponent<EarlyEffectComponent>("wp380e_early_effect");
    }
};

EarlyEffectRegistration early_effect_registration;

class EffectBehavior final : public Pelican::Behavior {
  public:
    using Params = ::Params;

    void onInit(Pelican::BehaviorContext &context) override {
        const auto &params = context.params<Params>();
        if (params.fail_after_effects) {
            probe.base_create_deferred =
                static_cast<Pelican::GameContext &>(context).createObject({}) ==
                Pelican::invalidGameObjectId;
        }
        context.setSeed(380U);
        (void)context.random();
        context.emit(EffectEvent{380});
        if (!params.init_scene.empty()) context.loadScene(params.init_scene);
        if (!params.sound.empty()) {
            const auto handle = context.playSound(params.sound);
            probe.handle = handle.value;
            probe.playing_after_play = context.isPlaying(handle);
            if (params.stop_provisional) context.stopSound(handle);
            probe.playing_after_stop = context.isPlaying(handle);
        }
        context.setBusVolume("se", 0.38F);
        context.setGameSettings({{"fixture", "wp380b"}});
        context.saveSettings();
        if (!params.slot.empty()) {
            const auto value = Pelican::GameContext::Json{{"slot", params.slot}};
            context.saveData(params.slot, value);
            probe.load_overlay_equal = context.loadData(params.slot) == value;
            for (const auto &entry : context.listSaves()) {
                if (entry.slot == params.slot) {
                    probe.list_overlay_timestamp_zero =
                        entry.timestamp == std::filesystem::file_time_type{};
                }
            }
        }
        if (params.fail_after_effects) {
            throw std::runtime_error("WP380b frozen v2 activation failure");
        }
    }

    void onDestroy(Pelican::BehaviorContext &context) noexcept override {
        const auto &params = context.params<Params>();
        if (!params.retire_slot.empty()) {
            const auto self = context.self();
            const auto transform = context.localTransform(self);
            auto document = Pelican::GameContext::Json{
                {"x", transform.pos.x}, {"object", Pelican::toString(self)}};
            if (const auto sprite = context.spriteView(self)) {
                document["texture"] = sprite->texture;
            }
            context.saveData(params.retire_slot, document);
        }
        if (params.invalid_retire_sound) {
            (void)context.playSound("project://missing-wp380b.wav");
        }
        if (!params.retire_scene.empty()) context.loadScene(params.retire_scene);
        if (!params.retire_reject_scene.empty()) {
            context.loadScene(params.retire_reject_scene);
        }
    }
};
} // namespace

PELICAN_REGISTER_EVENT(EffectEvent);
PELICAN_REGISTER_BEHAVIOR(EffectBehavior, "wp380b_effect_behavior", 1);

PELICAN_WP380B_FIXTURE_API bool
pelican_wp380b_effect_probe(Wp380bEffectProbe *output) {
    if (output == nullptr) return false;
    *output = probe;
    return true;
}
