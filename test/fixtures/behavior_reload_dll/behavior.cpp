#include <behavior.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef WP162_VARIANT
#define WP162_VARIANT 1
#endif

namespace {

struct Wp162QueuedEvent {};

constexpr std::string_view variantName() {
#if WP162_VARIANT == 1
    return "v1";
#elif WP162_VARIANT == 2
    return "code_v2";
#elif WP162_VARIANT == 3
    return "drift";
#elif WP162_VARIANT == 4
    return "bump_ok";
#elif WP162_VARIANT == 5
    return "bump_bad";
#elif WP162_VARIANT == 6
    return "removed";
#elif WP162_VARIANT == 7
    return "init_fault";
#else
#error Unknown WP162 fixture variant
#endif
}

} // namespace

PELICAN_REGISTER_EVENT(Wp162QueuedEvent);

#if WP162_VARIANT != 6

namespace {

struct ReloadBehaviorParams {
    std::int32_t count = 0;
    std::string label;
#if WP162_VARIANT == 3
    bool drift = false;
#elif WP162_VARIANT == 4
    bool enabled = false;
#endif

#if WP162_VARIANT == 3
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::count>("count"), 1),
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::label>("label"),
                           "default"),
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::drift>("drift"),
                           false));
#elif WP162_VARIANT == 4
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::count>("count"), 1),
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::label>("label"),
                           "default"),
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::enabled>("enabled"),
                           true));
#elif WP162_VARIANT == 5
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(
            Pelican::field<&ReloadBehaviorParams::count>("count", Pelican::irange(0, 10)),
            1),
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::label>("label"),
                           "default"));
#else
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::count>("count"), 1),
        Pelican::defaulted(Pelican::field<&ReloadBehaviorParams::label>("label"),
                           "default"));
#endif
};

class ReloadBehavior final : public Pelican::Behavior {
    std::int32_t internal_state = 0;
    bool emitted = false;

    static std::string prefix(std::string_view phase) {
        return "WP162 generation=" + std::string{variantName()} +
               " phase=" + std::string{phase};
    }

  public:
    using Params = ReloadBehaviorParams;

    void onInit(Pelican::BehaviorContext &ctx) override {
#if WP162_VARIANT == 7
        ctx.logInfo(prefix("init_fault"));
        throw std::runtime_error("WP162 fixture onInit fault");
#else
        const auto &params = ctx.params<Params>();
        ctx.logInfo(prefix("init") + " committed_count=" +
                    std::to_string(params.count) + " label=" + params.label +
                    " internal_state=" + std::to_string(internal_state));
#endif
    }

    void onEvent(const Wp162QueuedEvent &, Pelican::BehaviorContext &ctx) {
        ctx.logInfo(prefix("event"));
    }

    void onUpdate(Pelican::BehaviorContext &ctx) override {
        auto &params = ctx.params<Params>();
        ++internal_state;
        params.count = 99;
        ctx.logInfo(prefix("update") + " runtime_count=" +
                    std::to_string(params.count) + " internal_state=" +
                    std::to_string(internal_state));
        if (!emitted) {
            ctx.emit(Wp162QueuedEvent{});
            emitted = true;
        }
    }

    void onDestroy(Pelican::BehaviorContext &ctx) noexcept override {
        const auto &params = ctx.params<Params>();
        ctx.logInfo(prefix("destroy") + " runtime_count=" +
                    std::to_string(params.count) + " internal_state=" +
                    std::to_string(internal_state));
    }
};

} // namespace

#if WP162_VARIANT == 4 || WP162_VARIANT == 5
PELICAN_REGISTER_BEHAVIOR(ReloadBehavior, "wp162_reload_behavior", 2);
#else
PELICAN_REGISTER_BEHAVIOR(ReloadBehavior, "wp162_reload_behavior", 1);
#endif

#endif
