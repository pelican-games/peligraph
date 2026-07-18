#include "../src/core/appflow/framephase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace Pelican {

TEST_CASE("Windowed and RPC frames use the same five-phase executor", "[frame-phase]") {
    const auto observe_order = [] {
        std::vector<FramePhase> observed;
        forEachFramePhase([&](FramePhase phase) { observed.push_back(phase); });
        return observed;
    };

    const auto windowed_order = observe_order();
    const auto rpc_order = observe_order();
    const std::vector<FramePhase> expected{
        FramePhase::freeze_events,
        FramePhase::freeze_input,
        FramePhase::freeze_actions,
        FramePhase::deliver_events,
        FramePhase::update_game,
    };

    REQUIRE(windowed_order == expected);
    REQUIRE(rpc_order == expected);
}

namespace {
void recordEditorBoundary(void *context) noexcept {
    ++*static_cast<int *>(context);
}
}

TEST_CASE("Editor commit queue hook is optional and has one explicit boundary",
          "[frame-phase][editor-commit]") {
    removeEditorCommitQueueHook(nullptr);
    invokeEditorCommitQueueHook();

    int calls = 0;
    REQUIRE(installEditorCommitQueueHook(&calls, &recordEditorBoundary));
    invokeEditorCommitQueueHook();
    REQUIRE(calls == 1);
    removeEditorCommitQueueHook(&calls);
    invokeEditorCommitQueueHook();
    REQUIRE(calls == 1);
}

} // namespace Pelican
