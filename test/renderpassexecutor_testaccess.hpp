#pragma once

#include "../src/core/vkcore/render_pass_executor.hpp"

namespace Pelican::test_support {

struct RenderPassExecutorTestAccess {
    static void startBodyTrace(RenderPassExecutor &executor) {
        executor.body_calls_for_testing.clear();
        executor.trace_bodies_for_testing = true;
    }
    static const auto &bodyCalls(const RenderPassExecutor &executor) {
        return executor.body_calls_for_testing;
    }
};

} // namespace Pelican::test_support

using RenderPassExecutorTestAccess = Pelican::test_support::RenderPassExecutorTestAccess;
