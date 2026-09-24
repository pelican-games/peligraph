// GP-3: game code that writes the UI through ctx.ui(). The headless player
// this links into injects Gp3UiStep events over RPC; each step makes one
// facade call and reports its outcome as the RNG seed, which get_status
// prints: 7000 + step * 10 + (0 when accepted, else the UiWriteError value).
#include "../src/core/userpublic/gamesystem.hpp"

#include <cstdint>

namespace {

struct Gp3UiStep {
    std::int32_t step = 0;
    static constexpr auto pelican_payload = Pelican::payloadFields(
        Pelican::field<&Gp3UiStep::step>("step", Pelican::irange(0, 99)));
    template <class T> void ref(T &ar) { ar.prop("step", step); }
};

struct Gp3UiProbeSystem {
    void onEvent(const Gp3UiStep &event, Pelican::GameContext &ctx) {
        Pelican::UiWriteResult result;
        switch (event.step) {
        case 1: result = ctx.ui().setText("root/hud/label", "HELLO"); break;
        case 2: result = ctx.ui().setVisible("root/hud/label", false); break;
        case 3: result = ctx.ui().setValue("root/hud/gauge", 0.9f); break;
        // The meter's value is written by a ui.value sink when the render
        // config carries one; its visibility is nobody else's.
        case 4: result = ctx.ui().setValue("root/hud/meter", 0.5f); break;
        case 5: result = ctx.ui().setVisible("root/hud/meter", true); break;
        case 6: result = ctx.ui().setText("root/hud/missing", "X"); break;
        case 7: result = ctx.ui().setText("root/hud/gauge", "X"); break;
        // The document key named explicitly, then a key the project lacks.
        case 8: result = ctx.ui("gp3_hud").setText("root/hud/label", "KEYED"); break;
        case 9: result = ctx.ui("other").setText("root/hud/label", "X"); break;
        default: return;
        }
        ctx.setSeed(7000 + static_cast<std::uint64_t>(event.step) * 10 +
                    (result.ok() ? 0 : static_cast<std::uint64_t>(result.error())));
    }
};

} // namespace

PELICAN_REGISTER_EVENT(Gp3UiStep);
PELICAN_REGISTER_SYSTEM(Gp3UiProbeSystem, 5);
