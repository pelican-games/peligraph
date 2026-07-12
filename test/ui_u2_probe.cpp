#include "../src/core/userpublic/gamesystem.hpp"

#include <cstdint>
#include <string>

namespace {

struct UiDemoClick {
    std::string source;
    std::int32_t amount = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&UiDemoClick::source>("source"),
        Pelican::field<&UiDemoClick::amount>("amount", Pelican::irange(0, 99)));
    template <class T> void ref(T &ar) { ar.prop("source", source); ar.prop("amount", amount); }
};

struct UiDemoDrag {
    Pelican::vec2 delta{};
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&UiDemoDrag::delta>("delta"));
    template <class T> void ref(T &ar) { ar.prop("delta", delta); }
};

struct UiDemoSystem {
    void onEvent(const UiDemoClick &event, Pelican::GameContext &ctx) {
        if (event.source == "root/click") ctx.setSeed(9300 + static_cast<std::uint64_t>(event.amount));
    }
    void onEvent(const UiDemoDrag &event, Pelican::GameContext &ctx) {
        ctx.setSeed(9300 + static_cast<std::uint64_t>(event.delta.x));
    }
};

} // namespace

PELICAN_REGISTER_EVENT(UiDemoClick);
PELICAN_REGISTER_EVENT(UiDemoDrag);
PELICAN_REGISTER_SYSTEM(UiDemoSystem, 5);
